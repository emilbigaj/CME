// The market-data connectivity probe: answers whether CME's multicast feeds actually reach
// this machine on a given interface. It reads the channel configuration, joins a channel's
// incremental A and B feeds and its snapshot feed, and reports what arrives — packet and byte
// counts and the packet sequence numbers — over a fixed window. The snapshot feed cycles
// continuously through market hours, so silence on all three during the session points to the
// network (multicast not provisioned or wrong interface), not to a quiet market. As a second
// signal it checks the channel's TCP replay service, whose reachability does not depend on
// multicast routing at all.
//
//   Mdp3Probe <config.xml> [channelId=310] [interfaceIp=10.220.14.4] [seconds=15]

#include "ChannelConfig.hpp"
#include "UdpReceiver.hpp"
#include "Mdp3Sbe.hpp"        // Mdp3::PacketHeader
#include "TcpConnection.hpp"
#include "Timestamp.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace
{

// What one joined feed has delivered so far.
struct FeedProbe
{
	const Mdp3::Connection* Connection = nullptr;
	Mdp3::UdpReceiver Receiver;
	uint64_t Packets = 0;
	uint64_t Bytes = 0;
	uint32_t FirstSeqNum = 0;
	uint32_t LastSeqNum = 0;
};

} // namespace

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		std::cerr << "usage: Mdp3Probe <config.xml> [channelId=310] [interfaceIp=10.220.14.4] [seconds=15]\n";
		return 2;
	}
	const int32_t channelId = (argc > 2) ? std::stoi(argv[2]) : 310;
	const std::string interfaceIp = (argc > 3) ? argv[3] : "10.220.14.4";
	const int seconds = (argc > 4) ? std::stoi(argv[4]) : 15;

	try
	{
		// Step 1: Find the channel and show what we are about to join.
		Mdp3::ChannelConfig config = Mdp3::ChannelConfig::Load(argv[1]);
		const Mdp3::Channel* channel = config.FindChannel(channelId);
		if (channel == nullptr)
		{
			std::cerr << "channel " << channelId << " not in config (" << config.Channels().size() << " channels).\n";
			return 1;
		}
		std::cout << "Channel " << channel->Id << ": " << channel->Label << "\n";
		for (const Mdp3::Connection& connection : channel->Connections)
			std::cout << "  " << connection.Id << "  " << connection.Type << " " << connection.Feed
			          << "  " << connection.Protocol << "  " << (connection.IsUdp() ? connection.Ip : connection.HostIp)
			          << ":" << connection.Port << "\n";

		// Step 2: Join the incremental pair and the snapshot feed on the requested interface.
		std::vector<FeedProbe> probes;
		for (const char* pick : {"I:A", "I:B", "S:A"})
		{
			const Mdp3::Connection* connection = channel->Find(std::string(1, pick[0]), pick[2]);
			if (connection == nullptr || !connection->IsUdp())
				continue;
			FeedProbe& probe = probes.emplace_back();
			probe.Connection = connection;
			probe.Receiver.Join(connection->Ip, connection->Port, interfaceIp);
			std::cout << "Joined " << connection->Id << " (" << connection->Ip << ":" << connection->Port
			          << ") on " << interfaceIp << "\n";
		}

		// Step 3: Listen for the window, tallying whatever arrives on each feed.
		std::cout << "Listening for " << seconds << "s...\n";
		std::array<uint8_t, 65536> buffer{};
		const int64_t deadline = Tools::Timestamp::UtcNow().NanosSinceEpoch + static_cast<int64_t>(seconds) * 1'000'000'000LL;
		while (Tools::Timestamp::UtcNow().NanosSinceEpoch < deadline)
		{
			for (FeedProbe& probe : probes)
			{
				const ssize_t n = probe.Receiver.Recv(buffer);
				if (n < static_cast<ssize_t>(sizeof(Mdp3::PacketHeader)))
					continue;
				Mdp3::PacketHeader header{};
				std::memcpy(&header, buffer.data(), sizeof(header));
				if (probe.Packets == 0)
				{
					probe.FirstSeqNum = header.MsgSeqNum;
					std::cout << "FIRST PACKET on " << probe.Connection->Id << ": seq " << header.MsgSeqNum
					          << ", " << n << " bytes\n";
				}
				probe.LastSeqNum = header.MsgSeqNum;
				++probe.Packets;
				probe.Bytes += static_cast<uint64_t>(n);
			}
		}

		// Step 4: Per-feed verdicts.
		bool anyPackets = false;
		std::cout << "\n";
		for (FeedProbe& probe : probes)
		{
			std::cout << probe.Connection->Id << ": " << probe.Packets << " packets, " << probe.Bytes << " bytes";
			if (probe.Packets > 0)
				std::cout << ", seq " << probe.FirstSeqNum << " -> " << probe.LastSeqNum;
			std::cout << "\n";
			anyPackets |= probe.Packets > 0;
		}

		// Step 5: The TCP replay service as an independent reachability signal.
		const Mdp3::Connection* replay = channel->Find("H", 'A');
		if (replay != nullptr)
		{
			try
			{
				ILink3::TcpConnection tcp;
				tcp.Connect(replay->HostIp, replay->Port, 3);
				std::cout << "TCP replay " << replay->HostIp << ":" << replay->Port << " connects.\n";
			}
			catch (const std::exception& error)
			{
				std::cout << "TCP replay " << replay->HostIp << ":" << replay->Port << " unreachable: " << error.what() << "\n";
			}
		}

		std::cout << (anyPackets ? "\nVERDICT: multicast IS reaching this interface.\n"
		                         : "\nVERDICT: no multicast packets arrived (not provisioned, wrong interface, or market closed).\n");
		return anyPackets ? 0 : 1;
	}
	catch (const std::exception& error)
	{
		std::cerr << "error: " << error.what() << "\n";
		return 1;
	}
}
