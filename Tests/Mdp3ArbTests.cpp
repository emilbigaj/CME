// Proves feed arbitration against the live redundant pair, on the production configuration:
// the A feed received by kernel bypass on one Solarflare port, the B feed on the other, and
// the arbitrated stream checked for continuity. Healthy output shows both sides delivering
// (whichever copy of each packet lands first), the later copies counted as duplicates, and
// zero gaps in what comes out.
//
//   Mdp3Arb <config.xml> [channelId=310] [interfaceIpA=172.17.163.103] [interfaceIpB=172.17.163.94] [seconds=10]

#include "EfViReceiver.hpp"
#include "FeedArbitrator.hpp"
#include "ChannelConfig.hpp"
#include "PacketWalker.hpp"
#include "Timestamp.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <string>

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		std::cerr << "usage: Mdp3Arb <config.xml> [channelId=310] [interfaceIpA=172.17.163.103] [interfaceIpB=172.17.163.94] [seconds=10]\n";
		return 2;
	}
	const int32_t channelId = (argc > 2) ? std::stoi(argv[2]) : 310;
	const std::string interfaceIpA = (argc > 3) ? argv[3] : "172.17.163.103";
	const std::string interfaceIpB = (argc > 4) ? argv[4] : "172.17.163.94";
	const int seconds = (argc > 5) ? std::stoi(argv[5]) : 10;

	try
	{
		// Step 1: The channel's two incremental feeds.
		Mdp3::ChannelConfig config = Mdp3::ChannelConfig::Load(argv[1]);
		const Mdp3::Channel* channel = config.FindChannel(channelId);
		if (channel == nullptr)
		{
			std::cerr << "channel " << channelId << " not in config.\n";
			return 1;
		}
		const Mdp3::Connection* feedA = channel->Find("I", 'A');
		const Mdp3::Connection* feedB = channel->Find("I", 'B');
		if (feedA == nullptr || feedB == nullptr)
		{
			std::cerr << "channel " << channelId << " lacks a redundant incremental pair.\n";
			return 1;
		}

		// Step 2: Join each side on its own port, both through the adapter path.
		Mdp3::FeedArbitrator<Mdp3::EfViReceiver> arbitrator;
		arbitrator.A.Join(feedA->Ip, feedA->Port, interfaceIpA);
		arbitrator.B.Join(feedB->Ip, feedB->Port, interfaceIpB);
		arbitrator.HasB = true;
		std::cout << "Joined " << feedA->Id << " on " << interfaceIpA << " and " << feedB->Id
		          << " on " << interfaceIpB << " via kernel bypass; listening " << seconds << "s...\n";

		// Step 3: Read the arbitrated stream and watch its continuity.
		std::array<uint8_t, 65536> buffer{};
		uint64_t packets = 0, gaps = 0;
		uint32_t firstSeq = 0, lastSeq = 0;
		const int64_t deadline = Tools::Timestamp::UtcNow().NanosSinceEpoch + static_cast<int64_t>(seconds) * 1'000'000'000LL;
		while (Tools::Timestamp::UtcNow().NanosSinceEpoch < deadline)
		{
			const ssize_t n = arbitrator.Recv(buffer);
			if (n <= 0)
				continue;
			Mdp3::PacketWalker walker(std::span<const uint8_t>(buffer.data(), static_cast<size_t>(n)));
			const uint32_t sequenceNumber = walker.Header().MsgSeqNum;
			if (packets == 0)
				firstSeq = sequenceNumber;
			else if (sequenceNumber != lastSeq + 1)
				++gaps;
			lastSeq = sequenceNumber;
			++packets;
		}

		// Step 4: The verdict: both sides contributing, the stream whole.
		std::cout << "delivered " << packets << " (A " << arbitrator.APackets << ", B " << arbitrator.BPackets
		          << "), duplicates dropped " << arbitrator.Duplicates
		          << ", seq " << firstSeq << " -> " << lastSeq << ", gaps " << gaps << "\n";
		return packets > 0 && gaps == 0 ? 0 : 1;
	}
	catch (const std::exception& error)
	{
		std::cerr << "error: " << error.what() << "\n";
		return 1;
	}
}
