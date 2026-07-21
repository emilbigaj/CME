#pragma once

// One connection to one CME market-segment gateway, from logon to logoff. It ties together
// the three things a live session needs: the network connection, the logger that records
// every message, and the session state (the session id, the sequence numbers, and the
// keep-alive clock). One thread owns a gateway and drives it: it connects, logs in, then
// calls Poll() in a loop. The class is a template on its connection type — the kernel-socket
// TcpConnection for bring-up and certification, the kernel-bypass ZfConnection in production
// — with identical behavior above the transport and no indirection between them; the plain
// MarketSegmentGateway name is the kernel-socket form.
//
// The session state is kept directly in this class rather than a separate object, because it
// is meaningless without the connection it rides on. Given a session directory the state is
// also persistent: CME expects one session id per trading week, so a restart resumes the
// same session — sequence counters carry on, and messages CME published while we were away
// (an order cancelled on disconnect, say) are recovered by retransmission at logon.

#include "ILink3Config.hpp"
#include "ILink3Sbe.hpp"
#include "CmeLogger.hpp"
#include "SessionStore.hpp"
#include "TcpConnection.hpp"
#include "Wire.hpp"
#include "Timestamp.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace ILink3
{

// Remembers what each in-flight order request was for — the tag (an order revision) its
// session-unique id went out under — until the response that echoes the id consumes it. A
// ring, not a map: requests are pushed in id order and almost always answered promptly, so
// the outstanding window stays tiny; taking an entry frees its slot and the read frontier
// compacts past consumed slots, so a live entry is only ever overwritten once the ring holds
// a full capacity of genuinely unanswered requests. One thread owns the session and drives
// both sides, so everything here is plain.
class OrderRequestRing
{
	struct Entry
	{
		uint64_t RequestId = 0;   // 0 = empty; never a real id (the counter is clock-seeded)
		int32_t Tag = 0;
	};
	static constexpr uint64_t Capacity = 1 << 16;
	std::vector<Entry> _entries = std::vector<Entry>(Capacity);
	uint64_t _writeIndex = 0;
	uint64_t _readIndex = 0;

public:
	// Remember `tag` for `requestId`; always claims the next slot.
	void Push(uint64_t requestId, int32_t tag)
	{
		_entries[_writeIndex & (Capacity - 1)] = Entry{requestId, tag};
		++_writeIndex;
	}

	// Find `requestId`, hand back its tag, and free the slot; `fallback` on a miss. The scan
	// is bounded by the genuinely outstanding window, which prompt responses keep tiny.
	int32_t Take(uint64_t requestId, int32_t fallback)
	{
		// Step 1: A lapped ring has lost its oldest entries; never scan what is gone.
		if (_writeIndex - _readIndex > Capacity)
			_readIndex = _writeIndex - Capacity;

		// Step 2: Oldest first; on the find, free the slot and compact the frontier.
		for (uint64_t index = _readIndex; index < _writeIndex; ++index)
		{
			Entry& entry = _entries[index & (Capacity - 1)];
			if (entry.RequestId == requestId && requestId != 0)
			{
				const int32_t tag = entry.Tag;
				entry.RequestId = 0;   // taken
				while (_readIndex < _writeIndex && _entries[_readIndex & (Capacity - 1)].RequestId == 0)
					++_readIndex;
				return tag;
			}
		}
		return fallback;
	}
};

// Where a gateway is in its lifecycle.
enum class SessionState : uint8_t
{
	Disconnected = 0,   // no connection
	Connected = 1,      // network connected, not yet logged in
	Established = 2,     // logged in; the sequenced session is open
};

template <typename Connection>
class BasicMarketSegmentGateway
{
	// ---- fixed configuration (set once at construction) ----
	ILink3Config _config;
	MarketSegment _segment;
	bool _useSecondary;
	CmeLogger* _logger;

	// ---- connection + session state (owned by the single driving thread) ----
	Connection _connection;
	SessionState _state = SessionState::Disconnected;
	uint64_t _uuid = 0;                 // this session's id; the same value signs both logon messages
	uint64_t _partyDetailsListId = 0;   // non-zero once the parties are pre-registered; 0 = on-demand
	uint32_t _outboundSeqNo = 1;        // next business message number we will send
	uint32_t _inboundSeqNo = 1;         // next business message number we expect from CME
	uint32_t _replayBelowSeq = 0;       // set at resume: replayed messages carry numbers below this
	uint32_t _recoverNextFrom = 0;      // where the next recovery batch starts
	uint32_t _recoverEnd = 0;           // one past the last missed number the recovery must cover
	uint32_t _batchEnd = 0;             // one past the outstanding batch's last number; 0 = none out
	uint32_t _reconnectAttempts = 0;    // alternates the primary/backup address across attempts
	uint64_t _nextOrderRequestId = 1;   // session-unique order-request ids for every router on this session
	OrderRequestRing _orderRequests;    // what each in-flight request was for, until its response takes it

	// CME serves at most this many messages per retransmit request, one request in flight at
	// a time — a bigger gap recovers as consecutive batches.
	static constexpr uint32_t RetransmitBatchLimit = 2500;

	// The most held-back bytes a recovery may accumulate before it is abandoned for liveness.
	static constexpr size_t HoldbackLimitBytes = 1 << 20;
	uint32_t _establishRejects = 0;     // consecutive Establish rejections of the resumed session

	// A resumed session is discarded only after this many consecutive Establish rejections:
	// the common cause is CME still holding the dropped connection (the session is fine and a
	// retry succeeds), and discarding early abandons the recovery replay queued on it.
	static constexpr uint32_t DiscardSessionAfterRejects = 5;
	int64_t _lastSendNanos = 0;         // when we last sent anything, for the keep-alive clock
	SessionStore _sessionStore;         // when open, the session id and counters survive restarts

	// ---- buffers reused every message (no per-message allocation) ----
	std::array<uint8_t, 512> _sendBuffer{};
	std::vector<uint8_t> _recvBuffer;
	size_t _consumed = 0;               // how far into _recvBuffer we have already framed
	std::array<uint8_t, 8192> _chunk{};

	// New messages that arrived while a recovery replay was still running, kept whole and in
	// arrival order so the owner sees everything in sequence order: replays first, then
	// these. Only ever touched during a recovery — empty otherwise.
	std::vector<uint8_t> _holdback;

public:
	// Called for each received business message (execution reports and the like). Session-layer
	// messages are handled internally and never reach here. Wired to the order side later.
	std::function<void(const FramedMessage&)> OnBusinessMessage;

	// Build a gateway for one market segment. Looks up that segment's addresses in the config.
	// A session directory makes the session persistent: the week's id and sequence counters
	// live in a file there, so a restart resumes the same session and recovers what it missed.
	// Without one (the default) every logon starts a fresh session.
	BasicMarketSegmentGateway(const ILink3Config& config, int32_t marketSegmentId, CmeLogger* logger,
	                          bool useSecondary = false, const std::filesystem::path& sessionDirectory = {})
		: _config(config), _useSecondary(useSecondary), _logger(logger)
	{
		// Step 1: Resolve the market segment; without it there is nothing to connect to.
		const MarketSegment* segment = config.TryFindMarketSegment(marketSegmentId);
		if (segment == nullptr)
			throw std::invalid_argument("MarketSegmentGateway: market segment " + std::to_string(marketSegmentId) + " not in config");
		_segment = *segment;

		// Step 2: Map the persistent session state when asked for.
		if (!sessionDirectory.empty())
			_sessionStore.Open(sessionDirectory, marketSegmentId);

		// Step 3: Size the receive buffer once.
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

	// Log in. A persistent gateway resumes the trading week's session — same id, sequence
	// counters carrying on — which is what CME expects between weekly resets: Negotiate is
	// sent only the first time an id is used, and after the EstablishmentAck any messages CME
	// published while we were away are asked for again. An ephemeral gateway (no session
	// directory) starts a fresh session every time. Returns true once the sequenced session
	// is open.
	bool Logon()
	{
		// Step 1: Adopt the session identity: the stored one when it belongs to this trading
		// week, a fresh one otherwise (both streams from 1). Party registration is per run.
		const bool resuming = _sessionStore.IsOpen() && _sessionStore.SameWeek() && _sessionStore.State().Uuid != 0;
		if (resuming)
		{
			_uuid = _sessionStore.State().Uuid;
			_outboundSeqNo = _sessionStore.State().OutboundSeqNo;
			_inboundSeqNo = _sessionStore.State().InboundSeqNo;
			std::cout << "Resuming session " << _uuid << " (outbound " << _outboundSeqNo
			          << ", inbound " << _inboundSeqNo << ").\n";
		}
		else
		{
			_uuid = MakeUuid();
			_outboundSeqNo = 1;
			_inboundSeqNo = 1;
			if (_sessionStore.IsOpen())
				_sessionStore.BeginWeek(_uuid);
		}
		_partyDetailsListId = 0;
		_nextOrderRequestId = MakeUuid();   // clock-seeded: unique across restarts of the same session
		_replayBelowSeq = 0;
		_recoverNextFrom = 0;
		_recoverEnd = 0;
		_batchEnd = 0;
		_holdback.clear();

		// Step 2: Negotiate — but only once per session id: a resumed id that CME already
		// confirmed skips straight to Establish. If our record says unconfirmed yet CME
		// disagrees (a crash between its answer and our note of it), carry on to Establish.
		FramedMessage message{};
		if (!resuming || _sessionStore.State().Negotiated == 0)
		{
			SendFramed(EncodeNegotiate(_config, _uuid, RequestTimestampNow(), _sendBuffer));
			if (!ReceiveMessage(message))
				return false;
			if (message.TemplateId == NegotiationResponse::TemplateId)
			{
				if (_sessionStore.IsOpen())
					_sessionStore.State().Negotiated = 1;
			}
			else if (resuming && message.TemplateId == NegotiationReject::TemplateId)
			{
				std::cout << "Negotiate rejected for resumed session (likely already negotiated); trying Establish: "
				          << message.As<NegotiationReject>()->ToString() << "\n";
			}
			else
			{
				ReportUnexpected("Negotiate", message);
				return false;
			}
		}

		// Step 3: Send Establish carrying our next outbound number and require an
		// EstablishmentAck. A rejected resume is retried on the SAME session — the reject's
		// commonest cause is CME still holding the dropped connection, and the recovery replay
		// is queued on that session; only a persistent string of rejections means it is truly
		// gone, and only then is it discarded for a fresh start.
		SendFramed(EncodeEstablish(_config, _uuid, RequestTimestampNow(), _outboundSeqNo, _sendBuffer));
		if (!ReceiveMessage(message))
			return false;
		if (message.TemplateId != EstablishmentAck::TemplateId)
		{
			std::cout << "Establish got " << ToObjectType(message.TemplateId) << ": "
			          << ToJsonLine(message.TemplateId, message.Body) << "\n";
			if (resuming && ++_establishRejects >= DiscardSessionAfterRejects)
			{
				std::cout << "Discarding the stored session after " << _establishRejects
				          << " rejections and starting the week over.\n";
				_establishRejects = 0;
				_sessionStore.Clear();
				return Logon();
			}
			return false;
		}
		_establishRejects = 0;

		// Step 4: Session is open (the keep-alive clock starts from this last send). The ack
		// names CME's next outbound number; on a resume, anything between our counter and it
		// was published while we were away — ask for all of it again, batch by batch. The
		// recovered copies replay their original numbers, so the live counter jumps ahead to
		// the ack's now, and replays are recognized by their lower numbers.
		_state = SessionState::Established;
		const EstablishmentAck* ack = message.As<EstablishmentAck>();
		if (resuming && ack->NextSeqNo > _inboundSeqNo)
		{
			std::cout << "Missed " << (ack->NextSeqNo - _inboundSeqNo) << " messages while away; recovering.\n";
			_replayBelowSeq = ack->NextSeqNo;
			_recoverNextFrom = _inboundSeqNo;
			_recoverEnd = ack->NextSeqNo;
			RequestNextRetransmitBatch();
			_inboundSeqNo = ack->NextSeqNo;
		}
		PersistSequences();
		return true;
	}

	// Non-zero once a registered party id is in effect; 0 means on-demand mode.
	uint64_t PartyDetailsListId() const { return _partyDetailsListId; }

	// Claim the next order-request id — unique across every order and router on this session
	// (2422 is echoed on every response, so it cannot be the per-order revision) — remembering
	// the caller's tag (the order revision; opaque here) for the echo to resolve. The id space
	// is session-scoped, so its memory is too.
	uint64_t NextOrderRequestId(int32_t tag)
	{
		const uint64_t requestId = _nextOrderRequestId++;
		_orderRequests.Push(requestId, tag);
		return requestId;
	}

	// The tag remembered for an echoed request id, consuming its entry; the fallback when it
	// is gone (already taken, lapped, an unsolicited event, or a zero echo).
	int32_t TagOfOrderRequest(uint64_t requestId, int32_t fallback)
	{
		return _orderRequests.Take(requestId, fallback);
	}

	// Adopt a party id registered elsewhere (on the Order Entry Service Gateway), so every
	// order this session sends references it instead of dragging its own definition.
	void SetPartyDetailsListId(uint64_t partyDetailsListId) { _partyDetailsListId = partyDetailsListId; }

	// Register the trading parties under a fresh id. Meant for a session opened to the Order
	// Entry Service Gateway (the dedicated CME segment that stores party lists for the firm) —
	// a trading gateway only accepts id 0, so registration attempted there is always rejected.
	// The id is minted at the moment of registration, deliberately independent of the session
	// id: session ids persist for the trading week, and a re-registration must never collide
	// with a list CME still holds from an earlier run. On success trading sessions adopt the
	// id via SetPartyDetailsListId; any reply other than an accepted
	// PartyDetailsDefinitionRequestAck (a rejected status, a business reject, or silence)
	// leaves this session at id 0 and returns false.
	bool RegisterPartyDetails()
	{
		// Step 1: Only meaningful on an open session.
		if (_state != SessionState::Established)
			return false;

		// Step 2: Send the definition under a freshly minted id, as a sequenced business message.
		const uint64_t registrationId = MakeUuid();
		const size_t length = EncodePartyDetailsDefinitionRequest(_config.Parties, registrationId, _outboundSeqNo,
			static_cast<uint64_t>(Tools::Timestamp::UtcNow().NanosSinceEpoch), _sendBuffer);
		++_outboundSeqNo;
		PersistSequences();
		SendFramed(length);

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
		PersistSequences();

		// Step 5: Accepted means our id echoed back with status 0; adopt the id.
		if (message.TemplateId == PartyDetailsDefinitionRequestAck::TemplateId)
		{
			const PartyDetailsDefinitionRequestAck* ack = message.As<PartyDetailsDefinitionRequestAck>();
			if (ack->PartyDetailRequestStatus == 0 && ack->PartyDetailsListReqID == registrationId)
			{
				_partyDetailsListId = registrationId;
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
			const size_t length = EncodePartyDetailsDefinitionRequest(_config.Parties, /*partyDetailsListReqId*/ 0,
				_outboundSeqNo, static_cast<uint64_t>(Tools::Timestamp::UtcNow().NanosSinceEpoch), _sendBuffer);
			++_outboundSeqNo;
			PersistSequences();
			SendFramed(length);
		}

		// Step 3: Send the order referencing the session's party id. Stamp the fields the
		// gateway owns: this message's sequence number and send time.
		order.PartyDetailsListReqID = _partyDetailsListId;
		order.SeqNum = _outboundSeqNo;
		order.SendingTimeEpoch = static_cast<uint64_t>(Tools::Timestamp::UtcNow().NanosSinceEpoch);
		const size_t length = FrameMessage(_sendBuffer, NewOrderSingle::TemplateId, NewOrderSingle::BlockLength, &order, sizeof(order), 0);
		++_outboundSeqNo;
		PersistSequences();
		SendFramed(length);
		return _state == SessionState::Established;   // false when the send found the connection dead
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
			const size_t length = EncodePartyDetailsDefinitionRequest(_config.Parties, /*partyDetailsListReqId*/ 0,
				_outboundSeqNo, static_cast<uint64_t>(Tools::Timestamp::UtcNow().NanosSinceEpoch), _sendBuffer);
			++_outboundSeqNo;
			PersistSequences();
			SendFramed(length);
		}

		// Step 3: Send the cancel referencing the session's party id.
		cancel.PartyDetailsListReqID = _partyDetailsListId;
		cancel.SeqNum = _outboundSeqNo;
		cancel.SendingTimeEpoch = static_cast<uint64_t>(Tools::Timestamp::UtcNow().NanosSinceEpoch);
		const size_t length = FrameMessage(_sendBuffer, OrderCancelRequest::TemplateId, OrderCancelRequest::BlockLength, &cancel, sizeof(cancel), 0);
		++_outboundSeqNo;
		PersistSequences();
		SendFramed(length);
		return _state == SessionState::Established;   // false when the send found the connection dead
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
			const size_t length = EncodePartyDetailsDefinitionRequest(_config.Parties, /*partyDetailsListReqId*/ 0,
				_outboundSeqNo, static_cast<uint64_t>(Tools::Timestamp::UtcNow().NanosSinceEpoch), _sendBuffer);
			++_outboundSeqNo;
			PersistSequences();
			SendFramed(length);
		}

		// Step 3: Send the replace referencing the session's party id.
		replace.PartyDetailsListReqID = _partyDetailsListId;
		replace.SeqNum = _outboundSeqNo;
		replace.SendingTimeEpoch = static_cast<uint64_t>(Tools::Timestamp::UtcNow().NanosSinceEpoch);
		const size_t length = FrameMessage(_sendBuffer, OrderCancelReplaceRequest::TemplateId, OrderCancelReplaceRequest::BlockLength, &replace, sizeof(replace), 0);
		++_outboundSeqNo;
		PersistSequences();
		SendFramed(length);
		return _state == SessionState::Established;   // false when the send found the connection dead
	}

	// One service pass, called in a loop by the owning thread: take in whatever has arrived,
	// then send a heartbeat if we have been silent too long. A dropped session has nothing to
	// service — the owner's reconnect path is in charge until it is back.
	void Poll()
	{
		// Step 1: Nothing to do without a connection.
		if (_state == SessionState::Disconnected)
			return;

		// Step 2: Drain any messages that have arrived.
		ReceiveAvailable();

		// Step 3: Keep the session alive.
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

	// One reconnect attempt for a dropped session: alternate between the primary and backup
	// gateway addresses and log in again. A persistent gateway resumes the week's session and
	// recovers what it missed; the owner paces the attempts. Returns true once the session is
	// open again.
	bool TryReconnect()
	{
		// Step 1: Nothing to do on a live session; otherwise start from a clean connection.
		if (_state == SessionState::Established)
			return true;
		MarkConnectionDead();

		// Step 2: Pick the address: the configured preference first, the other side on every
		// second attempt (when a backup exists).
		bool secondary = _useSecondary;
		if (!_segment.SecondaryIPAddress.empty() && (_reconnectAttempts % 2) == 1)
			secondary = !secondary;
		++_reconnectAttempts;
		const std::string ip = secondary ? _segment.SecondaryIPAddress : _segment.PrimaryIPAddress;

		// Step 3: Connect and log in; any failure just reports false and the owner retries.
		try
		{
			_connection.Connect(ip, _config.Port, /*recvTimeoutSeconds*/ 1);
			_state = SessionState::Connected;
			if (!Logon())
			{
				MarkConnectionDead();
				return false;
			}
			std::cout << "Reconnected to " << ip << " (segment " << _segment.MarketSegmentID << ").\n";
			return true;
		}
		catch (const std::exception& error)
		{
			std::cout << "Reconnect to " << ip << " failed: " << error.what() << "\n";
			MarkConnectionDead();
			return false;
		}
	}

	// True while a resume's recovery is still owed messages (drains via Poll).
	bool RecoveryOutstanding() const { return _batchEnd != 0; }

	// Test hook: kill the connection without a Terminate, exactly as a network failure would,
	// so the reconnect path can be exercised. May be called from another thread; the owning
	// thread's next receive fails and lands in the same dead-connection handling.
	void DropConnection()
	{
		_connection.Close();
		_state = SessionState::Disconnected;
	}

private:
	// Send a framed message (already written into _sendBuffer) and log it strictly after the
	// send returns. Also restarts the keep-alive clock, since any send counts as activity.
	void SendFramed(size_t length)
	{
		// Step 1: Hand the bytes to the connection. A dead connection surfaces here: mark the
		// session down for the owner's reconnect path. The sequence counters were persisted
		// ahead of this write, so an unsent message resolves as a benign gap after reconnect.
		try
		{
			_connection.SendAll(std::span<const uint8_t>(_sendBuffer.data(), length));
		}
		catch (const std::exception& error)
		{
			std::cout << "Connection lost on send: " << error.what() << "\n";
			MarkConnectionDead();
			return;
		}

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
		// Step 1: One read. A timeout (-1) just means nothing arrived this pass; a peer close
		// or a read error means the connection is gone.
		ssize_t n;
		try
		{
			n = _connection.Recv(_chunk);
		}
		catch (const std::exception& error)
		{
			std::cout << "Connection lost on receive: " << error.what() << "\n";
			MarkConnectionDead();
			return;
		}
		if (n <= 0)
		{
			if (n == 0)
				MarkConnectionDead();   // peer closed
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

	// The connection is gone: close it, mark the session down, and throw away any half
	// received bytes — a partial message from the dead connection must never be framed
	// against the next one.
	void MarkConnectionDead()
	{
		_connection.Close();
		_state = SessionState::Disconnected;
		_recvBuffer.clear();
		_consumed = 0;
	}

	// Ask for the next slice of the missed messages, or note the recovery complete. CME
	// serves one bounded request at a time; the last message of each batch triggers the next.
	void RequestNextRetransmitBatch()
	{
		if (_recoverNextFrom >= _recoverEnd)
		{
			_batchEnd = 0;
			std::cout << "Recovery complete.\n";
			FlushHoldback();
			return;
		}
		const uint32_t count = std::min(_recoverEnd - _recoverNextFrom, RetransmitBatchLimit);
		SendFramed(EncodeRetransmitRequest(_uuid, _recoverNextFrom, static_cast<uint16_t>(count), _sendBuffer));
		_batchEnd = _recoverNextFrom + count;
		_recoverNextFrom = _batchEnd;
	}

	// Deliver the new messages that waited behind the recovery — oldest first, counted and
	// persisted one by one, exactly as if they had just arrived.
	void FlushHoldback()
	{
		size_t consumed = 0;
		FramedMessage message{};
		while (TryFrame(std::span<const uint8_t>(_holdback).subspan(consumed), message))
		{
			if (OnBusinessMessage)
				OnBusinessMessage(message);
			++_inboundSeqNo;
			PersistSequences();
			consumed += message.TotalLength;
		}
		_holdback.clear();
	}

	// Mirror the sequence counters into the persistent store. Callers advance a counter and
	// persist BEFORE the message it numbers reaches the wire: the stored value can then only
	// ever run ahead of what CME saw, and a crash replays as a benign gap (which NotApplied
	// resolves) instead of a duplicated sequence number (which kills the session).
	void PersistSequences()
	{
		if (!_sessionStore.IsOpen())
			return;
		_sessionStore.State().OutboundSeqNo = _outboundSeqNo;
		_sessionStore.State().InboundSeqNo = _inboundSeqNo;
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
			case NotApplied::TemplateId:
			{
				// CME saw a gap in OUR numbering (messages recorded as sent that never made
				// the wire, e.g. across a crash). Business messages are never re-sent — a
				// Sequence carrying our current next number tells CME to accept the gap.
				std::cout << "CME NotApplied (filling the gap): " << message.As<NotApplied>()->ToString() << "\n";
				SendKeepAlive();
				break;
			}
			case Retransmission::TemplateId:
			{
				// CME granting a retransmit request; the recovered messages follow. The grant
				// is authoritative about what will actually replay — a short grant (messages
				// no longer available) would otherwise leave the batch waiting forever.
				const Retransmission* grant = message.As<Retransmission>();
				std::cout << "CME Retransmission: " << grant->ToString() << "\n";
				const uint32_t grantEnd = grant->FromSeqNo + grant->MsgCount;
				if (_batchEnd != 0 && grantEnd != _batchEnd)
				{
					std::cout << "Recovery batch granted short of the request; the shortfall stays missed.\n";
					if (grant->MsgCount == 0)
					{
						_recoverNextFrom = _recoverEnd;
						_batchEnd = 0;
						FlushHoldback();
					}
					else
						_batchEnd = grantEnd;
				}
				break;
			}
			case RetransmitReject::TemplateId:
			{
				// The recovery request failed — anything missed stays missed; make it loud.
				std::cout << "CME RetransmitReject (missed messages NOT recovered): "
				          << message.As<RetransmitReject>()->ToString() << "\n";
				_recoverNextFrom = _recoverEnd;
				_batchEnd = 0;
				FlushHoldback();
				break;
			}
			default:
			{
				// A business message (execution report and the like). Every one begins with
				// its own number, so a recovery replay (below the resume point) is told apart
				// from new flow exactly. The owner sees everything in sequence order: replays
				// deliver at once (they are the oldest, and are never counted — the live
				// counter jumped past them at logon; the one finishing a batch asks for the
				// next); new messages landing mid-recovery wait their turn in the holdback,
				// uncounted until delivered, so a crash re-recovers rather than loses them.
				uint32_t sequenceNumber = 0;
				std::memcpy(&sequenceNumber, message.Body, sizeof(sequenceNumber));
				if (_replayBelowSeq != 0 && sequenceNumber < _replayBelowSeq)
				{
					if (OnBusinessMessage)
						OnBusinessMessage(message);
					if (_batchEnd != 0 && sequenceNumber + 1 >= _batchEnd)
						RequestNextRetransmitBatch();
				}
				else if (RecoveryOutstanding())
				{
					const uint8_t* frame = message.Body - MessagePrefixLength;
					_holdback.insert(_holdback.end(), frame, frame + message.TotalLength);

					// Liveness valve: a recovery CME never finishes must not dam the flow
					// forever. Give up on ordering, loudly, rather than starve the owner.
					if (_holdback.size() > HoldbackLimitBytes)
					{
						std::cout << "Recovery stalled with " << _holdback.size()
						          << " bytes held back; abandoning the replay to keep the flow alive.\n";
						_recoverNextFrom = _recoverEnd;
						_batchEnd = 0;
						FlushHoldback();
					}
				}
				else
				{
					if (OnBusinessMessage)
						OnBusinessMessage(message);
					++_inboundSeqNo;
					PersistSequences();
				}
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
			ssize_t n;
			try
			{
				n = _connection.Recv(_chunk);
			}
			catch (const std::exception& error)
			{
				std::cout << "Connection lost on receive: " << error.what() << "\n";
				MarkConnectionDead();
				return false;
			}
			if (n == 0)
			{
				MarkConnectionDead();
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

// The kernel-socket form: bring-up, certification, and every environment reached over an
// ordinary network path.
using MarketSegmentGateway = BasicMarketSegmentGateway<TcpConnection>;

} // namespace ILink3
