// A live test that the gateway keeps a session alive. It connects, logs in, then polls for a
// while and checks the session is still open at the end. CME terminates a session that stays
// silent for two keep-alive intervals, so if the heartbeat were missing the session would be
// gone well before the run finishes. To make that quick to see, this test asks for a 5-second
// keep-alive interval (the minimum CME allows) and runs long enough to cross several of them.
//
//   ILink3Gateway <settings.json> [marketSegmentId = 99] [seconds = 20]

#include "ILink3Config.hpp"
#include "CmeLogger.hpp"
#include "MarketSegmentGateway.hpp"
#include "Timestamp.hpp"

#include <cstdint>
#include <iostream>
#include <string>

int main(int argc, char** argv)
{
	// Step 1: Read arguments.
	if (argc < 2)
	{
		std::cerr << "usage: ILink3Gateway <settings.json> [marketSegmentId=99] [seconds=20]\n";
		return 2;
	}
	const std::string configPath = argv[1];
	const int32_t marketSegmentId = (argc > 2) ? std::stoi(argv[2]) : 99;
	const int64_t seconds = (argc > 3) ? std::stoll(argv[3]) : 20;

	try
	{
		// Step 2: Load settings, and ask for the shortest keep-alive so the test is quick.
		ILink3::ILink3Config config = ILink3::ILink3Config::Load(configPath);
		config.KeepAliveIntervalMs = 5000;

		// Step 3: Start the background logger for this connection.
		ILink3::CmeLoggerManager loggerManager;
		ILink3::CmeLogger& logger = loggerManager.NewLogger(
			ILink3::CmeLoggerManager::LogDirectory("/mnt/S", config.Environment, marketSegmentId), marketSegmentId);
		loggerManager.Start();

		// Step 4: Build the gateway, connect, and log in.
		ILink3::MarketSegmentGateway gateway(config, marketSegmentId, &logger);
		gateway.Connect();
		std::cout << "Connected to segment " << marketSegmentId << "; logging in...\n";
		if (!gateway.Logon())
		{
			std::cout << "Logon failed.\n";
			loggerManager.Stop();
			return 1;
		}
		std::cout << "Session established (UUID=" << gateway.Uuid() << ", keep-alive "
		          << config.KeepAliveIntervalMs << "ms). Holding for " << seconds << "s...\n";

		// Step 5: Poll the gateway in a loop; report every 5 seconds that the session is still up.
		const int64_t start = Tools::Timestamp::UtcNow().NanosSinceEpoch;
		int64_t nextReport = 5;
		while (gateway.State() == ILink3::SessionState::Established)
		{
			gateway.Poll();
			const int64_t elapsed = (Tools::Timestamp::UtcNow().NanosSinceEpoch - start) / 1'000'000'000LL;
			if (elapsed >= nextReport)
			{
				std::cout << "  ...alive at " << elapsed << "s\n";
				nextReport += 5;
			}
			if (elapsed >= seconds)
				break;
		}

		// Step 6: Report the outcome. Still established = the heartbeat kept it alive.
		const bool stillUp = gateway.State() == ILink3::SessionState::Established;
		std::cout << (stillUp ? "SESSION SURVIVED — keep-alive works.\n" : "SESSION DROPPED — CME closed it.\n");

		// Step 7: Close cleanly and flush the logger.
		gateway.Disconnect();
		loggerManager.Stop();
		if (logger.DroppedCount() > 0)
			std::cout << "WARNING: logger dropped " << logger.DroppedCount() << " entries\n";
		return stillUp ? 0 : 1;
	}
	catch (const std::exception& exception)
	{
		std::cerr << "ERROR: " << exception.what() << "\n";
		return 1;
	}
}
