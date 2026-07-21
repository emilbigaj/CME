#pragma once

// One logical market-data feed from CME's redundant pair. The exchange publishes the
// identical packet stream twice — the A feed on one network path, the B feed on the other —
// so losing either path loses nothing. Whichever copy of a packet arrives first is
// delivered; the later copy is recognized by its packet sequence number and dropped. The
// owner joins A always and B when the machine has a path for it; with only A joined this
// is a plain pass-through.
//
// Presents the receiver surface (Recv), so the market-data loop reads the arbitrated stream
// exactly as it would read one feed. Sequence policing stays where it always was — the book
// builder — this layer only merges and deduplicates.

#include <cstdint>
#include <cstring>
#include <span>

namespace Mdp3
{

template <typename Receiver>
class FeedArbitrator
{
	uint32_t _lastDelivered = 0;   // newest packet sequence handed to the caller
	bool _pollBFirst = false;      // alternates, so neither side is structurally favored

	// A packet sequence far below the last delivered is a stream restart (the weekly reset,
	// or a channel reset), not a duplicate.
	static constexpr uint32_t RestartBackstep = 1'000'000;

public:
	// The two sides; the owner joins A (and B when available) directly.
	Receiver A;
	Receiver B;
	bool HasB = false;   // set by the owner once B is joined

	// ---- tallies for monitoring ----
	uint64_t APackets = 0;     // delivered from A
	uint64_t BPackets = 0;     // delivered from B
	uint64_t Duplicates = 0;   // later copies dropped

	// Leave both feeds.
	void Close()
	{
		A.Close();
		if (HasB)
			B.Close();
	}

	// Take one fresh packet if either side has one: returns its byte count, or -1 when
	// nothing new is waiting.
	ssize_t Recv(std::span<uint8_t> buffer)
	{
		// Step 1: Alternate which side is asked first, so arrival order decides in the long
		// run rather than polling order.
		_pollBFirst = !_pollBFirst;
		if (_pollBFirst && HasB)
		{
			const ssize_t n = TakeFresh(B, BPackets, buffer);
			if (n > 0)
				return n;
		}
		const ssize_t fromA = TakeFresh(A, APackets, buffer);
		if (fromA > 0)
			return fromA;
		if (!_pollBFirst && HasB)
			return TakeFresh(B, BPackets, buffer);
		return -1;
	}

private:
	// Drain one side until it yields a packet the caller has not seen (delivered) or runs
	// dry (-1). Duplicates are consumed and counted, never delivered.
	ssize_t TakeFresh(Receiver& side, uint64_t& delivered, std::span<uint8_t> buffer)
	{
		for (;;)
		{
			const ssize_t n = side.Recv(buffer);
			if (n <= 0)
				return -1;

			// Step 1: The packet sequence lives at the very front of every datagram.
			if (static_cast<size_t>(n) < sizeof(uint32_t))
				continue;
			uint32_t sequenceNumber;
			std::memcpy(&sequenceNumber, buffer.data(), sizeof(sequenceNumber));

			// Step 2: Fresh means newer than anything delivered — or a stream restart.
			const bool restarted = _lastDelivered > RestartBackstep
			                    && sequenceNumber < _lastDelivered - RestartBackstep;
			if (_lastDelivered != 0 && sequenceNumber <= _lastDelivered && !restarted)
			{
				++Duplicates;
				continue;
			}
			_lastDelivered = sequenceNumber;
			++delivered;
			return n;
		}
	}
};

} // namespace Mdp3
