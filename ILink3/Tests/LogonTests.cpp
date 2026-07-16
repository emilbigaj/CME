// A live logon smoke test against a real CME gateway. It loads a settings file, connects to
// one market segment's gateway, and performs the two-step logon handshake:
//   1. Negotiate      -> NegotiationResponse   (identity and signature accepted)
//   2. Establish      -> EstablishmentAck      (sequenced session is now open)
// Both logon messages reuse the same session id, and Establish starts the outbound sequence
// at 1 — a fresh session every run, so nothing needs to be saved between runs. Each sent and
// received message is logged. It never sends an order, so it cannot trade; safe against New
// Release while bringing the protocol up.
//
//   ILink3Logon <settings.json> [marketSegmentId = 99] [primary | secondary]

#include "ILink3Config.hpp"
#include "ILink3Sbe.hpp"
#include "CmeLogger.hpp"
#include "TcpConnection.hpp"
#include "Timestamp.hpp"
#include "Wire.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <vector>

// Connect, log in with Negotiate then Establish, and report each reply.
int main(int argc, char** argv)
{
	// Step 1: Read the command-line arguments (settings file, which segment, which address).
	if (argc < 2)
	{
		std::cerr << "usage: ILink3Logon <settings.json> [marketSegmentId=99] [primary|secondary]\n";
		return 2;
	}
	const std::string configPath = argv[1];
	const int32_t marketSegmentId = (argc > 2) ? std::stoi(argv[2]) : 99;
	const bool useSecondary = (argc > 3) && std::string(argv[3]) == "secondary";

	try
	{
		// Step 2: Load and validate the settings; print them (the secret comes out masked).
		ILink3::ILink3Config config = ILink3::ILink3Config::Load(configPath);
		std::cout << "Loaded config:\n" << config.ToString() << "\n\n";

		// Step 3: Look up the chosen market segment and pick its primary or backup address.
		const ILink3::MarketSegment* segment = config.TryFindMarketSegment(marketSegmentId);
		if (segment == nullptr)
		{
			std::cerr << "market segment " << marketSegmentId << " not in config\n";
			return 2;
		}
		const std::string ip = useSecondary ? segment->SecondaryIPAddress : segment->PrimaryIPAddress;

		// Step 4: Start the background logger for this connection.
		ILink3::CmeLoggerManager loggerManager;
		const std::filesystem::path logDirectory =
			ILink3::CmeLoggerManager::LogDirectory("/mnt/S", config.Environment, marketSegmentId);
		ILink3::CmeLogger& logger = loggerManager.Create(logDirectory, marketSegmentId);
		loggerManager.Start();
		std::cout << "Logging to " << logDirectory.string() << "\n";

		// Step 5: Open the connection to that gateway.
		std::cout << "Connecting to segment " << marketSegmentId << " (" << segment->Description << ") "
		          << (useSecondary ? "secondary" : "primary") << " " << ip << ":" << config.Port << " ...\n";
		ILink3::TcpConnection connection;
		connection.Connect(ip, config.Port);
		std::cout << "Connected.\n";

		// Step 6: Helper to send a framed message and log it strictly after the send returns.
		std::array<uint8_t, 256> sendBuffer{};
		auto sendMessage = [&](size_t length, const char* label)
		{
			std::cout << "Sending " << label << ": " << length << " bytes\n";
			connection.SendAll(std::span<const uint8_t>(sendBuffer.data(), length));
			const int64_t stamp = Tools::Timestamp::UtcNow().NanosSinceEpoch;
			logger.Log(ILink3::Direction::Send, stamp, 0, std::span<const uint8_t>(sendBuffer.data(), length));
		};

		// Step 7: Helper to receive the next whole message, log it, and hand it back. Uses a
		// growing buffer plus a read offset, so a message's pointer stays valid across calls.
		std::vector<uint8_t> recvBuffer;
		size_t consumed = 0;
		std::array<uint8_t, 4096> chunk{};
		auto receiveMessage = [&](ILink3::FramedMessage& message) -> bool
		{
			int64_t stamp = 0;
			for (int attempt = 0; attempt < 5; ++attempt)
			{
				// Frame from the unread part of the buffer; a whole message may already be there.
				if (ILink3::TryFrame(std::span<const uint8_t>(recvBuffer).subspan(consumed), message))
				{
					logger.Log(ILink3::Direction::Recv, stamp,
						0, std::span<const uint8_t>(recvBuffer.data() + consumed, message.TotalLength));
					consumed += message.TotalLength;
					return true;
				}
				ssize_t n = connection.Recv(chunk);
				if (n == 0)
				{
					std::cout << "Peer closed the connection.\n";
					return false;
				}
				if (n < 0)
				{
					std::cout << "Receive timeout, retrying...\n";
					continue;
				}
				stamp = Tools::Timestamp::UtcNow().NanosSinceEpoch;
				recvBuffer.insert(recvBuffer.end(), chunk.begin(), chunk.begin() + n);
			}
			std::cout << "No complete message received.\n";
			return false;
		};

		// Step 8: The whole run's result, so the logger can flush before we exit.
		int result = 1;
		ILink3::FramedMessage message{};

		// Step 9: Negotiate. Reuse this session id for the Establish that follows.
		const uint64_t uuid = ILink3::MakeUuid();
		sendMessage(ILink3::EncodeNegotiate(config, uuid, ILink3::RequestTimestampNow(), sendBuffer), "Negotiate(500)");
		if (receiveMessage(message))
		{
			if (message.TemplateId == ILink3::NegotiationResponse::TemplateId)
			{
				std::cout << "NEGOTIATION RESPONSE (accepted):\n"
				          << message.As<ILink3::NegotiationResponse>()->ToString() << "\n\n";

				// Step 10: Establish, starting the outbound sequence at 1 (fresh session).
				sendMessage(ILink3::EncodeEstablish(config, uuid, ILink3::RequestTimestampNow(), 1, sendBuffer), "Establish(503)");
				if (receiveMessage(message))
				{
					std::cout << "Received template " << message.TemplateId << ":\n";
					if (message.TemplateId == ILink3::EstablishmentAck::TemplateId)
					{
						std::cout << "ESTABLISHMENT ACK (session open):\n"
						          << message.As<ILink3::EstablishmentAck>()->ToString() << "\n";
						result = 0;
					}
					else if (message.TemplateId == ILink3::EstablishmentReject::TemplateId)
					{
						std::cout << "ESTABLISHMENT REJECT:\n"
						          << message.As<ILink3::EstablishmentReject>()->ToString() << "\n";
					}
					else if (message.TemplateId == ILink3::Terminate::TemplateId)
					{
						std::cout << "TERMINATE:\n" << message.As<ILink3::Terminate>()->ToString() << "\n";
					}
					else
					{
						std::cout << "Unexpected reply to Establish.\n";
					}
				}
			}
			else if (message.TemplateId == ILink3::NegotiationReject::TemplateId)
			{
				std::cout << "NEGOTIATION REJECT:\n"
				          << message.As<ILink3::NegotiationReject>()->ToString() << "\n";
			}
			else
			{
				std::cout << "Unexpected reply to Negotiate (template " << message.TemplateId << ").\n";
			}
		}

		// Step 11: Stop the logger (flushes everything queued) before exiting.
		loggerManager.Stop();
		if (logger.DroppedCount() > 0)
			std::cout << "WARNING: logger dropped " << logger.DroppedCount() << " entries\n";
		return result;
	}
	catch (const std::exception& exception)
	{
		std::cerr << "ERROR: " << exception.what() << "\n";
		return 1;
	}
}
