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
	uint64_t _partyDetailsListId = 0;   // non-zero once the parties are pre-registered; 0 = on-demand
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

	// Shorten the receive time limit once logged on, so a serving loop that alternates between
	// this gateway and other work is never stalled by a quiet connection.
	void SetReceiveTimeout(int milliseconds) { _connection.SetReceiveTimeout(milliseconds); }

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
		// Any party registration belonged to the previous session — start on-demand again.
		_uuid = MakeUuid();
		_partyDetailsListId = 0;
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

	// Non-zero once a registered party id is in effect; 0 means on-demand mode.
	uint64_t PartyDetailsListId() const { return _partyDetailsListId; }

	// Adopt a party id registered elsewhere (on the Order Entry Service Gateway), so every
	// order this session sends references it instead of dragging its own definition.
	void SetPartyDetailsListId(uint64_t partyDetailsListId) { _partyDetailsListId = partyDetailsListId; }

	// Register the trading parties under this session's own id. Meant for a session opened to
	// the Order Entry Service Gateway (the dedicated CME segment that stores party lists for
	// the firm) — a trading gateway only accepts id 0, so registration attempted there is
	// always rejected. The id is the session uuid: unique per run, so a re-registration never
	// collides with a list CME still holds from earlier in the week. On success trading
	// sessions adopt the id via SetPartyDetailsListId; any reply other than an accepted
	// PartyDetailsDefinitionRequestAck (a rejected status, a business reject, or silence)
	// leaves this session at id 0 and returns false.
	bool RegisterPartyDetails()
	{
		// Step 1: Only meaningful on an open session.
		if (_state != SessionState::Established)
			return false;

		// Step 2: Send the definition under the session id, as a sequenced business message.
		SendFramed(EncodePartyDetailsDefinitionRequest(_config.Parties, _uuid, _outboundSeqNo,
			static_cast<uint64_t>(Tools::Timestamp::UtcNow().NanosSinceEpoch), _sendBuffer));
		++_outboundSeqNo;

		// Step 3: Wait for the reply, letting session-layer messages pass: a heartbeat is
		// skipped, a terminate closes the session and gives up.
		FramedMessage message{};
		for (int attempt = 0; attempt < 5; ++attempt)
		{
			if (!ReceiveMessage(message))
			{
				std::cout << "Party registration: no reply; staying on-demand.\n";
				return false;
			}
			if (message.TemplateId == Sequence::TemplateId)
				continue;
			if (message.TemplateId == Terminate::TemplateId)
			{
				std::cout << "CME Terminate during party registration: " << message.As<Terminate>()->ToString() << "\n";
				_state = SessionState::Disconnected;
				return false;
			}
			break;
		}

		// Step 4: Nothing but heartbeats means no reply; otherwise what came back is a
		// sequenced business message — count it.
		if (message.TemplateId == Sequence::TemplateId)
		{
			std::cout << "Party registration: no reply; staying on-demand.\n";
			return false;
		}
		++_inboundSeqNo;

		// Step 5: Accepted means our id echoed back with status 0; adopt the id.
		if (message.TemplateId == PartyDetailsDefinitionRequestAck::TemplateId)
		{
			const PartyDetailsDefinitionRequestAck* ack = message.As<PartyDetailsDefinitionRequestAck>();
			if (ack->PartyDetailRequestStatus == 0 && ack->PartyDetailsListReqID == _uuid)
			{
				_partyDetailsListId = _uuid;
				std::cout << "Party details registered (id " << _partyDetailsListId << ").\n";
				return true;
			}
		}

		// Step 6: Rejected — print the decoded reply (the memo carries CME's reason).
		std::cout << "Party registration rejected: "
		          << ToObjectType(message.TemplateId) << ": " << ToJsonLine(message.TemplateId, message.Body) << "\n";
		return false;
	}

	// Diagnostic: send one PartyDetailsDefinitionRequest with an explicit party id (0 allowed)
	// and print CME's reply, to probe which id values the session accepts. Returns the reply's
	// template id (0 if none arrived).
	uint16_t ProbePartyDetails(uint64_t reqId)
	{
		// Step 1: Only meaningful on an open session.
		if (_state != SessionState::Established)
			return 0;

		// Step 2: Send the request with the id exactly as given.
		SendFramed(EncodePartyDetailsDefinitionRequest(_config.Parties, reqId, _outboundSeqNo,
			static_cast<uint64_t>(Tools::Timestamp::UtcNow().NanosSinceEpoch), _sendBuffer));
		++_outboundSeqNo;

		// Step 3: Print whatever comes back, keyed by the id we sent.
		FramedMessage message{};
		if (!ReceiveMessage(message))
		{
			std::cout << "  reqId=" << reqId << " -> no reply\n";
			return 0;
		}
		std::cout << "  reqId=" << reqId << " -> " << ToObjectType(message.TemplateId) << ": "
		          << ToJsonLine(message.TemplateId, message.Body) << "\n";
		return message.TemplateId;
	}

	// Send a new order. A session with pre-registered parties sends just the order, referencing
	// the registered id. An on-demand session pairs it with a PartyDetailsDefinitionRequest
	// carrying the parties (id 0) sent immediately before — CME applies the just-defined parties
	// to it. The caller only fills the order details; the gateway stamps the party id, sequence
	// numbers, and send times. Returns false if the session is not open.
	bool SendNewOrderSingle(NewOrderSingle order)
	{
		// Step 1: Only send on an open session.
		if (_state != SessionState::Established)
			return false;

		// Step 2: On-demand only: define the parties for this order (id 0) as its own message.
		if (_partyDetailsListId == 0)
		{
			SendFramed(EncodePartyDetailsDefinitionRequest(_config.Parties, /*partyDetailsListReqId*/ 0,
				_outboundSeqNo, static_cast<uint64_t>(Tools::Timestamp::UtcNow().NanosSinceEpoch), _sendBuffer));
			++_outboundSeqNo;
		}

		// Step 3: Send the order referencing the session's party id. Stamp the fields the
		// gateway owns: this message's sequence number and send time.
		order.PartyDetailsListReqID = _partyDetailsListId;
		order.SeqNum = _outboundSeqNo;
		order.SendingTimeEpoch = static_cast<uint64_t>(Tools::Timestamp::UtcNow().NanosSinceEpoch);
		SendFramed(FrameMessage(_sendBuffer, NewOrderSingle::TemplateId, NewOrderSingle::BlockLength, &order, sizeof(order), 0));
		++_outboundSeqNo;
		return true;
	}

	// Send an order cancel. Every order-management message carries the session's party id — the
	// cancel included — so a pre-registered session sends just the cancel, and an on-demand
	// session pairs it with a party-details definition (id 0) sent immediately before. The
	// gateway stamps the sequence numbers and send times. Returns false if the session is not open.
	bool SendOrderCancel(OrderCancelRequest cancel)
	{
		// Step 1: Only send on an open session.
		if (_state != SessionState::Established)
			return false;

		// Step 2: On-demand only: define the parties for this message (id 0) as its own message.
		if (_partyDetailsListId == 0)
		{
			SendFramed(EncodePartyDetailsDefinitionRequest(_config.Parties, /*partyDetailsListReqId*/ 0,
				_outboundSeqNo, static_cast<uint64_t>(Tools::Timestamp::UtcNow().NanosSinceEpoch), _sendBuffer));
			++_outboundSeqNo;
		}

		// Step 3: Send the cancel referencing the session's party id.
		cancel.PartyDetailsListReqID = _partyDetailsListId;
		cancel.SeqNum = _outboundSeqNo;
		cancel.SendingTimeEpoch = static_cast<uint64_t>(Tools::Timestamp::UtcNow().NanosSinceEpoch);
		SendFramed(FrameMessage(_sendBuffer, OrderCancelRequest::TemplateId, OrderCancelRequest::BlockLength, &cancel, sizeof(cancel), 0));
		++_outboundSeqNo;
		return true;
	}

	// Send an order replace (new price/size for a working order). Like every order-management
	// message it carries the session's party id; an on-demand session pairs it with a
	// party-details definition (id 0) sent immediately before it. The gateway stamps the
	// sequence numbers and send times. Returns false if the session is not open.
	bool SendOrderCancelReplace(OrderCancelReplaceRequest replace)
	{
		// Step 1: Only send on an open session.
		if (_state != SessionState::Established)
			return false;

		// Step 2: On-demand only: define the parties for this message (id 0) as its own message.
		if (_partyDetailsListId == 0)
		{
			SendFramed(EncodePartyDetailsDefinitionRequest(_config.Parties, /*partyDetailsListReqId*/ 0,
				_outboundSeqNo, static_cast<uint64_t>(Tools::Timestamp::UtcNow().NanosSinceEpoch), _sendBuffer));
			++_outboundSeqNo;
		}

		// Step 3: Send the replace referencing the session's party id.
		replace.PartyDetailsListReqID = _partyDetailsListId;
		replace.SeqNum = _outboundSeqNo;
		replace.SendingTimeEpoch = static_cast<uint64_t>(Tools::Timestamp::UtcNow().NanosSinceEpoch);
		SendFramed(FrameMessage(_sendBuffer, OrderCancelReplaceRequest::TemplateId, OrderCancelReplaceRequest::BlockLength, &replace, sizeof(replace), 0));
		++_outboundSeqNo;
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
