// Diagnostic: log in once, then send several PartyDetailsDefinitionRequests with different
// party ids to see which values CME accepts. The key comparison is id 0 (the on-demand value)
// against non-zero ids (the pre-registration value); every non-zero id has so far been rejected
// as "out of range", so this tells us which flow the session is provisioned for. No trading.
//
//   ILink3ProbeParty <settings.json> [marketSegmentId = 99]

#include "ILink3Config.hpp"
#include "CmeLogger.hpp"
#include "MarketSegmentGateway.hpp"

#include <cstdint>
#include <iostream>
#include <string>

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		std::cerr << "usage: ILink3ProbeParty <settings.json> [marketSegmentId=99]\n";
		return 2;
	}
	const std::string configPath = argv[1];
	const int32_t marketSegmentId = (argc > 2) ? std::stoi(argv[2]) : 99;

	try
	{
		// Step 1: Load settings and start the logger.
		ILink3::ILink3Config config = ILink3::ILink3Config::Load(configPath);
		ILink3::CmeLoggerManager loggerManager;
		ILink3::CmeLogger& logger = loggerManager.NewLogger(
			ILink3::CmeLoggerManager::LogDirectory("/mnt/S", config.Environment, marketSegmentId), marketSegmentId);
		loggerManager.Start();

		// Step 2: Log in.
		ILink3::MarketSegmentGateway gateway(config, marketSegmentId, &logger);
		gateway.Connect();
		if (!gateway.Logon())
		{
			std::cout << "Logon failed.\n";
			loggerManager.Stop();
			return 1;
		}
		std::cout << "Session established. Probing PartyDetailsListReqID values:\n";

		// Step 3: Probe a spread of ids, starting with 0 (on-demand) then non-zero values.
		const uint64_t ids[] = {0ULL, 1ULL, 1000000000ULL, 4294967295ULL};
		for (uint64_t id : ids)
		{
			if (gateway.State() != ILink3::SessionState::Established)
			{
				std::cout << "  session dropped before reqId=" << id << "\n";
				break;
			}
			gateway.ProbePartyDetails(id);
		}

		// Step 4: Close cleanly.
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
