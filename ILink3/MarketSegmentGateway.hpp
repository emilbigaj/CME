#pragma once

// One connection to one CME market-segment gateway, from logon to logoff. It ties together
// the three things a live session needs: the network connection, the logger that records
// every message, and the session state (the session id, the sequence numbers, and the
// keep-alive clock). One thread owns a gateway and drives it: it connects, logs in, then
// calls Poll() in a loop. Everything above the connection is transport-agnostic, so swapping
// the kernel-socket connection for the kernel-bypass one in production changes nothing here.
//
// The session state is kept directly in this class rather than a separate object, because it
// is meaningless without the connection it rides on. Recovery of missed messages
// (retransmission after a gap) is not handled yet — noted at the points where it belongs.

#include "ILink3Config.hpp"
#include "ILink3Sbe.hpp"
#include "CmeLogger.hpp"
#include "TcpConnection.hpp"
#include "Wire.hpp"
#include "Timestamp.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace ILink3
{

// Where a gateway is in its lifecycle.
enum class SessionState : uint8_t
{
	Disconnected = 0,   // no connection
	Connected = 1,      // network connected, not yet logged in
	Established = 2,     // logged in; the sequenced session is open
};

class MarketSegmentGateway
{
	// ---- fixed configuration (set once at construction) ----
	ILink3Config _config;
	MarketSegment _segment;
	bool _useSecondary;
	CmeLogger* _logger;

	// ---- connection + session state (owned by the single driving thread) ----
	TcpConnection _connection;
	SessionState _state = SessionState::Disconnected;
	uint64_t _uuid = 0;                 // this session's id; the same value signs both logon messages
	uint32_t _outboundSeqNo = 1;        // next business message number we will send
	uint32_t _inboundSeqNo = 1;         // next business message number we expect from CME
	int64_t _lastSendNanos = 0;         // when we last sent anything, for the keep-alive clock

	// ---- buffers reused every message (no per-message allocation) ----
	std::array<uint8_t, 512> _sendBuffer{};
	std::vector<uint8_t> _recvBuffer;
	size_t _consumed = 0;               // how far into _recvBuffer we have already framed
	std::array<uint8_t, 8192> _chunk{};

public:
	// Called for each received business message (execution reports and the like). Session-layer
	// messages are handled internally and never reach here. Wired to the order side later.
	std::function<void(const FramedMessage&)> OnBusinessMessage;

	// Build a gateway for one market segment. Looks up that segment's addresses in the config.
	MarketSegmentGateway(const ILink3Config& config, int32_t marketSegmentId, CmeLogger* logger, bool useSecondary = false)
		: _config(config), _useSecondary(useSecondary), _logger(logger)
	{
		// Step 1: Resolve the market segment; without it there is nothing to connect to.
		const MarketSegment* segment = config.TryFindMarketSegment(marketSegmentId);
		if (segment == nullptr)
			throw std::invalid_argument("MarketSegmentGateway: market segment " + std::to_string(marketSegmentId) + " not in config");
		_segment = *segment;

		// Step 2: Size the receive buffer once.
		_recvBuffer.reserve(64 * 1024);
	}

	SessionState State() const { return _state; }
	int32_t MarketSegmentId() const { return _segment.MarketSegmentID; }
	uint64_t Uuid() const { return _uuid; }

	// Open the network connection to this segment's gateway (primary or backup).
	void Connect()
	{
		// Step 1: Pick the primary or backup address for this segment.
		const std::string ip = _useSecondary ? _segment.SecondaryIPAddress : _segment.PrimaryIPAddress;

		// Step 2: Connect with a short receive timeout, so Poll() returns promptly to service
		// the keep-alive clock even when no data is arriving.
		_connection.Connect(ip, _config.Port, /*recvTimeoutSeconds*/ 1);
		_state = SessionState::Connected;
	}

	// Log in: Negotiate then Establish. On success the sequenced session is open. Returns true
	// if EstablishmentAck came back, false on any rejection or unexpected reply.
	bool Logon()
	{
		// Step 1: A fresh session id for this run; both logon messages must carry the same one.
		_uuid = MakeUuid();
		_outboundSeqNo = 1;
		_inboundSeqNo = 1;

		// Step 2: Send Negotiate and require a NegotiationResponse in reply.
		SendFramed(EncodeNegotiate(_config, _uuid, RequestTimestampNow(), _sendBuffer));
		FramedMessage message{};
		if (!ReceiveMessage(message))
			return false;
		if (message.TemplateId != NegotiationResponse::TemplateId)
		{
			ReportUnexpected("Negotiate", message);
			return false;
		}

		// Step 3: Send Establish (outbound sequence starts at 1) and require an
		// EstablishmentAck. CME echoes back the keep-alive interval it accepted.
		SendFramed(EncodeEstablish(_config, _uuid, RequestTimestampNow(), _outboundSeqNo, _sendBuffer));
		if (!ReceiveMessage(message))
			return false;
		if (message.TemplateId != EstablishmentAck::TemplateId)
		{
			ReportUnexpected("Establish", message);
			return false;
		}

		// Step 4: Session is open. Start the keep-alive clock from this last send.
		_state = SessionState::Established;
		return true;
	}

	// One service pass, called in a loop by the owning thread: take in whatever has arrived,
	// then send a heartbeat if we have been silent too long.
	void Poll()
	{
		// Step 1: Drain any messages that have arrived.
		ReceiveAvailable();

		// Step 2: Keep the session alive.
		MaybeSendKeepAlive();
	}

	// Close the session cleanly: tell CME we are terminating, then drop the connection.
	void Disconnect(const std::string& reason = "normal")
	{
		// Step 1: If a session is open, send a Terminate so CME closes its side tidily.
		if (_state == SessionState::Established)
		{
			Terminate terminate{};
			terminate.Reason = reason;
			terminate.UUID = _uuid;
			terminate.RequestTimestamp = RequestTimestampNow();
			SendFramed(FrameMessage(_sendBuffer, Terminate::TemplateId, Terminate::BlockLength, &terminate, sizeof(terminate), 0));
		}

		// Step 2: Drop the connection and reset state.
		_connection.Close();
		_state = SessionState::Disconnected;
	}

private:
	// Send a framed message (already written into _sendBuffer) and log it strictly after the
	// send returns. Also restarts the keep-alive clock, since any send counts as activity.
	void SendFramed(size_t length)
	{
		// Step 1: Hand the bytes to the connection.
		_connection.SendAll(std::span<const uint8_t>(_sendBuffer.data(), length));

		// Step 2: Stamp and log after the transport is done.
		const int64_t stamp = Tools::Timestamp::UtcNow().NanosSinceEpoch;
		if (_logger != nullptr)
			_logger->Log(Direction::Send, stamp, 0, std::span<const uint8_t>(_sendBuffer.data(), length));

		// Step 3: Reset the silence timer.
		_lastSendNanos = stamp;
	}

	// Read once (bounded by the short receive timeout) and dispatch every complete message that
	// is now buffered. Does not block waiting for a specific message.
	void ReceiveAvailable()
	{
		// Step 1: One read. A timeout (-1) just means nothing arrived this pass.
		ssize_t n = _connection.Recv(_chunk);
		if (n <= 0)
		{
			if (n == 0)
				_state = SessionState::Disconnected;   // peer closed
			return;
		}
		const int64_t stamp = Tools::Timestamp::UtcNow().NanosSinceEpoch;
		_recvBuffer.insert(_recvBuffer.end(), _chunk.begin(), _chunk.begin() + n);

		// Step 2: Frame and dispatch every whole message now available.
		FramedMessage message{};
		while (TryFrame(std::span<const uint8_t>(_recvBuffer).subspan(_consumed), message))
		{
			LogRecv(stamp, message);
			Dispatch(message);
			_consumed += message.TotalLength;
		}

		// Step 3: Reclaim the buffer once everything in it has been consumed.
		if (_consumed == _recvBuffer.size())
		{
			_recvBuffer.clear();
			_consumed = 0;
		}
	}

	// Send a heartbeat if the keep-alive interval (less a safety margin) has elapsed since our
	// last send. CME terminates a session that stays silent for two intervals.
	void MaybeSendKeepAlive()
	{
		// Step 1: Only meaningful once the session is open.
		if (_state != SessionState::Established)
			return;

		// Step 2: Send at half the interval, so we are always comfortably inside it.
		const int64_t nowNanos = Tools::Timestamp::UtcNow().NanosSinceEpoch;
		const int64_t halfIntervalNanos = static_cast<int64_t>(_config.KeepAliveIntervalMs) * 1'000'000LL / 2;
		if (nowNanos - _lastSendNanos >= halfIntervalNanos)
			SendKeepAlive();
	}

	// Send a Sequence(506) carrying our current next-outbound number.
	void SendKeepAlive()
	{
		SendFramed(EncodeSequence(_uuid, _outboundSeqNo, _sendBuffer));
	}

	// Handle one received message by its kind: session-layer messages are dealt with here;
	// business messages go to the callback.
	void Dispatch(const FramedMessage& message)
	{
		switch (message.TemplateId)
		{
			case Sequence::TemplateId:
			{
				// CME's own heartbeat. If it says a keep-alive interval lapsed, it has not heard
				// from us — answer immediately. (A gap between its next-outbound and what we have
				// seen would trigger a retransmission request here; not handled yet.)
				const Sequence* sequence = message.As<Sequence>();
				if (sequence->KeepAliveIntervalLapsed == KeepAliveLapsed::Lapsed)
					SendKeepAlive();
				break;
			}
			case Terminate::TemplateId:
			{
				// CME is closing the session; record why and mark ourselves down.
				std::cout << "CME Terminate: " << message.As<Terminate>()->ToString() << "\n";
				_state = SessionState::Disconnected;
				break;
			}
			default:
			{
				// A business message (execution report and the like). Track the inbound count
				// and forward it. (Out-of-order detection and recovery belong here later.)
				++_inboundSeqNo;
				if (OnBusinessMessage)
					OnBusinessMessage(message);
				break;
			}
		}
	}

	// Log a received message, strictly after it has been framed.
	void LogRecv(int64_t stamp, const FramedMessage& message)
	{
		if (_logger != nullptr)
			_logger->Log(Direction::Recv, stamp, 0,
				std::span<const uint8_t>(_recvBuffer.data() + _consumed, message.TotalLength));
	}

	// Blocking wait for the next single message during logon (a few timeouts, then give up).
	bool ReceiveMessage(FramedMessage& message)
	{
		// Step 1: Try to frame, reading more only when needed.
		for (int attempt = 0; attempt < 5; ++attempt)
		{
			if (TryFrame(std::span<const uint8_t>(_recvBuffer).subspan(_consumed), message))
			{
				LogRecv(Tools::Timestamp::UtcNow().NanosSinceEpoch, message);
				_consumed += message.TotalLength;
				return true;
			}
			ssize_t n = _connection.Recv(_chunk);
			if (n == 0)
			{
				_state = SessionState::Disconnected;
				return false;
			}
			if (n < 0)
				continue;   // timeout, try again
			_recvBuffer.insert(_recvBuffer.end(), _chunk.begin(), _chunk.begin() + n);
		}
		return false;
	}

	// Print a decoded reject/unexpected reply during logon.
	void ReportUnexpected(const char* stage, const FramedMessage& message)
	{
		std::cout << stage << " got unexpected reply " << ToObjectType(message.TemplateId) << "\n";
	}
};

} // namespace ILink3
