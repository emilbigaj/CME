#pragma once

// Walks one market-data datagram. Every datagram is a packet header (the channel's packet
// sequence number and send time) followed by one or more messages, each prefixed by its size:
//
//   [ packet header : 4-byte sequence number, 8-byte send time ]
//   [ message size  : 2 bytes, counting itself + message header + body ]
//   [ message header: 2-byte block length, template id, schema id, version ]
//   [ fixed body + repeating groups ]
//   ... next message until the datagram ends
//
// The walker points at the bytes in place — nothing is copied. The size prefix is the offset
// to the next message, so unknown or newer message kinds are stepped over safely.

#include "Mdp3Sbe.hpp"   // Mdp3::PacketHeader / MessageSize / MessageHeader

#include <cstdint>
#include <span>

namespace Mdp3
{

// One message located inside a datagram: which template it is and where its body starts.
struct MessageView
{
	uint16_t TemplateId = 0;
	uint16_t BlockLength = 0;        // the fixed body's length (groups follow it)
	const uint8_t* Body = nullptr;   // first byte past the message header

	// Reinterpret the body as a generated struct. The caller must have checked TemplateId.
	template <typename T>
	const T* As() const
	{
		return reinterpret_cast<const T*>(Body);
	}
};

// Steps through the messages of one datagram, in place.
class PacketWalker
{
	const uint8_t* _at;
	const uint8_t* _end;
	const PacketHeader* _header;

public:
	// Point at a datagram. Valid() is false for anything too short to carry a packet header.
	explicit PacketWalker(std::span<const uint8_t> datagram)
		: _at(datagram.data() + sizeof(PacketHeader)),
		  _end(datagram.data() + datagram.size()),
		  _header(datagram.size() >= sizeof(PacketHeader)
			? reinterpret_cast<const PacketHeader*>(datagram.data()) : nullptr)
	{
	}

	bool Valid() const { return _header != nullptr; }
	const PacketHeader& Header() const { return *_header; }

	// Locate the next message. Returns false when the datagram is exhausted (or malformed —
	// a size that runs past the end stops the walk rather than reading beyond the buffer).
	bool TryNext(MessageView& message)
	{
		// Step 1: There must be room for the size prefix and the message header.
		if (_header == nullptr || _at + sizeof(MessageSize) + sizeof(MessageHeader) > _end)
			return false;

		// Step 2: Read the size prefix; the whole message must fit in the datagram.
		const MessageSize& size = *reinterpret_cast<const MessageSize*>(_at);
		if (size.Size < sizeof(MessageSize) + sizeof(MessageHeader) || _at + size.Size > _end)
			return false;

		// Step 3: Expose the message and advance by the on-wire size (forward compatible).
		const MessageHeader& header = *reinterpret_cast<const MessageHeader*>(_at + sizeof(MessageSize));
		message.TemplateId = header.TemplateId;
		message.BlockLength = header.BlockLength;
		message.Body = _at + sizeof(MessageSize) + sizeof(MessageHeader);
		_at += size.Size;
		return true;
	}
};

} // namespace Mdp3
