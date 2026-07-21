#pragma once

// Kernel-bypass multicast receive on a Solarflare port — the production market-data path.
// It presents the exact surface of UdpReceiver (Join / Recv / Close), so the packet walker
// and book builder run on either without change: Recv hands back one datagram's UDP payload.
//
// Two layers cooperate. A plain kernel socket holds the group membership — that is what
// makes the network deliver the group to this port at all — but is never read. The adapter
// filter then steers every matching packet straight into this receiver's own buffer ring in
// user space, never up the kernel stack. Receiving is a poll of the adapter's event queue:
// no system call, no interrupt, no copy until the payload is handed to the caller.
//
// The configuration names interfaces by address (same as the kernel receiver); the matching
// interface name is resolved from the address at join time.

#include "UdpReceiver.hpp"   // the kernel-level group membership

#include <etherfabric/base.h>
#include <etherfabric/ef_vi.h>
#include <etherfabric/memreg.h>
#include <etherfabric/pd.h>
#include <etherfabric/vi.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>

namespace Mdp3
{

class EfViReceiver
{
	static constexpr int BufferCount = 512;    // adapter receive ring depth
	static constexpr int BufferBytes = 2048;   // one whole frame per buffer

	// ---- the two receive layers ----
	UdpReceiver _membership;        // kernel join: drives delivery of the group to this port
	ef_driver_handle _driver = 0;
	ef_pd _protectionDomain{};
	ef_vi _virtualInterface{};
	ef_memreg _memoryRegion{};
	uint8_t* _buffers = nullptr;    // BufferCount frames of BufferBytes, adapter-registered
	int _prefixLength = 0;          // adapter-written metadata before each frame
	bool _timestamps = false;       // the adapter stamps each arrival
	bool _open = false;

	// ---- events already polled but not yet handed to the caller ----
	struct PendingPacket
	{
		uint32_t BufferId = 0;
		bool Discard = false;
	};
	PendingPacket _pending[64];
	uint32_t _pendingHead = 0;
	uint32_t _pendingCount = 0;

public:
	EfViReceiver() = default;
	EfViReceiver(const EfViReceiver&) = delete;
	EfViReceiver& operator=(const EfViReceiver&) = delete;
	~EfViReceiver() { Close(); }

	// Counters the owner may read: frames the adapter flagged bad, and payloads that were
	// not plain unicast-length datagrams we understood.
	uint64_t Discards = 0;
	uint64_t Unparsed = 0;

	// The adapter's own arrival stamp for the packet Recv just returned (0 when the adapter
	// cannot stamp). Absolute accuracy needs the adapter clock disciplined (the time
	// synchronization daemon); undisciplined, the stamps are still precise relative times.
	int64_t LastNicTimestampNanos = 0;

