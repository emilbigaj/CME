// A live test that sends one order and shows CME's response. It logs in, sends a single
// Limit NewOrderSingle, then polls until an execution report comes back and prints it.
//
// This is a round-trip proof, not a real order: without registered party details and a valid
// security id (which come from later steps), CME will reject it — and a decoded reject is
// exactly what proves the outbound order path and the inbound business-message path both work
// end to end. Safe against New Release; it cannot trade for real.
//
//   ILink3Order <settings.json> [marketSegmentId = 99] [securityId = 1]

#include "ILink3Config.hpp"
#include "ILink3Sbe.hpp"
#include "CmeLogger.hpp"
#include "MarketSegmentGateway.hpp"
#include "Wire.hpp"
#include "Timestamp.hpp"

#include <cstdint>
#include <iostream>
#include <string>

int main(int argc, char** argv)
{
	// Step 1: Read arguments.
	if (argc < 2)
	{
		std::cerr << "usage: ILink3Order <settings.json> [marketSegmentId=99] [securityId=1]\n";
		return 2;
	}
	const std::string configPath = argv[1];
	const int32_t marketSegmentId = (argc > 2) ? std::stoi(argv[2]) : 99;
	const int32_t securityId = (argc > 3) ? std::stoi(argv[3]) : 1;

	try
	{
		// Step 2: Load settings and start the background logger.
		ILink3::ILink3Config config = ILink3::ILink3Config::Load(configPath);
		ILink3::CmeLoggerManager loggerManager;
		ILink3::CmeLogger& logger = loggerManager.Create(
			ILink3::CmeLoggerManager::LogDirectory("/mnt/S", config.Environment, marketSegmentId), marketSegmentId);
		loggerManager.Start();

		// Step 3: Build the gateway and print each business reply it hands back.
		ILink3::MarketSegmentGateway gateway(config, marketSegmentId, &logger);
		bool gotReply = false;
		gateway.OnBusinessMessage = [&](const ILink3::FramedMessage& message)
		{
			std::cout << "\nCME replied with " << ILink3::ToObjectType(message.TemplateId) << ":\n"
			          << ILink3::ToJsonLine(message.TemplateId, message.Body) << "\n";
			gotReply = true;
		};

		// Step 4: Connect and log in.
		gateway.Connect();
		if (!gateway.Logon())
		{
			std::cout << "Logon failed.\n";
			loggerManager.Stop();
			return 1;
		}
		std::cout << "Session established. Sending a NewOrderSingle...\n";

		// Step 5: Send one Limit buy: 1 lot at price 1.0 (mantissa = 1 * 10^9).
		ILink3::NewOrderSingle order = ILink3::NewLimitOrder(
			securityId, ILink3::SideReq::Buy, /*quantity*/ 1, /*priceMantissa*/ 1'000'000'000LL,
			/*clOrdId*/ "CELERITY0001", /*senderId*/ "CELERITY",
			/*partyDetailsListReqId*/ 0, /*orderRequestId*/ 1, /*location*/ "NY");
		gateway.SendNewOrderSingle(order);

		// Step 6: Poll until the reply arrives or we give up after ~8 seconds.
		const int64_t start = Tools::Timestamp::UtcNow().NanosSinceEpoch;
		while (!gotReply && gateway.State() == ILink3::SessionState::Established)
		{
			gateway.Poll();
			if ((Tools::Timestamp::UtcNow().NanosSinceEpoch - start) / 1'000'000'000LL >= 8)
			{
				std::cout << "No reply within 8s.\n";
				break;
			}
		}

		// Step 7: Close cleanly and flush the logger.
		gateway.Disconnect();
		loggerManager.Stop();
		return gotReply ? 0 : 1;
	}
	catch (const std::exception& exception)
	{
		std::cerr << "ERROR: " << exception.what() << "\n";
		return 1;
	}
}
