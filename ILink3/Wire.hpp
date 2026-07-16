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
