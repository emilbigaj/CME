// A live test that sends one real order and shows CME's response. It looks the instrument up in
// the security-definition file to get its market segment and today's price band, logs in to that
// segment, and sends a Limit buy priced just above the day's low limit — a valid price far below
// the market, so the order rests rather than fills. It then polls for the execution report.
//
//   ILink3Order <settings.json> <secdef.dat> <securityId>

#include "ILink3Config.hpp"
#include "ILink3Sbe.hpp"
#include "CmeLogger.hpp"
#include "MarketSegmentGateway.hpp"
#include "Wire.hpp"
#include "Timestamp.hpp"
#include "SecDefFile.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>

int main(int argc, char** argv)
{
	// Step 1: Read arguments.
	if (argc < 4)
	{
		std::cerr << "usage: ILink3Order <settings.json> <secdef.dat> <securityId>\n";
		return 2;
	}
	const std::string configPath = argv[1];
	const std::string secdefPath = argv[2];
	const int32_t securityId = std::stoi(argv[3]);

	try
	{
		// Step 2a: Look the instrument up to get its market segment and today's price band.
		SecDef::SecDefFile secdef = SecDef::SecDefFile::Load(secdefPath);
		const SecDef::Loaded* instrument = secdef.FindBySecurityId(securityId);
		if (instrument == nullptr)
		{
			std::cerr << "SecurityID " << securityId << " not found in secdef.\n";
			return 1;
		}
		const int32_t marketSegmentId = instrument->MarketSegmentID;
		const double tickSize = instrument->Header.AsInstrumentHeader().TickSize;
		// A resting buy a few ticks below the prior-settle anchor: close enough to sit inside CME's
		// dynamic price band (which is far tighter than the day's static limits), but below the
		// market so it rests rather than fills.
		const double anchor = instrument->ReferencePrice > 0.0
			? instrument->ReferencePrice
			: (instrument->LowLimitPrice + instrument->HighLimitPrice) / 2.0;
		const double priceDisplay = (std::floor(anchor / tickSize) - 4.0) * tickSize;
		// The wire carries the Globex price (conventional / DisplayFactor) as a PRICE9 mantissa.
		const int64_t globexPrice = std::llround(priceDisplay / instrument->DisplayFactor);
		const int64_t priceMantissa = globexPrice * 1'000'000'000LL;
		std::cout << "Instrument " << securityId << " on segment " << marketSegmentId
		          << ": resting buy at " << priceDisplay << " (globex " << globexPrice
		          << ", ref " << instrument->ReferencePrice
		          << ", band [" << instrument->LowLimitPrice << ", " << instrument->HighLimitPrice << "])\n";

		// Step 2b: Load settings and start the background logger.
		ILink3::ILink3Config config = ILink3::ILink3Config::Load(configPath);
		ILink3::CmeLoggerManager loggerManager;
		ILink3::CmeLogger& logger = loggerManager.NewLogger(
			ILink3::CmeLoggerManager::LogDirectory("/mnt/S", config.Environment, marketSegmentId), marketSegmentId);
		loggerManager.Start();

		// Step 3: Build the gateway and print each business reply it hands back. On-demand sends
		// two messages (party details then order), so CME may reply with two — print them all.
		ILink3::MarketSegmentGateway gateway(config, marketSegmentId, &logger);
		int replyCount = 0;
		gateway.OnBusinessMessage = [&](const ILink3::FramedMessage& message)
		{
			std::cout << "\nCME replied with " << ILink3::ToObjectType(message.TemplateId) << ":\n"
			          << ILink3::ToJsonLine(message.TemplateId, message.Body) << "\n";
			++replyCount;
		};

		// Step 4: Connect and log in.
		gateway.Connect();
		if (!gateway.Logon())
		{
			std::cout << "Logon failed.\n";
			loggerManager.Stop();
			return 1;
		}
		std::cout << "Session established. Sending a NewOrderSingle (on-demand parties)...\n";

		// Step 5: Send one Limit buy of 1 lot at the resting price. The gateway sends the party
		// details (id 0) immediately before the order and references id 0 on it; the caller
		// supplies the desk sender id and location from config.
		ILink3::NewOrderSingle order = ILink3::NewLimitOrder(
			securityId, ILink3::SideReq::Buy, /*quantity*/ 1, priceMantissa,
			/*clOrdId*/ "CELERITY0001", /*senderId*/ config.Parties.Operator,
			/*partyDetailsListReqId*/ 0, /*orderRequestId*/ 1, /*location*/ config.Parties.Location);
		gateway.SendNewOrderSingle(order);

		// Step 6: Poll for a fixed few seconds so both replies (party details + order) arrive.
		const int64_t start = Tools::Timestamp::UtcNow().NanosSinceEpoch;
		while (gateway.State() == ILink3::SessionState::Established)
		{
			gateway.Poll();
			if ((Tools::Timestamp::UtcNow().NanosSinceEpoch - start) / 1'000'000'000LL >= 5)
				break;
		}
		if (replyCount == 0)
			std::cout << "No reply within 5s.\n";

		// Step 7: Close cleanly and flush the logger.
		gateway.Disconnect();
		loggerManager.Stop();
		return replyCount > 0 ? 0 : 1;
	}
	catch (const std::exception& exception)
	{
		std::cerr << "ERROR: " << exception.what() << "\n";
		return 1;
	}
}
