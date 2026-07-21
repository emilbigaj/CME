#pragma once

// Kernel-bypass network client for one long-lived iLink 3 session, on TCPDirect over a
// Solarflare port — the production transport for the tail-latency target. It presents the
// exact surface of TcpConnection, so the gateway template stamps out either one unchanged.
//
// One connection owns one TCPDirect stack, and the single thread that owns the gateway
// drives everything — TCPDirect stacks are single-threaded by design, which is the same
// one-thread-owns-the-session rule the server already follows. Sends take the adapter's
// cut-through path when available; receives copy into the caller's buffer (zero-copy and
// hardware receive timestamps are a later refinement on this same surface).
//
// The Solarflare interface is named by the standard ZF_ATTR environment variable (for
// example ZF_ATTR="interface=p513p1"), set by whatever launches the process, so this class
// needs no configuration of its own. The stack survives reconnects: only the connection
// itself is torn down and remade.

#include <zf/zf.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>   // the standard TCP_* state names zft_state() reports
#include <sys/uio.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>

#include "Timestamp.hpp"

namespace ILink3
{

class ZfConnection
{
	zf_attr* _attributes = nullptr;   // library attributes, read from ZF_ATTR at allocation
	zf_stack* _stack = nullptr;       // one stack per connection, kept across reconnects
	zft* _zocket = nullptr;           // the live TCP connection; null means none
	int _recvTimeoutMs = 10'000;      // how long Recv drives the reactor before giving up

public:
	ZfConnection() = default;
	ZfConnection(const ZfConnection&) = delete;
	ZfConnection& operator=(const ZfConnection&) = delete;

	// Release the connection, then the stack and attributes, when this object goes away.
	~ZfConnection()
	{
		Close();
		if (_stack != nullptr)
			zf_stack_free(_stack);
		if (_attributes != nullptr)
			zf_attr_free(_attributes);
	}

	// True while a connection is open.
	bool IsOpen() const { return _zocket != nullptr; }

	// Open a connection to the numeric address and port; throws on any failure. The
	// handshake is driven by hand and bounded by connectTimeoutMs.
	void Connect(const std::string& ip, uint16_t port, int recvTimeoutSeconds = 10, int connectTimeoutMs = 2000)
	{
		// Step 1: Drop any connection already held; the stack beneath it is kept.
		Close();
		_recvTimeoutMs = recvTimeoutSeconds * 1000;
		EnsureStack();

		// Step 2: Build the destination address from the numeric text (no name lookup).
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_port = htons(port);
		if (::inet_pton(AF_INET, ip.c_str(), &address.sin_addr) != 1)
			throw std::runtime_error("ZfConnection: bad address " + ip);

		// Step 3: Start the handshake. The connect call resolves the route and returns
		// without waiting; the handle becomes the connection object on success.
		zft_handle* handle = nullptr;
		int rc = zft_alloc(_stack, _attributes, &handle);
		if (rc != 0)
			throw std::runtime_error(std::string("ZfConnection: zft_alloc failed: ") + std::strerror(-rc));
		rc = zft_connect(handle, reinterpret_cast<sockaddr*>(&address), sizeof(address), &_zocket);
		if (rc != 0)
		{
			zft_handle_free(handle);
			throw std::runtime_error("ZfConnection: connect(" + ip + ":" + std::to_string(port) + ") failed: " + std::strerror(-rc));
		}

		// Step 4: Drive the stack until the handshake concludes, within the time limit.
		const int64_t deadline = Tools::Timestamp::UtcNow().NanosSinceEpoch + static_cast<int64_t>(connectTimeoutMs) * 1'000'000LL;
		while (zft_state(_zocket) == TCP_SYN_SENT)
		{
			zf_reactor_perform(_stack);
			if (Tools::Timestamp::UtcNow().NanosSinceEpoch >= deadline)
			{
				Close();
				throw std::runtime_error("ZfConnection: connect(" + ip + ":" + std::to_string(port) + ") timed out");
			}
		}
		if (zft_state(_zocket) != TCP_ESTABLISHED)
		{
			const int error = zft_error(_zocket);
			Close();
			throw std::runtime_error("ZfConnection: connect(" + ip + ":" + std::to_string(port) + ") failed: " + std::strerror(error));
		}
	}

	// Change the receive time limit on the open connection.
	void SetReceiveTimeout(int milliseconds) { _recvTimeoutMs = milliseconds; }

	// Send the whole buffer; throws on error. A full send queue is drained by servicing the
	// stack and retrying — the bytes are accepted or the connection is declared dead.
	void SendAll(std::span<const uint8_t> data)
	{
		// Step 1: Keep sending until nothing is left.
		size_t sent = 0;
		while (sent < data.size())
		{
			// Step 2: Queue the remaining slice on the wire (cut-through when available).
			const ssize_t n = zft_send_single(_zocket, data.data() + sent, data.size() - sent, 0);

			// Step 3: A full queue is not an error — service the stack and try again.
			if (n < 0)
			{
				if (n == -EAGAIN)
				{
					zf_reactor_perform(_stack);
					continue;
				}
				throw std::runtime_error(std::string("ZfConnection: send failed: ") + std::strerror(static_cast<int>(-n)));
			}

			// Step 4: Count the bytes accepted this time and continue.
			sent += static_cast<size_t>(n);
		}
	}

	// Read once. Returns the byte count, 0 if the peer closed, or -1 if nothing arrived
	// within the receive time limit; throws on a real error. The wait is a spin driving the
	// stack — this thread owns the stack, so nobody else can.
	ssize_t Recv(std::span<uint8_t> buffer)
	{
		if (_zocket == nullptr)
			throw std::runtime_error("ZfConnection: recv on a closed connection");

		// Step 1: Service the stack and take whatever is there; repeat until the limit.
		const int64_t deadline = Tools::Timestamp::UtcNow().NanosSinceEpoch + static_cast<int64_t>(_recvTimeoutMs) * 1'000'000LL;
		for (;;)
		{
			zf_reactor_perform(_stack);
			iovec vector{buffer.data(), buffer.size()};
			const int n = zft_recv(_zocket, &vector, 1, 0);
			if (n > 0)
				return n;
			if (n == 0)
				return 0;   // peer closed
			if (n != -EAGAIN)
				throw std::runtime_error(std::string("ZfConnection: recv failed: ") + std::strerror(-n));
			if (Tools::Timestamp::UtcNow().NanosSinceEpoch >= deadline)
				return -1;
		}
	}

	// Close the connection if one is open; the stack stays for the next Connect.
	void Close()
	{
		if (_zocket != nullptr)
		{
			zft_free(_zocket);   // shuts the connection down if still up
			_zocket = nullptr;
		}
	}

private:
	// Initialize the library and allocate this connection's stack, first time through. The
	// stack allocation is the step that needs the Solarflare port (from ZF_ATTR) and the
	// activated license, so its failure message points there.
	void EnsureStack()
	{
		if (_stack != nullptr)
			return;
		int rc = zf_init();
		if (rc != 0)
			throw std::runtime_error(std::string("ZfConnection: zf_init failed: ") + std::strerror(-rc));
		rc = zf_attr_alloc(&_attributes);
		if (rc != 0)
			throw std::runtime_error(std::string("ZfConnection: zf_attr_alloc failed: ") + std::strerror(-rc));
		rc = zf_stack_alloc(_attributes, &_stack);
		if (rc != 0)
			throw std::runtime_error(std::string("ZfConnection: zf_stack_alloc failed: ") + std::strerror(-rc)
				+ "  (is ZF_ATTR=\"interface=<solarflare port>\" set, and the license active?)");
	}
};

} // namespace ILink3
