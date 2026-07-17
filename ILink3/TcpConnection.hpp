#pragma once

// Minimal blocking network client for one long-lived iLink 3 session. It uses the ordinary
// operating-system socket calls: run the process under Solarflare's `onload` and the exact
// same code becomes kernel-bypass accelerated with no change (the onload-first stage of the
// transport plan). The faster cut-through send path comes later, behind this same
// Send / Recv surface.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>

namespace ILink3
{

// Owns a single open connection and closes it automatically when destroyed. It cannot be
// copied (two owners of one connection would be a bug), but it can be moved, which hands the
// connection to a new owner and leaves the old one empty. Framing the byte stream into
// messages is the caller's job — this class only opens, sends, receives, and closes.
class TcpConnection
{
	int _fd = -1;   // operating-system handle for the open connection; -1 means none

public:
	TcpConnection() = default;

	// Release the connection when this object goes away.
	~TcpConnection()
	{
		Close();
	}

	TcpConnection(const TcpConnection&) = delete;
	TcpConnection& operator=(const TcpConnection&) = delete;

	// Move: take over the other object's connection and leave it empty so its destructor
	// does nothing.
	TcpConnection(TcpConnection&& other) noexcept : _fd(other._fd) { other._fd = -1; }

	// True while a connection is open.
	bool IsOpen() const { return _fd >= 0; }

	// Open a blocking connection to the numeric address and port; throws on any failure.
	void Connect(const std::string& ip, uint16_t port, int recvTimeoutSeconds = 10)
	{
		// Step 1: Drop any connection already held so this object owns at most one.
		Close();

		// Step 2: Create a blocking stream socket.
		_fd = ::socket(AF_INET, SOCK_STREAM, 0);
		if (_fd < 0)
			throw std::runtime_error(std::string("TcpConnection: socket() failed: ") + std::strerror(errno));

		// Step 3: Turn off the built-in delay that batches up tiny sends, so a small order
		// message goes out immediately instead of waiting to be grouped with more data.
		int one = 1;
		::setsockopt(_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

		// Step 4: Put a time limit on blocking receives and sends so they give up after the
		// timeout instead of hanging forever. (This does not limit the connect below.)
		timeval timeout{};
		timeout.tv_sec = recvTimeoutSeconds;
		::setsockopt(_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
		::setsockopt(_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

		// Step 5: Build the destination address from the numeric text (no name lookup).
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_port = htons(port);
		if (::inet_pton(AF_INET, ip.c_str(), &address.sin_addr) != 1)
		{
			Close();
			throw std::runtime_error("TcpConnection: bad address " + ip);
		}

		// Step 6: Perform the connection handshake; clean up and throw on failure, capturing
		// the error reason before Close() can overwrite it.
		if (::connect(_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
		{
			std::string reason = std::strerror(errno);
			Close();
			throw std::runtime_error("TcpConnection: connect(" + ip + ":" + std::to_string(port) + ") failed: " + reason);
		}
	}

	// Change the receive time limit on the open connection. Logon uses a generous limit (a
	// reply is expected); a serving loop then shortens it so a quiet connection hands control
	// back almost immediately instead of stalling the loop.
	void SetReceiveTimeout(int milliseconds)
	{
		timeval timeout{};
		timeout.tv_sec = milliseconds / 1000;
		timeout.tv_usec = (milliseconds % 1000) * 1000;
		::setsockopt(_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	}

	// Send the whole buffer, looping until every byte is accepted; throws on error or timeout.
	void SendAll(std::span<const uint8_t> data)
	{
		// Step 1: Keep sending until nothing is left.
		size_t sent = 0;
		while (sent < data.size())
		{
			// Step 2: Send the remaining slice. The no-signal flag means a dead connection
			// reports an error instead of killing the whole process with a signal.
			ssize_t n = ::send(_fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);

			// Step 3: On failure, retry if the call was merely interrupted; otherwise throw
			// (a timeout or real error may leave a half-written message, so the caller must
			// tear the session down).
			if (n <= 0)
			{
				if (n < 0 && errno == EINTR)
					continue;
				throw std::runtime_error(std::string("TcpConnection: send() failed: ") + std::strerror(errno));
			}

			// Step 4: Count the bytes accepted this time and continue.
			sent += static_cast<size_t>(n);
		}
	}

	// Read once. Returns the byte count, 0 if the peer closed, or -1 on timeout; throws on a
	// real error. Reassembling bytes into whole messages is the caller's job.
	ssize_t Recv(std::span<uint8_t> buffer)
	{
		// Step 1: Read whatever is available, up to the buffer size.
		ssize_t n = ::recv(_fd, buffer.data(), buffer.size(), 0);

		// Step 2: A receive-timeout expiry becomes a soft -1 so the caller can poll again.
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return -1;

		// Step 3: Any other error is real — throw.
		if (n < 0)
			throw std::runtime_error(std::string("TcpConnection: recv() failed: ") + std::strerror(errno));

		// Step 4: n bytes read, or 0 meaning the peer closed the connection cleanly.
		return n;
	}

	// Close the connection if one is open; safe to call more than once.
	void Close()
	{
		// Step 1: Only act if something is open.
		if (_fd >= 0)
		{
			// Step 2: Close it and mark empty so a later Close() or the destructor is harmless.
			::close(_fd);
			_fd = -1;
		}
	}
};

} // namespace ILink3