	// Join a multicast group on the interface with the given address. The time limit is
	// accepted for surface-compatibility; receiving never blocks.
	void Join(const std::string& groupIp, uint16_t port, const std::string& interfaceIp, int recvTimeoutMilliseconds = 100)
	{
		(void)recvTimeoutMilliseconds;

		// Step 1: The kernel joins the group; the membership is the delivery driver.
		_membership.Join(groupIp, port, interfaceIp, 1);

		// Step 2: Open the adapter and claim a virtual interface on the named port.
		const std::string interfaceName = InterfaceNameFor(interfaceIp);
		int rc = ef_driver_open(&_driver);
		if (rc != 0)
			throw std::runtime_error("EfViReceiver: ef_driver_open failed: " + std::string(std::strerror(-rc)));
		rc = ef_pd_alloc_by_name(&_protectionDomain, _driver, interfaceName.c_str(), EF_PD_DEFAULT);
		if (rc != 0)
			throw std::runtime_error("EfViReceiver: ef_pd_alloc(" + interfaceName + ") failed: " + std::string(std::strerror(-rc)));
		rc = ef_vi_alloc_from_pd(&_virtualInterface, _driver, &_protectionDomain, _driver,
			/*evq*/ -1, /*rxq*/ -1, /*txq*/ 0, nullptr, -1,
			static_cast<enum ef_vi_flags>(EF_VI_RX_TIMESTAMPS));
		if (rc != 0)
		{
			// Not every adapter generation can stamp; run without timestamps there.
			rc = ef_vi_alloc_from_pd(&_virtualInterface, _driver, &_protectionDomain, _driver,
				/*evq*/ -1, /*rxq*/ -1, /*txq*/ 0, nullptr, -1, EF_VI_FLAGS_DEFAULT);
			if (rc != 0)
				throw std::runtime_error("EfViReceiver: ef_vi_alloc(" + interfaceName + ") failed: " + std::string(std::strerror(-rc)));
		}
		else
			_timestamps = true;
		_prefixLength = ef_vi_receive_prefix_len(&_virtualInterface);

		// Step 3: One contiguous packet-buffer region, registered with the adapter, every
		// frame posted to the receive ring up front.
		_buffers = static_cast<uint8_t*>(std::aligned_alloc(4096, static_cast<size_t>(BufferCount) * BufferBytes));
		if (_buffers == nullptr)
			throw std::runtime_error("EfViReceiver: buffer allocation failed");
		rc = ef_memreg_alloc(&_memoryRegion, _driver, &_protectionDomain, _driver,
			_buffers, static_cast<size_t>(BufferCount) * BufferBytes);
		if (rc != 0)
			throw std::runtime_error("EfViReceiver: ef_memreg_alloc failed: " + std::string(std::strerror(-rc)));
		for (int index = 0; index < BufferCount; ++index)
			ef_vi_receive_init(&_virtualInterface,
				ef_memreg_dma_addr(&_memoryRegion, static_cast<size_t>(index) * BufferBytes),
				static_cast<ef_request_id>(index));
		ef_vi_receive_push(&_virtualInterface);

		// Step 4: Steer the group's datagrams into this receiver instead of the kernel.
		ef_filter_spec filter;
		ef_filter_spec_init(&filter, EF_FILTER_FLAG_NONE);
		rc = ef_filter_spec_set_ip4_local(&filter, IPPROTO_UDP,
			::inet_addr(groupIp.c_str()), htons(port));
		if (rc == 0)
			rc = ef_vi_filter_add(&_virtualInterface, _driver, &filter, nullptr);
		if (rc != 0)
			throw std::runtime_error("EfViReceiver: filter(" + groupIp + ") failed: " + std::string(std::strerror(-rc)));
		_open = true;
	}

	// Take one datagram's payload if one has arrived: returns its byte count, or -1 when
	// nothing is waiting. Never blocks — the owning loop is the spin.
	ssize_t Recv(std::span<uint8_t> buffer)
	{
		// Step 1: When nothing is queued from the last poll, poll the adapter once.
		if (_pendingCount == 0 && !PollAdapter())
			return -1;

		// Step 2: Take the oldest packet; its buffer returns to the ring either way.
		const PendingPacket packet = _pending[_pendingHead % 64];
		++_pendingHead;
		--_pendingCount;
		uint8_t* start = _buffers + static_cast<size_t>(packet.BufferId) * BufferBytes;
		ssize_t payloadBytes = -1;
		if (packet.Discard)
			++Discards;
		else
		{
			payloadBytes = CopyPayload(start + _prefixLength, buffer);
			LastNicTimestampNanos = 0;
			if (_timestamps && payloadBytes > 0)
			{
				ef_precisetime stamp{};
				if (ef_vi_receive_get_precise_timestamp(&_virtualInterface, start, &stamp) == 0)
					LastNicTimestampNanos = stamp.tv_sec * 1'000'000'000LL + stamp.tv_nsec;
			}
		}
		Repost(packet.BufferId);
		return payloadBytes;
	}

