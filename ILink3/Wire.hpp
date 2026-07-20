#pragma once

// Turns iLink 3 messages into the exact bytes that go on the connection, and locates one
// complete message inside a receive buffer. This is the boundary between our typed structs
// and the raw stream.
//
// Every message on the connection has the same shape, all little-endian:
//   [framing header : 2-byte total length, 2-byte encoding marker 0xCAFE ]
//   [message header : 2-byte block length, 2-byte template id, 2-byte schema id, version ]
//   [fixed body     : the generated struct for that template; its size == the block length ]
//   [trailing data  : repeating groups or variable-length fields, when a message has them ]
// The framing header's total length counts the whole message including its own four bytes
// (checked against CME's own worked example: 128 = 4 + 8 + 116).
//
// The two logon messages (Negotiate and Establish) end with a variable-length Credentials
// field that CME does not use yet but requires to be present with length zero — two 0x00
// bytes appended after the body.

#include "ILink3Config.hpp"
#include "ILink3Sbe.hpp"
#include "Hmac.hpp"
#include "Timestamp.hpp"

#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>

namespace ILink3
{

inline constexpr size_t FramingHeaderLength = sizeof(SimpleOpenFramingHeader);              // 4
inline constexpr size_t MessagePrefixLength = FramingHeaderLength + sizeof(MessageHeader);  // 12

// Write framing header + message header + body + a run of trailing zero bytes into `dst`.
// Returns the total number of bytes written.
inline size_t FrameMessage(std::span<uint8_t> dst, uint16_t templateId, uint16_t blockLength,
                           const void* body, size_t bodyLength, size_t trailingZeros)
{
	// Step 1: Work out the full size and make sure it fits in the caller's buffer.
	const size_t total = MessagePrefixLength + bodyLength + trailingZeros;
	if (dst.size() < total)
		throw std::runtime_error("ILink3::FrameMessage: destination buffer too small");

	// Step 2: Write the framing header — total length (including these four bytes) and the
	// fixed encoding marker.
	SimpleOpenFramingHeader framing{};
	framing.MessageLength = static_cast<uint16_t>(total);
	framing.EncodingType = SbeEncodingType;   // 0xCAFE
	std::memcpy(dst.data(), &framing, FramingHeaderLength);

	// Step 3: Write the message header — which template this is and which schema decodes it.
	MessageHeader header{};
	header.BlockLength = blockLength;
	header.TemplateId = templateId;
	header.SchemaId = SchemaId;            // 8
	header.Version = SchemaVersion;        // 9
	std::memcpy(dst.data() + FramingHeaderLength, &header, sizeof(MessageHeader));

	// Step 4: Copy the message body straight after the two headers.
	std::memcpy(dst.data() + MessagePrefixLength, body, bodyLength);

	// Step 5: Zero the trailing bytes (the empty Credentials field on the logon messages).
	if (trailingZeros > 0)
		std::memset(dst.data() + MessagePrefixLength + bodyLength, 0, trailingZeros);

	return total;
}

// Make a session identifier: microseconds since 1970. CME asks that it strictly increase
// through the week, and the current time naturally does.
inline uint64_t MakeUuid()
{
	return static_cast<uint64_t>(Tools::Timestamp::UtcNow().NanosSinceEpoch) / 1000ULL;
}

// Current time in nanoseconds since 1970 for a message's request timestamp; CME rejects one
// more than five seconds old.
inline uint64_t RequestTimestampNow()
{
	return static_cast<uint64_t>(Tools::Timestamp::UtcNow().NanosSinceEpoch);
}

// Build a fully signed, framed Negotiate message ready to send. Returns bytes written.
inline size_t EncodeNegotiate(const ILink3Config& config, uint64_t uuid, uint64_t requestTimestamp, std::span<uint8_t> dst)
{
	// Step 1: Pull the session and firm identity from the config.
	const std::string session = config.SessionID.ToString();
	const std::string firm = config.GFID.ToString();

	// Step 2: Build the signing string for these values and sign it with the account secret.
	const std::string canonical = Hmac::BuildNegotiateCanonicalMessage(requestTimestamp, uuid, session, firm);
	const std::array<uint8_t, 32> signature = Hmac::Sign(config.SecretKey.Value, canonical);

	// Step 3: Fill the message body, copying the raw signature bytes into its signature field.
	Negotiate negotiate{};
	std::memcpy(negotiate.HMACSignature.Chars, signature.data(), signature.size());
	negotiate.AccessKeyID = config.AccessID;
	negotiate.UUID = uuid;
	negotiate.RequestTimestamp = requestTimestamp;
	negotiate.Session = session;
	negotiate.Firm = firm;

	// Step 4: Frame it, appending the two-byte empty Credentials field CME requires.
	return FrameMessage(dst, Negotiate::TemplateId, Negotiate::BlockLength, &negotiate, sizeof(negotiate), sizeof(uint16_t));
}

// Build a fully signed, framed Establish message ready to send. Returns bytes written.
inline size_t EncodeEstablish(const ILink3Config& config, uint64_t uuid, uint64_t requestTimestamp, uint32_t nextSeqNo, std::span<uint8_t> dst)
{
	// Step 1: Pull the session and firm identity from the config.
	const std::string session = config.SessionID.ToString();
	const std::string firm = config.GFID.ToString();

	// Step 2: Build the (longer) signing string and sign it with the account secret.
	const std::string canonical = Hmac::BuildEstablishCanonicalMessage(
		requestTimestamp, uuid, session, firm, config.TradingSystem, nextSeqNo, config.KeepAliveIntervalMs);
	const std::array<uint8_t, 32> signature = Hmac::Sign(config.SecretKey.Value, canonical);

	// Step 3: Fill the message body, including the trading-system identity and session fields.
	Establish establish{};
	std::memcpy(establish.HMACSignature.Chars, signature.data(), signature.size());
	establish.AccessKeyID = config.AccessID;
	establish.TradingSystemName = config.TradingSystem.Name;
	establish.TradingSystemVersion = config.TradingSystem.Version;
	establish.TradingSystemVendor = config.TradingSystem.Vendor;
	establish.UUID = uuid;
	establish.RequestTimestamp = requestTimestamp;
	establish.NextSeqNo = nextSeqNo;
	establish.Session = session;
	establish.Firm = firm;
	establish.KeepAliveInterval = config.KeepAliveIntervalMs;

	// Step 4: Frame it, appending the two-byte empty Credentials field CME requires.
	return FrameMessage(dst, Establish::TemplateId, Establish::BlockLength, &establish, sizeof(establish), sizeof(uint16_t));
}

// Build a Limit-order NewOrderSingle with every optional field set to its "not present"
// value. Getting these sentinels wrong is a classic source of rejects, so they are set once
// here: a zero would be read as a real price or quantity. Price is in units of ten-to-the
// minus-nine. The gateway fills SeqNum and SendingTimeEpoch when it sends, so they are left
// zero here.
inline NewOrderSingle NewLimitOrder(int32_t securityId, SideReq side, uint32_t quantity, int64_t priceMantissa,
                                    const std::string& clOrdId, const std::string& senderId,
                                    uint64_t partyDetailsListReqId, uint64_t orderRequestId, const std::string& location)
{
	// Step 1: The "absent" values for the optional fields.
	constexpr int64_t PriceNull = INT64_MAX;          // PRICENULL9 not present
	constexpr uint32_t QuantityNull = UINT32_MAX;     // optional quantity not present
	constexpr uint16_t DateNull = 65535;              // optional date not present
	constexpr uint8_t ByteNull = 255;                 // optional single-byte enum not present

	// Step 2: The order the strategy actually wants.
	NewOrderSingle order{};
	order.Price.Mantissa = priceMantissa;
	order.OrderQty = quantity;
	order.SecurityID = securityId;
	order.Side = side;
	order.SenderID = senderId;
	order.ClOrdID = clOrdId;
	order.PartyDetailsListReqID = partyDetailsListReqId;
	order.OrderRequestID = orderRequestId;
	order.Location = location;
	order.OrdType = OrderTypeReq::Limit;
	order.TimeInForce = TimeInForce::Day;
	order.ManualOrderIndicator = ManualOrdIndReq::Automated;

	// Step 3: Everything optional we are not using, set to "not present".
	order.StopPx.Mantissa = PriceNull;
	order.MinQty = QuantityNull;
	order.DisplayQty = QuantityNull;
	order.ExpireDate = DateNull;
	order.ExecInst.Value = 0;
	order.ExecutionMode = static_cast<ExecMode>(0);           // absent -> aggressive default
	order.LiquidityFlag = static_cast<BooleanNULL>(ByteNull);
	order.ManagedOrder = static_cast<BooleanNULL>(ByteNull);
	order.ShortSaleType = static_cast<ShortSaleType>(ByteNull);
	order.DiscretionPrice.Mantissa = PriceNull;
	order.ReservationPrice.Mantissa = PriceNull;
	return order;
}

// Build an OrderCancelRequest to pull a working order. It references the order by the exchange's
// own id (returned on the acceptance report), so it needs no party details of its own. The
// gateway fills SeqNum and SendingTimeEpoch when it sends.
inline OrderCancelRequest NewCancel(int32_t securityId, SideReq side, uint64_t exchangeOrderId,
                                    const std::string& clOrdId, const std::string& senderId,
                                    uint64_t orderRequestId, const std::string& location)
{
	OrderCancelRequest cancel{};
	cancel.OrderID = exchangeOrderId;
	cancel.PartyDetailsListReqID = 0;                       // references an existing order; no parties
	cancel.ManualOrderIndicator = ManualOrdIndReq::Automated;
	cancel.SenderID = senderId;
	cancel.ClOrdID = clOrdId;
	cancel.OrderRequestID = orderRequestId;
	cancel.Location = location;
	cancel.SecurityID = securityId;
	cancel.Side = side;
	cancel.LiquidityFlag = static_cast<BooleanNULL>(255);   // not present
	return cancel;
}

// Build an OrderCancelReplaceRequest to move a working order to a new price and/or size. It
// references the order by the exchange's own id, with every optional field set to its "not
// present" value. The gateway fills SeqNum and SendingTimeEpoch when it sends.
inline OrderCancelReplaceRequest NewReplace(int32_t securityId, SideReq side, uint64_t exchangeOrderId,
                                            uint32_t quantity, int64_t priceMantissa,
                                            const std::string& senderId, uint64_t orderRequestId,
                                            const std::string& location)
{
	// Step 1: The "absent" values for the optional fields.
	constexpr int64_t PriceNull = INT64_MAX;
	constexpr uint32_t QuantityNull = UINT32_MAX;
	constexpr uint16_t DateNull = 65535;
	constexpr uint8_t ByteNull = 255;

	// Step 2: The new price and size, referencing the working order.
	OrderCancelReplaceRequest replace{};
	replace.Price.Mantissa = priceMantissa;
	replace.OrderQty = quantity;
	replace.SecurityID = securityId;
	replace.Side = side;
	replace.SenderID = senderId;
	replace.OrderID = exchangeOrderId;
	replace.OrderRequestID = orderRequestId;
	replace.Location = location;
	replace.OrdType = OrderTypeReq::Limit;
	replace.TimeInForce = TimeInForce::Day;
	replace.ManualOrderIndicator = ManualOrdIndReq::Automated;
	replace.OFMOverride = OFMOverrideReq::Disabled;   // keep the order's queue position when possible

	// Step 3: Everything optional we are not using, set to "not present".
	replace.StopPx.Mantissa = PriceNull;
	replace.MinQty = QuantityNull;
	replace.DisplayQty = QuantityNull;
	replace.ExpireDate = DateNull;
	replace.ExecInst.Value = 0;
	replace.ExecutionMode = static_cast<ExecMode>(0);
	replace.LiquidityFlag = static_cast<BooleanNULL>(ByteNull);
	replace.ManagedOrder = static_cast<BooleanNULL>(ByteNull);
	replace.ShortSaleType = static_cast<ShortSaleType>(ByteNull);
	replace.DiscretionPrice.Mantissa = PriceNull;
	return replace;
}

// Build a framed PartyDetailsDefinitionRequest(518): registers the trading parties (firm,
// account, operator, and clearing firm on a give-up) under `partyDetailsListReqId`, which
// every order then references. The caller supplies the sequence number and send time. Returns
// bytes written.
inline size_t EncodePartyDetailsDefinitionRequest(const PartyDetailsConfig& parties, uint64_t partyDetailsListReqId,
                                                  uint32_t seqNo, uint64_t sendingTimeEpoch, std::span<uint8_t> dst)
{
	// Step 1: Fill the root block. Optional fields we are not using get their null values.
	constexpr uint64_t UInt64Null = UINT64_MAX;
	PartyDetailsDefinitionRequest root{};
	root.PartyDetailsListReqID = partyDetailsListReqId;
	root.SendingTimeEpoch = sendingTimeEpoch;
	root.ListUpdateAction = ListUpdAct::Add;
	root.SeqNum = seqNo;

	//Information required to accept and match the order on the central limit order book 
	root.SelfMatchPreventionID = parties.SelfMatchPreventionID;
	root.SelfMatchPreventionInstruction = static_cast<SMPI>(parties.SelfMatchPreventionInstruction);

	//Information required to clear the trade 
	root.CustOrderHandlingInst = static_cast<CustOrdHandlInst>(parties.CustOrderHandlingInst);
	root.AvgPxIndicator = static_cast<AvgPxInd>(parties.AvgPxIndicator);
	root.ClearingTradePriceType = static_cast<SLEDS>(parties.ClearingTradePriceType);
	root.CmtaGiveupCD = parties.TakeUpFirm.empty() ? static_cast<CmtaGiveUpCD>(0) : CmtaGiveUpCD::GiveUp;

	

	root.CustOrderCapacity = static_cast<CustOrderCapacity>(parties.CustOrderCapacity);
	root.ClearingAccountType = static_cast<ClearingAcctType>(parties.ClearingAccountType);
	root.Executor = UInt64Null;
	root.IDMShortCode = UInt64Null;

	// Step 2: Copy the root block to the start of the message body.
	uint8_t body[512];
	std::memcpy(body, &root, sizeof(root));
	size_t pos = PartyDetailsDefinitionRequest::BlockLength;

	// Step 3: Gather the party entries that are set (skip any that are empty). Information
	// required for market regulation compliance, in CME's documented role order: take-up firm
	// and account (give-up only), then executing firm, operator, and customer account.
	PartyDetailsDefinitionRequest_NoPartyDetails entries[5]{};
	uint8_t count = 0;
	auto addParty = [&](const std::string& id, PartyDetailRole role)
	{
		if (id.empty())
			return;
		entries[count].PartyDetailID = id;
		entries[count].PartyDetailRole = role;
		++count;
	};
	addParty(parties.TakeUpFirm, PartyDetailRole::TakeUpFirm);           // 96
	addParty(parties.TakeUpAccount, PartyDetailRole::TakeUpAccount);     // 1000
	addParty(parties.ExecutingFirm, PartyDetailRole::ExecutingFirm);     // 1
	addParty(parties.Operator, PartyDetailRole::Operator);               // 118
	addParty(parties.CustomerAccount, PartyDetailRole::CustomerAccount); // 24

	// Step 4: Append the party group, then an empty regulatory-publications group.
	pos += Sbe::WriteGroup(body + pos, entries, count);
	pos += Sbe::WriteGroup<PartyDetailsDefinitionRequest_NoTrdRegPublications>(body + pos, nullptr, 0);

	// Step 5: Frame the whole body (root + groups); the header block length is the root only.
	return FrameMessage(dst, PartyDetailsDefinitionRequest::TemplateId, PartyDetailsDefinitionRequest::BlockLength, body, pos, 0);
}

// Build a framed Sequence(506): the heartbeat that keeps the session alive, and the way we
// tell CME our next outbound sequence number. Unlike the logon messages it is not signed and
// has no trailing field. Returns bytes written.
inline size_t EncodeSequence(uint64_t uuid, uint32_t nextSeqNo, std::span<uint8_t> dst)
{
	// Step 1: Fill the small body.
	Sequence sequence{};
	sequence.UUID = uuid;
	sequence.NextSeqNo = nextSeqNo;
	sequence.FaultToleranceIndicator = FTI::Primary;
	sequence.KeepAliveIntervalLapsed = KeepAliveLapsed::NotLapsed;

	// Step 2: Frame it (no trailing bytes).
	return FrameMessage(dst, Sequence::TemplateId, Sequence::BlockLength, &sequence, sizeof(sequence), 0);
}

// Build a framed RetransmitRequest(508): ask CME to resend `count` of its business messages
// starting at `fromSeqNo` — the recovery step after a reconnect shows we missed some. Both
// session id fields carry the same id (recovery within the current session). Like Sequence
// it is not signed and has no trailing field. Returns bytes written.
inline size_t EncodeRetransmitRequest(uint64_t uuid, uint32_t fromSeqNo, uint16_t count, std::span<uint8_t> dst)
{
	// Step 1: Fill the small body.
	RetransmitRequest request{};
	request.UUID = uuid;
	request.LastUUID = uuid;
	request.RequestTimestamp = RequestTimestampNow();
	request.FromSeqNo = fromSeqNo;
	request.MsgCount = count;

	// Step 2: Frame it (no trailing bytes).
	return FrameMessage(dst, RetransmitRequest::TemplateId, RetransmitRequest::BlockLength, &request, sizeof(request), 0);
}

// A pointer to one complete message that has been located inside a receive buffer. It does
// not own the bytes — it just marks where the message is and how to read it.
struct FramedMessage
{
	uint16_t TemplateId = 0;
	uint16_t BlockLength = 0;
	const uint8_t* Body = nullptr;   // first byte of the message body (past both headers)
	size_t TotalLength = 0;          // whole message size, so the caller can advance past it

