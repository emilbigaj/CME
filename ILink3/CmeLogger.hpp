#pragma once

// Records every message we send to and receive from CME, without slowing the trading path.
//
// The trick is to split the work across two threads. The session thread — the one that owns
// the connection — does the bare minimum: it copies the raw message bytes and a timestamp
// into a lock-free queue and moves on (tens of nanoseconds, no formatting, no file access).
// A separate background thread, pinned to non-trading cores, drains that queue whenever it
// has spare time, decodes each message into readable text, and appends it to a daily file.
//
// There is one queue per connection, and both directions (send and receive) share it with a
// one-byte flag, so the file reads in true processing order: a message we sent, then the
// reply, exactly as they happened. If the background thread ever falls behind and the queue
// fills, the session thread drops the log entry and counts it, rather than ever waiting — a
// missing log line is harmless; a stalled order is not.
//
// One background thread drains all connections' queues in turn. Files live at
//   /mnt/S/CME/<Environment>/Logs/MSGW_<id>/<date>.log

#include "ILink3Config.hpp"
#include "ILink3Sbe.hpp"        // ToObjectType, ToJsonLine
#include "Wire.hpp"             // TryFrame
#include "ByteQueue.hpp"        // Tools::ByteQueue (lock-free single-producer/consumer ring)
#include "LowLatency.hpp"       // Tools::LowLatency (background thread, core pinning)
#include "Timestamp.hpp"
#include <magic_enum.hpp>       // enum -> name for the environment (same source glaze uses)

#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>

namespace ILink3
{

// Which way a logged message was travelling.
enum class Direction : uint8_t
{
	Send = 0,
	Recv = 1,
};

// The logger for one connection. The session thread calls Log() (the fast side); the drain
// thread calls Drain() (the slow side). A single lock-free queue carries entries from one to
// the other, so the two never block each other. Not copyable or movable — it holds a queue
// with atomics and is referred to by pointer.
class CmeLogger
{
	// One entry in the queue: two timestamps and a direction flag, followed by the raw message
	// bytes. The queue itself records each entry's length, so the message size is implicit.
	static constexpr size_t TimestampBytes = sizeof(int64_t);
	static constexpr size_t EntryPrefixBytes = TimestampBytes + TimestampBytes + 1;   // software, nic, direction

	Tools::ByteQueue _queue;
	std::filesystem::path _directory;
	int32_t _marketSegmentId;

	// Drain-thread-only file state.
	std::ofstream _stream;
	Tools::Timestamp _openDate{Tools::Timestamp::MinValue};

	// Written by the session thread, read by the drain thread when it reports; relaxed is fine.
	std::atomic<uint64_t> _droppedCount{0};

public:
	// Open a logger for one connection: create its log directory and its queue.
	CmeLogger(const std::filesystem::path& directory, int32_t marketSegmentId, int32_t queueCapacityBytes = Tools::Memory::HugePageLength)
		: _queue(queueCapacityBytes), _directory(directory), _marketSegmentId(marketSegmentId)
	{
		// Step 1: Make sure the destination directory exists before any writing starts.
		std::filesystem::create_directories(_directory);
	}

	CmeLogger(const CmeLogger&) = delete;
	CmeLogger& operator=(const CmeLogger&) = delete;

	int32_t MarketSegmentId() const { return _marketSegmentId; }
	uint64_t DroppedCount() const { return _droppedCount.load(std::memory_order_relaxed); }

	// Fast side (session thread): record one raw framed message. Never blocks — if the queue
	// is full the entry is dropped and counted.
	void Log(Direction direction, int64_t softwareTimestamp, int64_t nicTimestamp, std::span<const uint8_t> frame)
	{
		// Step 1: Reserve room for the fixed prefix plus the message bytes.
		const int32_t entryLength = static_cast<int32_t>(EntryPrefixBytes + frame.size());
		std::span<uint8_t> dst;
		if (!_queue.TryEnqueue(entryLength, dst))
		{
			_droppedCount.fetch_add(1, std::memory_order_relaxed);
			return;
		}

		// Step 2: Lay down the two timestamps, the direction, then the raw frame.
		std::memcpy(dst.data(), &softwareTimestamp, TimestampBytes);
		std::memcpy(dst.data() + TimestampBytes, &nicTimestamp, TimestampBytes);
		dst[TimestampBytes + TimestampBytes] = static_cast<uint8_t>(direction);
		std::memcpy(dst.data() + EntryPrefixBytes, frame.data(), frame.size());

		// Step 3: Publish the entry for the drain thread.
		_queue.Commit();
	}

