// Loads a CME secdef file, reports how many futures and calendar spreads it holds, and lists the
// nearest-expiry contracts for one product root so we can pick a real SecurityID (and its market
// segment) to trade. Offline; reads the file only.
//
//   SecDefTests <secdef.dat> [root = ES]

#include "SecDefFile.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		std::cerr << "usage: SecDefTests <secdef.dat> [root=ES]\n";
		return 2;
	}
	const std::string root = (argc > 2) ? argv[2] : "ES";

	try
	{
		// Step 1: Load and report the totals.
		SecDef::SecDefFile file = SecDef::SecDefFile::Load(argv[1]);
		std::cout << "Loaded " << file.FutureCount() << " futures and " << file.SpreadCount()
		          << " one-to-one calendar spreads.\n\n";

		// Step 2: Collect this root's futures and sort them by expiry (nearest first).
		std::vector<const SecDef::Loaded*> futures;
		for (const SecDef::Loaded& instrument : file.Instruments())
		{
			const Data::InstrumentHeader& header = instrument.Header.AsInstrumentHeader();
			if (header.InstrumentType == Data::InstrumentType::Future && header.Root.ToString() == root)
				futures.push_back(&instrument);
		}
		std::sort(futures.begin(), futures.end(), [](const SecDef::Loaded* a, const SecDef::Loaded* b)
		{
			return a->Header.AsFuture().ExpiryDate.NanosSinceEpoch < b->Header.AsFuture().ExpiryDate.NanosSinceEpoch;
		});

		// Step 3: Show the front few, with the ids and price scale an order needs.
		std::cout << root << ": " << futures.size() << " futures. Nearest expiries:\n";
		for (size_t i = 0; i < futures.size() && i < 6; ++i)
		{
			const SecDef::Loaded& f = *futures[i];
			const Data::FutureHeader& fh = f.Header.AsFuture();
			std::cout << "  SecurityID=" << f.SecurityID
			          << "  segment=" << f.MarketSegmentID
			          << "  expiry=" << fh.ExpiryDate.ToDateString()
			          << "  tick=" << fh.InstrumentHeader.TickSize
			          << "  mult=" << fh.Multiplier
			          << "  band=[" << f.LowLimitPrice << ", " << f.HighLimitPrice << "]"
			          << "  ref=" << f.ReferencePrice << "\n";
		}

		// Step 4: Show one calendar spread on this root, with its two legs.
		for (const SecDef::Loaded& instrument : file.Instruments())
		{
			const Data::InstrumentHeader& header = instrument.Header.AsInstrumentHeader();
			if (header.InstrumentType == Data::InstrumentType::Spread && header.Root.ToString() == root)
			{
				const Data::SpreadHeader& sh = instrument.Header.AsSpread();
				std::cout << "\nexample " << root << " calendar spread: SecurityID=" << instrument.SecurityID
				          << "  segment=" << instrument.MarketSegmentID
				          << "  longLeg=" << instrument.LongLegSecurityID << " (" << sh.LongExpiryDate.ToDateString() << ")"
				          << "  shortLeg=" << instrument.ShortLegSecurityID << " (" << sh.ShortExpiryDate.ToDateString() << ")\n";
				break;
			}
		}

		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "error: " << error.what() << "\n";
		return 1;
	}
}
