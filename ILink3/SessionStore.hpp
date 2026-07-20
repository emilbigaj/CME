#pragma once

// Persistent session identity for one gateway connection: the week's session id and both
// sequence counters, memory-mapped from a small file so the hot-path updates are plain
// stores into a shared page — no system call, and the values survive a process restart.
// CME expects one session id per trading week, reused across reconnects so the sequenced
// streams continue and missed messages can be recovered; a fresh id belongs only at the
// weekly reset or when this state is lost. The trading week starts Sunday 16:00 Chicago
// time. After a whole-machine crash the mapped page may be stale on disk; the gateway's
// write ordering keeps stored counters running ahead of the wire, never behind, so the
// worst outcome is a benign sequence gap.
//
// File: <root>/Servers/CME/Session/<Environment>/MSGW_<id>.state

#include "ILink3Config.hpp"   // ILink3::Environment

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>

#include <magic_enum.hpp>   // enum -> name for the environment (same source glaze uses)

namespace ILink3
{

// The mapped state itself. A zeroed page (a brand-new file) reads as "no session yet".
struct StoredSession
{
	uint64_t Uuid = 0;             // this week's session id; 0 = none started
	uint32_t OutboundSeqNo = 0;    // next business message number we will send
	uint32_t InboundSeqNo = 0;     // next business message number we expect from CME
	uint32_t Negotiated = 0;       // 1 once CME confirmed the Negotiate for this id
	int64_t WeekStartSeconds = 0;  // the trading week this state belongs to
};

class SessionStore
{
	StoredSession* _state = nullptr;
	int _fd = -1;

public:
	SessionStore() = default;
	SessionStore(const SessionStore&) = delete;
	SessionStore& operator=(const SessionStore&) = delete;

	// The conventional directory for an environment's session files.
	static std::filesystem::path Directory(const std::filesystem::path& root, ILink3::Environment environment)
	{
		return root / "Servers" / "CME" / "Session" / std::string(magic_enum::enum_name(environment));
	}

	// Map the segment's state file, creating it (zeroed, = fresh) when absent.
	void Open(const std::filesystem::path& directory, int32_t marketSegmentId)
	{
		// Step 1: Make sure the directory exists and open (or create) the file.
		std::filesystem::create_directories(directory);
		const std::filesystem::path filePath = directory / ("MSGW_" + std::to_string(marketSegmentId) + ".state");
		_fd = ::open(filePath.c_str(), O_RDWR | O_CREAT, 0644);
		if (_fd < 0)
			throw std::runtime_error("SessionStore: cannot open " + filePath.string());

		// Step 2: Size it to one page; a fresh file arrives zero-filled.
		static_assert(sizeof(StoredSession) <= 4096);
		if (::ftruncate(_fd, 4096) != 0)
		{
			::close(_fd);
			_fd = -1;
			throw std::runtime_error("SessionStore: cannot size " + filePath.string());
		}

		// Step 3: Map it shared, so every update lands in the file-backed page directly.
		void* mapped = ::mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, _fd, 0);
		if (mapped == MAP_FAILED)
		{
			::close(_fd);
			_fd = -1;
			throw std::runtime_error("SessionStore: cannot map " + filePath.string());
		}
		_state = static_cast<StoredSession*>(mapped);
	}

	// True once a file is mapped; everything else requires it.
	bool IsOpen() const { return _state != nullptr; }

	// The live state, written in place.
	StoredSession& State() { return *_state; }

	// True when the stored state belongs to the current trading week.
	bool SameWeek() const { return _state->WeekStartSeconds == CurrentWeekStartSeconds(); }

	// Start the week over with a fresh session id and both streams at 1.
	void BeginWeek(uint64_t uuid)
	{
		*_state = StoredSession{};
		_state->Uuid = uuid;
		_state->OutboundSeqNo = 1;
		_state->InboundSeqNo = 1;
		_state->WeekStartSeconds = CurrentWeekStartSeconds();
	}

	// Forget the stored session entirely (CME no longer recognizes it).
	void Clear() { *_state = StoredSession{}; }

	// When the current trading week began: the most recent Sunday 16:00 in Chicago.
	static int64_t CurrentWeekStartSeconds()
	{
		// Step 1: The current wall clock in Chicago.
		const std::chrono::time_zone* chicago = std::chrono::locate_zone("America/Chicago");
		const auto localNow = chicago->to_local(std::chrono::system_clock::now());

		// Step 2: Walk back to this week's Sunday 16:00; earlier in the day (or week) than
		// that means the week began the Sunday before.
		const auto today = std::chrono::floor<std::chrono::days>(localNow);
		const std::chrono::weekday weekday{today};
		auto weekStart = today - std::chrono::days{weekday.c_encoding()} + std::chrono::hours{16};
		if (localNow < weekStart)
			weekStart -= std::chrono::days{7};

		// Step 3: Return it on the system clock, immune to local-time ambiguity.
		return std::chrono::duration_cast<std::chrono::seconds>(
			chicago->to_sys(weekStart).time_since_epoch()).count();
	}

	// Unmap and close on destruction.
	~SessionStore()
	{
		if (_state != nullptr)
			::munmap(_state, 4096);
		if (_fd >= 0)
			::close(_fd);
	}
};

} // namespace ILink3