	// Slow side (drain thread): write out every queued entry. Returns true if it wrote any,
	// so the manager knows this pass was not idle.
	bool Drain()
	{
		// Step 1: Pull and write entries until the queue is empty.
		bool wroteAny = false;
		std::span<const uint8_t> entry;
		while (_queue.TryPeek(entry))
		{
			WriteEntry(entry);
			_queue.Dequeue();
			wroteAny = true;
		}

		// Step 2: Flush once per pass so a burst is not fsynced line by line.
		if (wroteAny)
			_stream.flush();
		return wroteAny;
	}

private:
	// Decode one queued entry and append it as a JSON line to the right daily file.
	void WriteEntry(std::span<const uint8_t> entry)
	{
		// Step 1: Read back the prefix (two timestamps, direction) and locate the frame.
		int64_t softwareTimestamp = 0;
		int64_t nicTimestamp = 0;
		std::memcpy(&softwareTimestamp, entry.data(), TimestampBytes);
		std::memcpy(&nicTimestamp, entry.data() + TimestampBytes, TimestampBytes);
		const Direction direction = static_cast<Direction>(entry[TimestampBytes + TimestampBytes]);
		std::span<const uint8_t> frame = entry.subspan(EntryPrefixBytes);

		// Step 2: Open (or roll to) the file for this entry's date.
		EnsureFile(Tools::Timestamp(softwareTimestamp).Date());

		// Step 3: Build the metadata: what the message is, which way it went, and when.
		std::string line = "{\"ObjectType\":\"";
		line += ObjectTypeOf(frame);
		line += "\",\"Direction\":\"";
		line += (direction == Direction::Send) ? "User ==> CME" : "User <== CME";
		line += "\",\"SoftwareTimestamp\":\"";
		line += Tools::Timestamp(softwareTimestamp).ToString();
		line += "\",\"NicTimestamp\":";
		if (nicTimestamp > 0)
		{
			line += '"';
			line += Tools::Timestamp(nicTimestamp).ToString();
			line += '"';
		}
		else
		{
			line += "null";   // no hardware timestamp yet (kernel-socket bring-up phase)
		}

		// Step 4: Splice the decoded message fields in beside the metadata (or, if the message
		// cannot be rendered as text, its raw bytes as hex).
		AppendMessage(line, frame);

		// Step 5: One JSON object per line.
		line += '}';
		_stream << line << '\n';
	}

	// Frame the raw bytes and return the message's object-type name, or a stub if unframable.
	static std::string_view ObjectTypeOf(std::span<const uint8_t> frame)
	{
		FramedMessage framed;
		if (!TryFrame(frame, framed))
			return "Unframable";
		return ToObjectType(framed.TemplateId);
	}

	// Append the message body to `line`: its fields inline when they render as text, otherwise
	// a hex dump under a "raw" key. Either way `line` is left without its closing brace.
	static void AppendMessage(std::string& line, std::span<const uint8_t> frame)
	{
		// Step 1: Find the message body inside the frame.
		FramedMessage framed;
		if (!TryFrame(frame, framed))
		{
			AppendRawHex(line, frame);
			return;
		}

		// Step 2: Try to serialize the typed message.
		std::string body;
		try
		{
			body = ToJsonLine(framed.TemplateId, framed.Body);
		}
		catch (...)
		{
			AppendRawHex(line, frame);
			return;
		}

		// Step 3: A message carrying binary data (a logon signature) serializes with raw
		// control bytes, which are not valid inside text. If any slipped in, log the raw bytes
		// as hex instead so every line stays valid. (Properly escaped characters read as two
		// printable characters like backslash-n, so this only catches genuine binary.)
		for (char c : body)
		{
			if (static_cast<unsigned char>(c) < 0x20)
			{
				AppendRawHex(line, frame);
				return;
			}
		}

		// Step 4: Inline the body's fields next to the metadata. "{}" means no fields.
		if (body.size() <= 2)
			return;
		line += ',';
		line += body.substr(1, body.size() - 2);   // drop the body's own braces
	}

