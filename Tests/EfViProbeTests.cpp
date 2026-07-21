// Proves the kernel-bypass market-data receive against a live feed: joins one channel's
// incremental feed through the Solarflare adapter path and reports what arrives — packets,
// bytes, and packet-sequence continuity — plus anything the adapter flagged or the frame
// parser refused. The exact numbers a kernel receiver would show, off the bypass path.
//
//   EfViProbe <config.xml> [channelId=310] [feed=A] [interfaceIp=172.17.163.103] [seconds=10]

#include "EfViReceiver.hpp"
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
		std::cerr << "usage: EfViProbe <config.xml> [channelId=310] [feed=A] [interfaceIp=172.17.163.103] [seconds=10]\n";
		return 2;
	}
	const int32_t channelId = (argc > 2) ? std::stoi(argv[2]) : 310;
	const char feed = (argc > 3) ? argv[3][0] : 'A';
	const std::string interfaceIp = (argc > 4) ? argv[4] : "172.17.163.103";
	const int seconds = (argc > 5) ? std::stoi(argv[5]) : 10;

	try
	{
		// Step 1: The channel's incremental feed address.
		Mdp3::ChannelConfig config = Mdp3::ChannelConfig::Load(argv[1]);
		const Mdp3::Channel* channel = config.FindChannel(channelId);
		if (channel == nullptr)
		{
			std::cerr << "channel " << channelId << " not in config.\n";
			return 1;
		}
		const Mdp3::Connection* incremental = channel->Find("I", feed);
		if (incremental == nullptr)
		{
			std::cerr << "channel " << channelId << " has no incremental feed " << feed << ".\n";
			return 1;
		}

		// Step 2: Join through the adapter path.
		Mdp3::EfViReceiver receiver;
		receiver.Join(incremental->Ip, incremental->Port, interfaceIp);
		std::cout << "Joined " << incremental->Id << " (" << incremental->Ip << ":" << incremental->Port
		          << ") on " << interfaceIp << " via kernel bypass; listening " << seconds << "s...\n";

		// Step 3: Count arrivals and watch the packet sequence.
		std::array<uint8_t, 65536> buffer{};
		uint64_t packets = 0, bytes = 0, gaps = 0;
		uint32_t firstSeq = 0, lastSeq = 0;
		const int64_t deadline = Tools::Timestamp::UtcNow().NanosSinceEpoch + static_cast<int64_t>(seconds) * 1'000'000'000LL;
		while (Tools::Timestamp::UtcNow().NanosSinceEpoch < deadline)
		{
			const ssize_t n = receiver.Recv(buffer);
			if (n <= 0)
				continue;
			Mdp3::PacketWalker walker(std::span<const uint8_t>(buffer.data(), static_cast<size_t>(n)));
			const uint32_t sequenceNumber = walker.Header().MsgSeqNum;
			if (packets == 0)
			{
				firstSeq = sequenceNumber;
				// The adapter's arrival stamp against this machine's clock: a small steady
				// difference means the adapter clock is disciplined; zero means no stamping.
				const int64_t softwareNanos = Tools::Timestamp::UtcNow().NanosSinceEpoch;
				std::cout << "first packet: adapter stamp " << receiver.LastNicTimestampNanos
				          << ", software clock " << softwareNanos
				          << " (adapter leads by " << (softwareNanos - receiver.LastNicTimestampNanos) << " ns)\n";
			}
			else if (sequenceNumber != lastSeq + 1)
				++gaps;
			lastSeq = sequenceNumber;
			++packets;
			bytes += static_cast<uint64_t>(n);
		}

		// Step 4: The verdict.
		std::cout << "packets " << packets << ", bytes " << bytes
		          << ", seq " << firstSeq << " -> " << lastSeq << ", gaps " << gaps
		          << ", discards " << receiver.Discards << ", unparsed " << receiver.Unparsed << "\n";
		receiver.Close();
		return packets > 0 ? 0 : 1;
	}
	catch (const std::exception& error)
	{
		std::cerr << "error: " << error.what() << "\n";
		return 1;
	}
}
