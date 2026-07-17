// A live test of the InstrumentRouter: it feeds the router a server-style order intent (in ticks
// and lots) and lets the router turn it into an iLink3 order, send it, and reconcile CME's reply
// back into a server order event. It looks the instrument up in the secdef for its segment and
// price scale, and prices a resting buy near the prior settle so the order rests rather than
// fills.
//
//   ILink3Router <settings.json> <secdef.dat> <securityId>

#include "ILink3Config.hpp"
#include "CmeLogger.hpp"
#include "MarketSegmentGateway.hpp"
#include "InstrumentRouter.hpp"
#include "SecDefFile.hpp"
#include "Order.hpp"
#include "Timestamp.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>

int main(int argc, char** argv)
{
	if (argc < 4)
	{
		std::cerr << "usage: ILink3Router <settings.json> <secdef.dat> <securityId>\n";
		return 2;
	}
	const std::string configPath = argv[1];
	const std::string secdefPath = argv[2];
	const int32_t securityId = std::stoi(argv[3]);

	try
	{
		// Step 1: Look the instrument up for its segment, tick, display factor, and settle anchor.
		SecDef::SecDefFile secdef = SecDef::SecDefFile::Load(secdefPath);
		const SecDef::Loaded* instrument = secdef.FindBySecurityId(securityId);
		if (instrument == nullptr)
		{
			std::cerr << "SecurityID " << securityId << " not found in secdef.\n";
			return 1;
		}
		const int32_t marketSegmentId = instrument->MarketSegmentID;
		const double tickSize = instrument->Header.AsInstrumentHeader().TickSize;
		const double anchor = instrument->ReferencePrice > 0.0
			? instrument->ReferencePrice
			: (instrument->LowLimitPrice + instrument->HighLimitPrice) / 2.0;
		// The server thinks in ticks: a resting buy four ticks below the settle anchor.
		const int32_t ticks = static_cast<int32_t>(std::floor(anchor / tickSize)) - 4;

		// Step 2: Load settings, start the logger, build the gateway.
		ILink3::ILink3Config config = ILink3::ILink3Config::Load(configPath);
		ILink3::CmeLoggerManager loggerManager;
		ILink3::CmeLogger& logger = loggerManager.NewLogger(
			ILink3::CmeLoggerManager::LogDirectory("/mnt/S", config.Environment, marketSegmentId), marketSegmentId);
		loggerManager.Start();
		ILink3::MarketSegmentGateway gateway(config, marketSegmentId, &logger);

		// Step 3: Build the router for this instrument and print whatever it publishes.
		// Capacity covers the full global-index space the packed id can carry (the test id below
		// is a timestamp, so its index bits are arbitrary).
		ILink3::InstrumentRouter router(/*instrumentId*/ 0, securityId, &gateway, tickSize,
			instrument->DisplayFactor, config.Parties.Operator, config.Parties.Location,
			/*ordersCapacity*/ 1 << 16);
		bool isResting = false;
		router.OnOrderState = [&](const Execution::OrderState& state)
		{
			std::cout << "\n-> OrderState: " << state.ToString() << "\n";
			isResting = state.OrderStateStatus == Execution::OrderStateStatus::Active;
		};
		router.OnOrderRejected = [](const Execution::OrderRejected& rejected, const std::string& text)
		{
			std::cout << "\n-> OrderRejected (" << text << "): " << rejected.ToString() << "\n";
		};

		// Step 4: Feed every execution report the gateway hands back into the router.
		gateway.OnBusinessMessage = [&](const ILink3::FramedMessage& message)
		{
			router.OnExecutionReport(message);
		};

		// Step 5: Log in.
		gateway.Connect();
		if (!gateway.Logon())
		{
			std::cout << "Logon failed.\n";
			loggerManager.Stop();
			return 1;
		}

		// Step 6: Hand the router a create intent (buy 1 lot at the resting tick).
		Execution::OrderTarget target{};
		target.OrderTargetAction = Execution::OrderTargetAction::Create;
		target.OrderTargetStatus = Execution::OrderStateStatus::Active;
		// A unique id per run (the exchange rejects a duplicate ClOrdID within a session/day).
		target.OrderHeader.ClientOrderId = static_cast<uint64_t>(Tools::Timestamp::UtcNow().NanosSinceEpoch);
		target.OrderHeader.InstrumentId = 0;
		target.OrderHeader.Seq = 0;
		target.OrderProfile.Ticks = ticks;
		target.OrderProfile.Quantity = 1;   // positive quantity = buy
		std::cout << "Routing create: buy 1 @ ticks " << ticks << " (" << ticks * tickSize << ")\n";
		router.OnOrderTarget(target);

		// Step 7: Poll for the accept, then route a cancel for the same order and watch it go done.
		bool cancelSent = false;
		const int64_t start = Tools::Timestamp::UtcNow().NanosSinceEpoch;
		while (gateway.State() == ILink3::SessionState::Established)
		{
			gateway.Poll();
			if (isResting && !cancelSent)
			{
				Execution::OrderTarget cancel = target;
				cancel.OrderTargetAction = Execution::OrderTargetAction::Cancel;
				cancel.OrderHeader.Seq = 1;
				std::cout << "Routing cancel for order " << cancel.OrderHeader.ClientOrderId << "\n";
				router.OnOrderTarget(cancel);
				cancelSent = true;
			}
			if ((Tools::Timestamp::UtcNow().NanosSinceEpoch - start) / 1'000'000'000LL >= 6)
				break;
		}

		gateway.Disconnect();
		loggerManager.Stop();
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "error: " << error.what() << "\n";
		return 1;
	}
}
