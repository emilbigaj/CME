// Live decode of one market-data feed: joins a feed, walks every datagram's messages, prints
// the first few in full (decoded to JSON, repeating groups included), then keeps a tally. The
// summary shows which message kinds the channel actually publishes, and whether the packet
// sequence ran gap-free — the two facts the book builder needs to be sure of the walker.
//
//   Mdp3Dump <config.xml> [channelId=310] [feed=IA] [interfaceIp=10.210.19.34] [seconds=10]

#include "ChannelConfig.hpp"
#include "PacketWalker.hpp"
#include "UdpReceiver.hpp"
#include "Mdp3Sbe.hpp"
#include "Timestamp.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <map>
#include <string>

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		std::cerr << "usage: Mdp3Dump <config.xml> [channelId=310] [feed=IA] [interfaceIp=10.210.19.34] [seconds=10]\n";
		return 2;
	}
	const int32_t channelId = (argc > 2) ? std::stoi(argv[2]) : 310;
	const std::string feedPick = (argc > 3) ? argv[3] : "IA";
	const std::string interfaceIp = (argc > 4) ? argv[4] : "10.210.19.34";
	const int seconds = (argc > 5) ? std::stoi(argv[5]) : 10;

	try
	{
		// Step 1: Find the requested feed of the channel.
		Mdp3::ChannelConfig config = Mdp3::ChannelConfig::Load(argv[1]);
		const Mdp3::Channel* channel = config.FindChannel(channelId);
		if (channel == nullptr)
		{
			std::cerr << "channel " << channelId << " not in config.\n";
			return 1;
		}
		const Mdp3::Connection* feed = channel->Find(feedPick.substr(0, feedPick.size() - 1), feedPick.back());
		if (feed == nullptr || !feed->IsUdp())
		{
			std::cerr << "feed " << feedPick << " not on channel " << channelId << ".\n";
			return 1;
		}

		// Step 2: Join it.
		Mdp3::UdpReceiver receiver;
		receiver.Join(feed->GroupIp, feed->Port, interfaceIp);
		std::cout << "Joined " << feed->Id << " (" << feed->GroupIp << ":" << feed->Port << ") on "
		          << interfaceIp << "; listening " << seconds << "s...\n\n";

		// Step 3: Walk every packet: print the first few messages whole, tally the rest.
		std::array<uint8_t, 65536> buffer{};
		std::map<uint16_t, uint64_t> countByTemplate;
		uint64_t packets = 0, messages = 0, gaps = 0;
		uint32_t nextSeqNum = 0;
		int printed = 0;
		const int64_t deadline = Tools::Timestamp::UtcNow().NanosSinceEpoch + static_cast<int64_t>(seconds) * 1'000'000'000LL;
		while (Tools::Timestamp::UtcNow().NanosSinceEpoch < deadline)
		{
			const ssize_t n = receiver.Recv(buffer);
			if (n <= 0)
				continue;

			Mdp3::PacketWalker walker(std::span<const uint8_t>(buffer.data(), static_cast<size_t>(n)));
			if (!walker.Valid())
				continue;

			// Step 4: Sequence bookkeeping (the snapshot feed restarts per cycle; count only
			// backward-free forward gaps).
			const uint32_t seqNum = walker.Header().MsgSeqNum;
			if (packets > 0 && seqNum > nextSeqNum)
				++gaps;
			nextSeqNum = seqNum + 1;
			++packets;

			// Step 5: The messages inside.
			Mdp3::MessageView message;
			while (walker.TryNext(message))
			{
				++messages;
				++countByTemplate[message.TemplateId];
				if (printed < 6)
				{
					std::cout << "packet seq " << seqNum << "  " << Mdp3::ToObjectType(message.TemplateId) << ":\n"
					          << Mdp3::ToJsonLine(message.TemplateId, message.Body) << "\n\n";
					++printed;
				}
			}
		}

		// Step 6: The inventory and the sequence verdict.
		std::cout << "---- " << packets << " packets, " << messages << " messages, " << gaps << " sequence gaps ----\n";
		for (const auto& [templateId, count] : countByTemplate)
			std::cout << "  " << Mdp3::ToObjectType(templateId) << ": " << count << "\n";
		return packets > 0 ? 0 : 1;
	}
	catch (const std::exception& error)
	{
		std::cerr << "error: " << error.what() << "\n";
		return 1;
	}
}
