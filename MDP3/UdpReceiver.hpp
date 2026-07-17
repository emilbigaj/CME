#pragma once

// Minimal multicast receiver for one market-data feed. It joins a multicast group on a chosen
// network interface and hands back one datagram per read. Like the order-entry connection,
// this is the plain operating-system version used for bring-up and the certification
// environments; the production path (kernel-bypass receive on the Solarflare) comes later
// behind the same Recv surface.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>

namespace Mdp3
{

// Owns one joined multicast subscription and closes it automatically when destroyed. One
// receiver per feed: the A and B sides of a channel are two receivers.
class UdpReceiver
{
	int _fd = -1;   // operating-system handle; -1 means not joined

public:
	UdpReceiver() = default;

	~UdpReceiver()
	{
		Close();
	}

	UdpReceiver(const UdpReceiver&) = delete;
	UdpReceiver& operator=(const UdpReceiver&) = delete;
	UdpReceiver(UdpReceiver&& other) noexcept : _fd(other._fd) { other._fd = -1; }

	bool IsOpen() const { return _fd >= 0; }

	// Join `groupIp:port` on the interface that owns `interfaceIp`; throws on any failure.
	void Join(const std::string& groupIp, uint16_t port, const std::string& interfaceIp, int recvTimeoutMilliseconds = 100)
	{
		// Step 1: Drop any subscription already held.
		Close();

		// Step 2: Create the datagram socket.
		_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
		if (_fd < 0)
			throw std::runtime_error(std::string("UdpReceiver: socket() failed: ") + std::strerror(errno));

		// Step 3: Allow other processes to join the same group and port (the usual arrangement
		// when several consumers or a capture tool listen alongside).
		int one = 1;
		::setsockopt(_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

		// Step 4: Put a time limit on blocking receives so a quiet feed hands control back.
		timeval timeout{};
		timeout.tv_sec = recvTimeoutMilliseconds / 1000;
		timeout.tv_usec = (recvTimeoutMilliseconds % 1000) * 1000;
		::setsockopt(_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

		// Step 5: Bind to the group address and port, so this socket only sees this group's
		// datagrams even when another group shares the port.
		sockaddr_in bindAddress{};
		bindAddress.sin_family = AF_INET;
		bindAddress.sin_port = htons(port);
		if (::inet_pton(AF_INET, groupIp.c_str(), &bindAddress.sin_addr) != 1)
		{
			Close();
			throw std::runtime_error("UdpReceiver: bad group address " + groupIp);
		}
		if (::bind(_fd, reinterpret_cast<sockaddr*>(&bindAddress), sizeof(bindAddress)) != 0)
		{
			std::string reason = std::strerror(errno);
			Close();
			throw std::runtime_error("UdpReceiver: bind(" + groupIp + ":" + std::to_string(port) + ") failed: " + reason);
		}

		// Step 6: Join the group on the requested interface — this is what makes the network
		// start forwarding the feed to us.
		ip_mreq membership{};
		membership.imr_multiaddr = bindAddress.sin_addr;
		if (::inet_pton(AF_INET, interfaceIp.c_str(), &membership.imr_interface) != 1)
		{
			Close();
			throw std::runtime_error("UdpReceiver: bad interface address " + interfaceIp);
		}
		if (::setsockopt(_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership, sizeof(membership)) != 0)
		{
			std::string reason = std::strerror(errno);
			Close();
			throw std::runtime_error("UdpReceiver: join(" + groupIp + " on " + interfaceIp + ") failed: " + reason);
		}
	}

	// Read one datagram. Returns its byte count, or -1 if the time limit passed with nothing
	// arriving; throws on a real error.
	ssize_t Recv(std::span<uint8_t> buffer)
	{
		// Step 1: One receive — a datagram arrives whole or not at all.
		ssize_t n = ::recv(_fd, buffer.data(), buffer.size(), 0);

		// Step 2: A timeout becomes a soft -1 so the caller can poll on.
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return -1;

		// Step 3: Any other failure is real.
		if (n < 0)
			throw std::runtime_error(std::string("UdpReceiver: recv() failed: ") + std::strerror(errno));
		return n;
	}

	// Leave the group and close; safe to call more than once.
	void Close()
	{
		if (_fd >= 0)
		{
			::close(_fd);
			_fd = -1;
		}
	}
};

} // namespace Mdp3