	// Append a "raw" hex dump of the whole frame (used when a message will not render as text).
	static void AppendRawHex(std::string& line, std::span<const uint8_t> frame)
	{
		static const char* digits = "0123456789abcdef";
		line += ",\"raw\":\"";
		for (uint8_t b : frame)
		{
			line += digits[b >> 4];
			line += digits[b & 0x0F];
		}
		line += '"';
	}

	// Open the daily file for `date`, rolling to a new file when the date changes.
	void EnsureFile(Tools::Timestamp date)
	{
		// Step 1: Nothing to do if the right file is already open.
		if (_stream.is_open() && date == _openDate)
			return;

		// Step 2: Close the previous day and open the new one in append mode.
		if (_stream.is_open())
			_stream.close();
		_openDate = date;
		_stream.open(_directory / (date.ToDateString() + ".log"), std::ios::out | std::ios::app);
	}
};

// Owns every connection's logger and the single background thread that drains them all. Create
// the loggers up front, then Start(); each session thread logs into its own logger, and this
// one thread writes them all out in the background.
class CmeLoggerManager
{
	std::unordered_map<std::string, std::unique_ptr<CmeLogger>> _loggers;   // keyed by directory
	std::mutex _loggersLock;              // guards the map against NewLogger() racing the drainer
	std::thread _thread;
	std::atomic<bool> _running{false};

public:
	CmeLoggerManager() = default;

	~CmeLoggerManager()
	{
		Stop();
	}

	CmeLoggerManager(const CmeLoggerManager&) = delete;
	CmeLoggerManager& operator=(const CmeLoggerManager&) = delete;

	// Standard log directory for one connection: <root>/CME/<Environment>/Logs/MSGW_<id>.
	// The environment name comes from the enum itself (via magic_enum, the same mechanism glaze
	// uses), so it always matches the enum with no separate table to keep in sync.
	static std::filesystem::path LogDirectory(const std::filesystem::path& root, ILink3::Environment environment, int32_t marketSegmentId)
	{
		return root / "CME"
			/ std::string(magic_enum::enum_name(environment))
			/ "Logs" / ("MSGW_" + std::to_string(marketSegmentId));
	}

	// Create and register a logger for one connection, keyed by its directory, and return a
	// reference the session uses. Throws if a logger for that directory already exists — two
	// loggers writing the same file would interleave each other's lines.
	CmeLogger& NewLogger(const std::filesystem::path& directory, int32_t marketSegmentId)
	{
		std::lock_guard<std::mutex> lock(_loggersLock);
		const std::string key = directory.string();
		if (_loggers.contains(key))
			throw std::invalid_argument("CmeLoggerManager: a logger already exists for " + key);
		auto [entry, inserted] = _loggers.emplace(key, std::make_unique<CmeLogger>(directory, marketSegmentId));
		return *entry->second;
	}

	// Start the background drain thread on the non-trading cores.
	void Start()
	{
		// Step 1: Ignore a second Start().
		if (_running.exchange(true))
			return;

		// Step 2: Spin up the drain loop on a background core.
		_thread = Tools::LowLatency::StartBackgroundThread("CmeLogger", [this]() { DrainLoop(); });
	}

	// Stop the drain thread, then write out anything still queued.
	void Stop()
	{
		// Step 1: Signal the loop to exit and wait for it.
		if (_running.exchange(false))
		{
			if (_thread.joinable())
				_thread.join();
		}

		// Step 2: Final drain so nothing queued at shutdown is lost.
		std::lock_guard<std::mutex> lock(_loggersLock);
		for (auto& entry : _loggers)
			entry.second->Drain();
	}

private:
	// The background thread: drain every logger, and if a whole pass found nothing, pause 1ms.
	void DrainLoop()
	{
		// Step 1: Keep going until Stop() clears the flag.
		while (_running.load(std::memory_order_acquire))
		{
			// Step 2: Snapshot the logger pointers under the lock, then drain without holding it.
			bool wroteAny = false;
			{
				std::lock_guard<std::mutex> lock(_loggersLock);
				for (auto& entry : _loggers)
					wroteAny |= entry.second->Drain();
			}

			// Step 3: Nothing to do this pass — sleep briefly before looking again.
			if (!wroteAny)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}
};

} // namespace ILink3
