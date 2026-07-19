// A live test of the whole trading-server seam. It starts the CME server (catalog from the
// security-definition file, iLink3 session to New Release), then plays the strategy side over
// the real wire protocol — the same shared-memory messages the C# client sends: connect, get a
// client id, allocate an instrument, then write an order target and read the order states that
// come back. The order walks create -> resting -> cancel -> done, every hop crossing the seam:
// strategy channel -> server -> router -> CME -> router -> server -> strategy channel.
//
// The server side normally runs beside a logging server; here a stub accepts its connections.
//
//   CmeServerTests <settings.json> <secdef.dat> <config.xml> <securityId>

#include "CmeServer.hpp"
#include "ILink3Config.hpp"
#include "SecDefFile.hpp"
#include "Allocate.hpp"
#include "Context.hpp"
#include "Order.hpp"
#include "OrderIdAllocator.hpp"
#include "SharedArray.hpp"
#include "Socket.hpp"
#include "Timestamp.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>

// Blocking read of one message from a client-socket channel, with a time limit.
static bool ReadOne(Socket::ClientSocket& socket, int32_t channelId, std::span<const uint8_t>& message, int seconds)
{
	const int64_t start = Tools::Timestamp::UtcNow().NanosSinceEpoch;
	while (socket.TryRead(channelId, message) != Socket::ReadStatus::New)
	{
		if ((Tools::Timestamp::UtcNow().NanosSinceEpoch - start) / 1'000'000'000LL >= seconds)
			return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return true;
}

int main(int argc, char** argv)
{
	if (argc < 5)
	{
		std::cerr << "usage: CmeServerTests <settings.json> <secdef.dat> <config.xml> <securityId>\n";
		return 2;
	}
	const int32_t securityId = std::stoi(argv[4]);

	try
	{
		// Step 1: The pieces the server needs: settings, the instrument file, and a stub logging
		// server accepting the connections a real logging server normally takes.
		ILink3::ILink3Config config = ILink3::ILink3Config::Load(argv[1]);
		SecDef::SecDefFile secdef = SecDef::SecDefFile::Load(argv[2]);
		Mdp3::ChannelConfig channels = Mdp3::ChannelConfig::Load(argv[3]);
		const std::filesystem::path serverDirectory = Provider::ServerContext::GetDirectoryPath("CME");
		Socket::ServerSocket loggingStub(Provider::Context::GetLoggingServerDirectoryPath(serverDirectory).string(), 8);
		loggingStub.Listen();
		// Stop the stub's listen thread on every exit path — a joinable thread in a destructor
		// otherwise terminates the process during unwind.
		struct StubGuard { Socket::ServerSocket& Stub; ~StubGuard() { Stub.Dispose(); } } stubGuard{loggingStub};

		// Step 2: Start the CME server: the E-mini S&P segment on core group 2.
		Server::CmeServerConfig serverConfig;
		serverConfig.ServerName = "CME";
		serverConfig.MarketDataInterfaceIp = "10.210.19.34";
		Server::MarketSegments marketSegments;
		marketSegments.Segments = {Server::MarketSegment{/*MarketSegmentID*/ 64, /*Channel*/ 310,
			/*CoreGroupId*/ 2, /*MarketDataCore*/ 8, /*ExecutionCore*/ 9}};
		Server::CmeServer cme(serverConfig, marketSegments, config, secdef, channels);
		const int32_t instrumentHeaderId = cme.FindInstrumentHeaderId(securityId);
		if (instrumentHeaderId < 0)
		{
			std::cerr << "SecurityID " << securityId << " is not in the committed catalog.\n";
			return 1;
		}
		cme.Start();
		std::cout << "Server started; sessions open." << std::endl;

		// Step 3: Connect as a strategy over the real wire protocol and receive our client id.
		Tools::Bitset64 coreGroupIds;
		coreGroupIds.Set(2);
		const std::vector<int32_t> channelLengths = Socket::SocketChannel::BuildChannelLengths(coreGroupIds);
		const std::string clientName = Provider::ClientContext::GetDirectoryPath("CmeSeamTest").string();
		Socket::ClientSocket socket(clientName, serverDirectory.string(), channelLengths, channelLengths);
		socket.Connect();
		std::span<const uint8_t> message;
		if (!ReadOne(socket, Socket::SocketChannel::Admin, message, 5))
			throw std::runtime_error("no client-allocation reply");
		const int32_t clientId = reinterpret_cast<const Provider::AllocateClient*>(message.data())->ClientId;
		std::cout << "Connected as client " << clientId << "." << std::endl;

		// Step 4: Allocate the instrument and receive its compact instrument id.
		Provider::AllocateInstrument allocate{};
		allocate.ClientId = clientId;
		allocate.InstrumentHeaderId = instrumentHeaderId;
		socket.Write(Socket::SocketChannel::Admin, allocate);
		if (!ReadOne(socket, Socket::SocketChannel::Admin, message, 5))
			throw std::runtime_error("no instrument-allocation reply");
		const Provider::AllocateInstrument& allocated = *reinterpret_cast<const Provider::AllocateInstrument*>(message.data());
		const int32_t instrumentId = allocated.InstrumentId;
		std::cout << "Allocated instrument " << instrumentId << " (" << allocated.Symbol.ToString() << ")." << std::endl;

		// Step 4b: Set the algo live — the risk layer discards algo orders while it is paused
		// (the default). The server echoes the position back on the segment channel; wait for it
		// so the order that follows is validated against the live status.
		Provider::ControlAlgoStatus algoLive{};
		algoLive.ClientId = clientId;
		algoLive.StrategyId = clientId;
		algoLive.InstrumentId = instrumentId;
		algoLive.AlgoStatus = Execution::AlgoStatus::Live;
		socket.Write(Socket::SocketChannel::Admin, algoLive);
		if (!ReadOne(socket, /*channel*/ 2, message, 5))
			throw std::runtime_error("no algo-status reply");
		std::cout << "Algo set live." << std::endl;

		// Step 5: A resting buy a few ticks below the settle anchor, as the strategy would send
		// it: claim a client order id, publish the target to the shared order-target slot, then
		// send it on the segment's channel.
		const SecDef::Loaded& instrument = *secdef.FindBySecurityId(securityId);
		const double tickSize = instrument.Header.AsInstrumentHeader().TickSize;
		const int32_t ticks = static_cast<int32_t>(std::floor(instrument.ReferencePrice / tickSize)) - 4;

		const int32_t ordersCapacity = 	Provider::ServerHeader{}.OrdersPerClient * 64;
		Socket::SharedArray<Execution::OrderTarget> orderTargets(serverDirectory.string() + "OrderTargets", ordersCapacity, Tools::Access::Write, false);

		Tools::Bitset64 isOrderActive;
		Execution::OrderTarget target{};
		target.OrderHeader.ClientId = clientId;
		target.OrderHeader.StrategyId = clientId;
		target.OrderHeader.InstrumentId = instrumentId;
		target.OrderHeader.Seq = 1;
		target.OrderTargetAction = Execution::OrderTargetAction::Create;
		target.OrderTargetStatus = Execution::OrderStateStatus::Active;
		target.OrderProfile.Ticks = ticks;
		target.OrderProfile.Quantity = 1;
		if (!Execution::OrderIdAllocator::TryAllocate(isOrderActive, clientId, instrumentId, target.OrderHeader.ClientOrderId))
			throw std::runtime_error("could not allocate a client order id");
		const int32_t globalOrderIndex = target.OrderHeader.GlobalOrderIndex();
		orderTargets[globalOrderIndex].Write(target);
		socket.Write(/*channel*/ 2, target);
		std::cout << "Sent create: buy 1 @ ticks " << ticks << " (" << ticks * tickSize << ")." << std::endl;

		// Step 6: Read the order states back: the create acknowledgment (revision 0), then the
		// exchange acceptance (revision 1) — cancel on that — then done after the cancel.
		bool cancelSent = false, done = false;
		const int64_t start = Tools::Timestamp::UtcNow().NanosSinceEpoch;
		while (!done && (Tools::Timestamp::UtcNow().NanosSinceEpoch - start) / 1'000'000'000LL < 15)
		{
			if (!ReadOne(socket, /*channel*/ 2, message, 15))
				break;
			switch (message[0])
			{
				case static_cast<uint8_t>(Execution::OrderType::OrderState):
				{
					const Execution::OrderState& state = *reinterpret_cast<const Execution::OrderState*>(message.data());
					std::cout << "\n-> OrderState: " << state.ToString() << std::endl;
					if (state.OrderStateStatus == Execution::OrderStateStatus::Done)
						done = true;
					else if (state.OrderHeader.Seq >= 1 && !cancelSent)
					{
						// A cancel carries the empty cancel profile — a next-revision target with
						// the same profile as the working state reads as a no-op and is discarded.
						target.OrderTargetAction = Execution::OrderTargetAction::Cancel;
						target.OrderHeader.Seq = 2;
						target.OrderProfile = Execution::OrderProfile::Cancel();
						orderTargets[globalOrderIndex].Write(target);
						socket.Write(/*channel*/ 2, target);
						cancelSent = true;
						std::cout << "Sent cancel." << std::endl;
					}
					break;
				}
				case static_cast<uint8_t>(Execution::OrderType::OrderRejected):
				{
					const Execution::OrderRejected& rejected = *reinterpret_cast<const Execution::OrderRejected*>(message.data());
					std::cout << "\n-> OrderRejected: " << rejected.ToString() << std::endl;
					done = true;
					break;
				}
				default:
					break;   // positions and fills are not part of this walk
			}
		}

		// Step 7: Shut down.
		socket.Close();
		socket.Dispose();
		cme.Stop();
		std::cout << (done ? "\nSeam round trip complete.\n" : "\nTimed out waiting for the round trip.\n");
		return done ? 0 : 1;
	}
	catch (const std::exception& error)
	{
		std::cerr << "error: " << error.what() << "\n";
		return 1;
	}
}