	// Reinterpret the body as a given generated struct. The caller must have checked the
	// template id first, so it is casting to the right type.
	template <typename T>
	const T* As() const
	{
		return reinterpret_cast<const T*>(Body);
	}
};

// Try to locate one whole message at the front of `buf`. Returns false, leaving `out`
// untouched, when the buffer does not yet hold a complete message (read more, then retry).
inline bool TryFrame(std::span<const uint8_t> buf, FramedMessage& out)
{
	// Step 1: Need at least both headers before anything can be read.
	if (buf.size() < MessagePrefixLength)
		return false;

	// Step 2: Read the framing header; its length tells us how big the whole message is.
	SimpleOpenFramingHeader framing{};
	std::memcpy(&framing, buf.data(), FramingHeaderLength);
	if (buf.size() < framing.MessageLength)
		return false;   // whole message not buffered yet

	// Step 3: Read the message header to learn which template this is.
	MessageHeader header{};
	std::memcpy(&header, buf.data() + FramingHeaderLength, sizeof(MessageHeader));

	// Step 4: Point `out` at the body and record what the caller needs to decode and advance.
	out.TemplateId = header.TemplateId;
	out.BlockLength = header.BlockLength;
	out.Body = buf.data() + MessagePrefixLength;
	out.TotalLength = framing.MessageLength;
	return true;
}

} // namespace ILink3
