// A live book for one instrument: looks the instrument up in the security definitions (its
// market-data channel and price scale), joins that channel's incremental feed, runs the book
// builder, and applies its update ticks to a real server-side book — printing the top of book
// as it moves, and every trade. This is the whole market-data path short of the trading
// server: feed -> packet walker -> book builder -> market-by-price book.
//
//   Mdp3Book <config.xml> <secdef.dat> <securityId> [interfaceIp=10.210.19.34] [seconds=20]

#include "BookBuilder.hpp"
#include "ChannelConfig.hpp"
#include "UdpReceiver.hpp"
#include "SecDefFile.hpp"
#include "MarketByPrice.hpp"   // Data::MarketByPrice64 (the server-side book)
#include "Timestamp.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <string>

int main(int argc, char** argv)
{
	if (argc < 4)
	{
		std::cerr << "usage: Mdp3Book <config.xml> <secdef.dat> <securityId> [interfaceIp=10.210.19.34] [seconds=20]\n";
		return 2;
	}
	const int32_t securityId = std::stoi(argv[3]);
	const std::string interfaceIp = (argc > 4) ? argv[4] : "10.210.19.34";
	const int seconds = (argc > 5) ? std::stoi(argv[5]) : 20;

	try
	{
		// Step 1: The instrument tells us its channel and price scale.
		SecDef::SecDefFile secdef = SecDef::SecDefFile::Load(argv[2]);
		const SecDef::Loaded* instrument = secdef.FindBySecurityId(securityId);
		if (instrument == nullptr)
		{
			std::cerr << "SecurityID " << securityId << " not found in secdef.\n";
			return 1;
		}
		const double tickSize = instrument->Header.AsInstrumentHeader().TickSize;
		std::cout << "Instrument " << securityId << " on market-data channel " << instrument->Channel
		          << ", tick " << tickSize << "\n";

		// Step 2: Join the channel's incremental feed.
		Mdp3::ChannelConfig config = Mdp3::ChannelConfig::Load(argv[1]);
		const Mdp3::Channel* channel = config.FindChannel(instrument->Channel);
		if (channel == nullptr)
		{
			std::cerr << "channel " << instrument->Channel << " not in config.\n";
			return 1;
		}
		const Mdp3::Connection* feed = channel->Find("I", 'A');
		Mdp3::UdpReceiver receiver;
		receiver.Join(feed->Ip, feed->Port, interfaceIp);
		std::cout << "Joined " << feed->Id << " (" << feed->Ip << ":" << feed->Port << ") on "
		          << interfaceIp << "; listening " << seconds << "s...\n\n";

		// Step 3: The builder feeds a real server-side book; print the top when it moves.
		Data::MarketByPrice64 book;
		Data::Level lastBid{}, lastAsk{};
		uint64_t updates = 0, trades = 0;
		Mdp3::BookBuilder builder;
		builder.Subscribe(securityId, /*instrumentId*/ 0, tickSize, instrument->DisplayFactor);
		builder.OnMarketByPrice = [&](Data::MarketByPrice&, std::span<uint8_t> span)
		{
			++updates;
			book.TrySet(span);
			const Data::Level bid = book.BestBid();
			const Data::Level ask = book.BestAsk();
			if (bid.Ticks != lastBid.Ticks || bid.Quantity != lastBid.Quantity
			 || ask.Ticks != lastAsk.Ticks || ask.Quantity != lastAsk.Quantity)
			{
				std::cout << "book  " << bid.Quantity << " x " << bid.Ticks * tickSize
				          << "  |  " << ask.Ticks * tickSize << " x " << ask.Quantity << "\n";
				lastBid = bid;
				lastAsk = ask;
			}
		};
		builder.OnTrade = [&](const Data::Trade& trade)
		{
			++trades;
			std::cout << "TRADE " << trade.Level.Quantity << " @ " << trade.Level.Ticks * tickSize
			          << (trade.Direction > 0 ? "  (buyer)" : trade.Direction < 0 ? "  (seller)" : "") << "\n";
		};

		// Step 4: Pump the feed into the builder for the window.
		std::array<uint8_t, 65536> buffer{};
		const int64_t deadline = Tools::Timestamp::UtcNow().NanosSinceEpoch + static_cast<int64_t>(seconds) * 1'000'000'000LL;
		while (Tools::Timestamp::UtcNow().NanosSinceEpoch < deadline)
		{
			const ssize_t n = receiver.Recv(buffer);
			if (n <= 0)
				continue;
			builder.OnPacket(std::span<const uint8_t>(buffer.data(), static_cast<size_t>(n)), Tools::Timestamp::UtcNow());
		}

		// Step 5: The tallies the recovery layer will care about.
		std::cout << "\n---- " << updates << " book updates, " << trades << " trades, "
		          << builder.RptSeqGaps << " update-sequence gaps, "
		          << builder.UnhandledActions << " unhandled actions ----\n";
		return updates > 0 ? 0 : 1;
	}
	catch (const std::exception& error)
	{
		std::cerr << "error: " << error.what() << "\n";
		return 1;
	}
}
