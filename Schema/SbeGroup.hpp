#pragma once

// Repeating-group support shared by the generated iLink 3 and MDP 3.0 headers.
//
// A message is a fixed root block followed by zero or more repeating groups laid end to end.
// Each group on the wire is a three-byte dimension header — the size of one entry and the
// number of entries — then that many fixed-size entries. This file provides the reader that
// walks a group and a helper that writes one, both generic over the entry struct the code
// generator produces for that group.
//
// One subtlety that matters for schema upgrades: entries are stepped over using the entry
// size read from the wire, not the size of our compiled struct. If CME lengthens an entry in
// a newer schema, older code still walks it correctly and simply ignores the extra bytes.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace Sbe
{

// The three-byte dimension header in front of every repeating group.
#pragma pack(push, 1)
struct GroupSize
{
	uint16_t BlockLength;   // size in bytes of one entry
	uint8_t NumInGroup;     // number of entries that follow
};
#pragma pack(pop)
static_assert(sizeof(GroupSize) == 3);

// Reads one repeating group out of the wire bytes. Construct it pointing at the group's
// dimension header; it then exposes the entry count and each entry as the generated struct T,
// plus a pointer to just past the group so the next group can be found.
template <typename T>
class GroupReader
{
	const uint8_t* _at;   // points at this group's dimension header

public:
	explicit GroupReader(const uint8_t* at) : _at(at) {}

	// This group's dimension header.
	const GroupSize& Dimension() const { return *reinterpret_cast<const GroupSize*>(_at); }

	// How many entries this group holds.
	uint16_t Count() const { return Dimension().NumInGroup; }

	// The byte length of one entry as stated on the wire (the stride between entries).
	uint16_t EntryLength() const { return Dimension().BlockLength; }

	// Entry `index`, read straight out of the buffer.
	const T& operator[](uint16_t index) const
	{
		return *reinterpret_cast<const T*>(_at + sizeof(GroupSize) + static_cast<size_t>(index) * EntryLength());
	}

	// One byte past the whole group — where the next group (if any) begins.
	const uint8_t* End() const
	{
		return _at + sizeof(GroupSize) + static_cast<size_t>(Count()) * EntryLength();
	}

	// Range-based-for support: `for (const auto& entry : reader)`.
	class Iterator
	{
		const GroupReader* _reader;
		uint16_t _index;

	public:
		Iterator(const GroupReader* reader, uint16_t index) : _reader(reader), _index(index) {}
		const T& operator*() const { return (*_reader)[_index]; }
		Iterator& operator++() { ++_index; return *this; }
		bool operator!=(const Iterator& other) const { return _index != other._index; }
	};

	Iterator begin() const { return Iterator(this, 0); }
	Iterator end() const { return Iterator(this, Count()); }
};

// Write a group into a buffer at `dst`: its dimension header (entry size and count) followed
// by `count` copies of the entry struct. Returns the number of bytes written.
template <typename T>
inline size_t WriteGroup(uint8_t* dst, const T* entries, uint8_t count)
{
	// Step 1: Write the dimension header.
	GroupSize dimension{static_cast<uint16_t>(sizeof(T)), count};
	std::memcpy(dst, &dimension, sizeof(dimension));

	// Step 2: Write the entries (if any).
	if (count > 0)
		std::memcpy(dst + sizeof(dimension), entries, static_cast<size_t>(count) * sizeof(T));

	return sizeof(dimension) + static_cast<size_t>(count) * sizeof(T);
}

} // namespace Sbe