	// Leave the adapter and the group; safe to call more than once.
	void Close()
	{
		if (_open)
		{
			ef_memreg_free(&_memoryRegion, _driver);
			ef_vi_free(&_virtualInterface, _driver);
			ef_pd_free(&_protectionDomain, _driver);
			ef_driver_close(_driver);
			_open = false;
		}
		if (_buffers != nullptr)
		{
			std::free(_buffers);
			_buffers = nullptr;
		}
		_membership.Close();
	}

private:
	// One pass over the adapter's event queue; queues every received frame. Returns true
	// when at least one is now pending.
	bool PollAdapter()
	{
		ef_event events[16];
		const int count = ef_eventq_poll(&_virtualInterface, events, 16);
		for (int index = 0; index < count && _pendingCount < 64; ++index)
		{
			if (EF_EVENT_TYPE(events[index]) == EF_EVENT_TYPE_RX)
				_pending[(_pendingHead + _pendingCount++) % 64] =
					PendingPacket{static_cast<uint32_t>(EF_EVENT_RX_RQ_ID(events[index])), false};
			else if (EF_EVENT_TYPE(events[index]) == EF_EVENT_TYPE_RX_DISCARD)
				_pending[(_pendingHead + _pendingCount++) % 64] =
					PendingPacket{static_cast<uint32_t>(EF_EVENT_RX_DISCARD_RQ_ID(events[index])), true};
		}
		return _pendingCount > 0;
	}

	// Walk one received frame's headers and copy the datagram payload out. Returns the
	// payload byte count, or -1 for a frame that is not the plain UDP shape expected.
	ssize_t CopyPayload(const uint8_t* frame, std::span<uint8_t> buffer)
	{
		// Step 1: Ethernet: step over the addresses, and any VLAN tag, to the type.
		size_t offset = 12;
		uint16_t etherType = static_cast<uint16_t>(frame[offset] << 8 | frame[offset + 1]);
		offset += 2;
		if (etherType == 0x8100)
		{
			offset += 2;
			etherType = static_cast<uint16_t>(frame[offset] << 8 | frame[offset + 1]);
			offset += 2;
		}
		if (etherType != 0x0800)
		{
			++Unparsed;
			return -1;
		}

		// Step 2: The internet header names its own length and the protocol.
		const uint8_t* ip = frame + offset;
		if ((ip[0] >> 4) != 4 || ip[9] != IPPROTO_UDP)
		{
			++Unparsed;
			return -1;
		}
		const size_t ipHeaderBytes = static_cast<size_t>(ip[0] & 0x0F) * 4;

		// Step 3: The datagram header gives the payload length; copy the payload out.
		const uint8_t* udp = ip + ipHeaderBytes;
		const size_t udpLength = static_cast<size_t>(udp[4]) << 8 | udp[5];
		if (udpLength < 8)
		{
			++Unparsed;
			return -1;
		}
		const size_t payloadBytes = udpLength - 8;
		if (payloadBytes > buffer.size())
		{
			++Unparsed;
			return -1;
		}
		std::memcpy(buffer.data(), udp + 8, payloadBytes);
		return static_cast<ssize_t>(payloadBytes);
	}

	// Hand a consumed buffer back to the adapter's receive ring.
	void Repost(uint32_t bufferId)
	{
		ef_vi_receive_init(&_virtualInterface,
			ef_memreg_dma_addr(&_memoryRegion, static_cast<size_t>(bufferId) * BufferBytes),
			static_cast<ef_request_id>(bufferId));
		ef_vi_receive_push(&_virtualInterface);
	}

	// The name of the interface holding the given address (the settings name interfaces by
	// address; the adapter wants the name).
	static std::string InterfaceNameFor(const std::string& interfaceIp)
	{
		ifaddrs* all = nullptr;
		if (::getifaddrs(&all) != 0)
			throw std::runtime_error("EfViReceiver: getifaddrs failed");
		std::string name;
		for (ifaddrs* entry = all; entry != nullptr; entry = entry->ifa_next)
		{
			if (entry->ifa_addr == nullptr || entry->ifa_addr->sa_family != AF_INET)
				continue;
			char text[INET_ADDRSTRLEN]{};
			::inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(entry->ifa_addr)->sin_addr, text, sizeof(text));
			if (interfaceIp == text)
			{
				name = entry->ifa_name;
				break;
			}
		}
		::freeifaddrs(all);
		if (name.empty())
			throw std::runtime_error("EfViReceiver: no interface has address " + interfaceIp);
		return name;
	}
};

} // namespace Mdp3
