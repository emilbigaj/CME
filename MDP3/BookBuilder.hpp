#pragma once

// Turns a channel's incremental market-data messages into the trading server's book and trade
// ticks, for the instruments the server subscribed. One builder per channel, driven by the
// segment's market-data thread; everything here is single-threaded.
//
// The exchange sends book updates as absolute states of a price level — this many resting at
// this price, or the level deleted — grouped into match events that may span several messages.
// Level changes are gathered per instrument as they arrive and published once, when the event
// ends (the end-of-event flag), so a strategy never acts on half an event. The published tick
// is the server's market-by-price update: absolute quantities per price, zero removing the
// level; the server's own book keys levels by price, so the exchange's positional level number
// is not needed. Trades publish immediately as trade ticks.
//
// Prices arrive as nine-decimal mantissas and leave as tick counts, through the same
// per-instrument scale the order path uses (one tick = a fixed mantissa step, set once at
// subscription). Instrument lookup on the hot path is a binary search over a small sorted
// array — a channel carries at most a few dozen subscribed instruments — with no hashing and
// no allocation anywhere past subscription.
//
// Correctness from the first minute comes from the snapshot feed. Every instrument starts
// stale — its incremental book changes are dropped — until a snapshot rebuilds its book whole
// (published through the same update path, with removals for levels the snapshot no longer
// shows) and hands over the instrument's update sequence; incrementals then apply only past
// that point. A gap in the channel's packet sequence marks everything stale again and the
// same recovery runs. A channel reset empties every book and does likewise. Bulk window-edge
// deletes expand against the current book into per-level removals.

#include "PacketWalker.hpp"
#include "QueueTracker.hpp"
#include "Mdp3Sbe.hpp"
#include "Tick.hpp"        // Data::MarketByPrice / Trade / Level / TickHeader
#include "Timestamp.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <span>
#include <vector>

namespace Mdp3
{

class BookBuilder
{
	// The most price levels one match event can touch per side of one instrument. The visible
	// book is ten levels; an event that shifts the window touches at most the old and new sets,
	// and a bulk window-edge delete can expand to the whole visible side.
	static constexpr int32_t MaxLevelsPerEvent = 40;

	// A snapshot rebuild can carry a full side plus removals for everything the book held.
	static constexpr int32_t MaxTickLevels = 80;

	// One subscribed instrument: its exchange identity, the server's id, and the price scale.
	struct Instrument
	{
		int32_t SecurityID = 0;
		int32_t InstrumentId = 0;
		int64_t GlobexTickMantissa = 0;   // price mantissa per one tick
		uint32_t LastRptSeq = 0;          // the instrument's own update sequence (resync anchor)
		bool Synced = false;              // false until a snapshot has rebuilt this book
	};

	// The level changes gathered for one instrument during the current match event. Quantities
	// are absolute; the same price updated twice within an event keeps only the latest state.
	struct Pending
	{
		Data::Level Bids[MaxLevelsPerEvent];
		Data::Level Asks[MaxLevelsPerEvent];
		int32_t BidsCount = 0;
		int32_t AsksCount = 0;
		uint64_t TransactTime = 0;
		bool Dirty = false;
	};

	std::vector<Instrument> _instruments;   // in subscription order — slots stay stable
	std::vector<Pending> _pending;          // parallel to _instruments
	std::vector<int32_t> _bySecurityId;     // slot numbers sorted by SecurityID, for the hot lookup
	std::vector<int32_t> _dirty;            // which instruments the current event touched

	// The published tick is assembled here — big enough for a snapshot rebuild on both sides.
	alignas(Data::MarketByPrice) uint8_t _tickBuffer[static_cast<size_t>(Data::MarketByPrice::SizeOf(MaxTickLevels, MaxTickLevels))]{};

	// Recovery state: the channel's packet sequence, and how many instruments await a snapshot.
	uint32_t _lastPacketSeqNum = 0;
	int32_t _staleCount = 0;

public:
	// Called with each finished book update (absolute quantities per price) and each trade.
	// The owner wires these to the server, which broadcasts to the subscribed strategies.
	std::function<void(Data::MarketByPrice&, std::span<uint8_t>)> OnMarketByPrice;
	std::function<void(const Data::Trade&)> OnTrade;

	// The queue positions of our own working orders, fed from the same walk (order events at
	// their prices) and settled after each book flush, when the level totals are current.
	QueueTracker Queue;

	// Wired by the owner: the authoritative book's current levels for one side (best first),
	// used to expand bulk deletes and to emit removals on snapshot rebuilds and resets.
	std::function<int32_t(int32_t instrumentId, bool isBid, Data::Level* levels, int32_t capacity)> ReadSide;

	// Counters for judgement: how the recovery machinery has been exercised.
	uint64_t PacketGaps = 0;        // channel packet-sequence gaps (each marks everything stale)
	uint64_t SnapshotResyncs = 0;   // books rebuilt from the snapshot feed
	uint64_t StaleDrops = 0;        // book changes dropped while awaiting a snapshot
	uint64_t BulkDeletes = 0;       // window-edge bulk deletes expanded and applied
	uint64_t StatusChanges = 0;     // trading-status messages seen (logged, not yet acted on)

	// Register an instrument (cold; called between packets by the owning thread). tickSize is
	// the conventional tick, displayFactor the exchange's raw-to-conventional scale — the same
	// pair the order path uses. Slots are stable: a new subscription never disturbs the pending
	// event state of the existing ones.
	void Subscribe(int32_t securityId, int32_t instrumentId, double tickSize, double displayFactor)
	{
		Instrument instrument;
		instrument.SecurityID = securityId;
		instrument.InstrumentId = instrumentId;
		instrument.GlobexTickMantissa = static_cast<int64_t>(std::llround(tickSize / displayFactor)) * 1'000'000'000LL;
		_instruments.push_back(instrument);
		++_staleCount;   // stale until its first snapshot
		_pending.push_back(Pending{});
		_bySecurityId.push_back(static_cast<int32_t>(_instruments.size()) - 1);
		std::sort(_bySecurityId.begin(), _bySecurityId.end(), [this](int32_t a, int32_t b)
		{
			return _instruments[static_cast<size_t>(a)].SecurityID < _instruments[static_cast<size_t>(b)].SecurityID;
		});
		_dirty.reserve(_instruments.size());
	}

	// Consume one datagram: gather book changes, publish trades, and flush the book at each
	// end of event. `nicTimestamp` is when the datagram hit the wire on our side.
	void OnPacket(std::span<const uint8_t> datagram, Tools::Timestamp nicTimestamp)
	{
		// Step 1: Frame the packet and police the channel's packet sequence: a gap means missed
		// packets, so every book is suspect until a snapshot rebuilds it. Late or repeated
		// packets are dropped.
		PacketWalker walker(datagram);
		if (!walker.Valid())
			return;
		const uint32_t sequenceNumber = walker.Header().MsgSeqNum;
		if (_lastPacketSeqNum != 0 && sequenceNumber <= _lastPacketSeqNum)
			return;
		if (_lastPacketSeqNum != 0 && sequenceNumber != _lastPacketSeqNum + 1)
		{
			++PacketGaps;
			StaleAll();
		}
		_lastPacketSeqNum = sequenceNumber;
		const Tools::Timestamp sendingTimestamp(static_cast<int64_t>(walker.Header().SendingTime));

		// Step 2: Walk the messages. Any message kind can carry the end-of-event flag.
		MessageView message;
		while (walker.TryNext(message))
		{
			switch (message.TemplateId)
			{
				case MDIncrementalRefreshBook::TemplateId:
				{
					const MDIncrementalRefreshBook& book = *message.As<MDIncrementalRefreshBook>();
					MDIncrementalRefreshBookGroups groups = MDIncrementalRefreshBookGroups::Of(book);
					Sbe::GroupReader<MDIncrementalRefreshBook_NoMDEntries> entries = groups.NoMDEntries();
					for (uint16_t i = 0; i < entries.Count(); ++i)
						GatherLevel(entries[i], book.TransactTime);
					if (Queue.HasOrders())
						FeedOrderEntries(groups, entries);
					if (book.MatchEventIndicator.EndOfEvent())
					{
						Flush(sendingTimestamp, nicTimestamp);
						Queue.OnEndOfEvent();
					}
					break;
				}
				case MDIncrementalRefreshOrderBook::TemplateId:
				{
					const MDIncrementalRefreshOrderBook& orderBook = *message.As<MDIncrementalRefreshOrderBook>();
					if (Queue.HasOrders())
						FeedStandaloneOrderEntries(orderBook);
					if (orderBook.MatchEventIndicator.EndOfEvent())
					{
						Flush(sendingTimestamp, nicTimestamp);
						Queue.OnEndOfEvent();
					}
					break;
				}
				case MDIncrementalRefreshTradeSummary::TemplateId:
				{
					const MDIncrementalRefreshTradeSummary& summary = *message.As<MDIncrementalRefreshTradeSummary>();
					for (const MDIncrementalRefreshTradeSummary_NoMDEntries& entry : MDIncrementalRefreshTradeSummaryGroups::Of(summary).NoMDEntries())
						PublishTrade(entry, summary.TransactTime, sendingTimestamp, nicTimestamp);
					if (summary.MatchEventIndicator.EndOfEvent())
					{
						Flush(sendingTimestamp, nicTimestamp);
						Queue.OnEndOfEvent();
					}
					break;
				}
				case ChannelReset::TemplateId:
				{
					// The channel restarted: empty every book (removals through the same update
					// path) and recover everything from snapshots.
					std::cout << "BookBuilder: channel reset — clearing books.\n";
					for (size_t slot = 0; slot < _instruments.size(); ++slot)
						EmitRemovalsOnly(static_cast<int32_t>(slot), sendingTimestamp, nicTimestamp);
					StaleAll();
					break;
				}
				case SecurityStatus::TemplateId:
				{
					const SecurityStatus& status = *message.As<SecurityStatus>();
					++StatusChanges;
					if (Find(status.SecurityID) >= 0)
						std::cout << "BookBuilder: trading status for " << status.SecurityID << ": "
						          << Mdp3::ToJsonLine(SecurityStatus::TemplateId, message.Body) << "\n";
					break;
				}
				default:
					break;   // statistics messages: later phases
			}
		}
	}

	// How many subscribed instruments still await a snapshot rebuild.
	int32_t StaleCount() const { return _staleCount; }

	// Consume one datagram from the snapshot feed: rebuild any still-stale instrument whose
	// full refresh appears. Cheap when nothing is stale.
	void OnSnapshotPacket(std::span<const uint8_t> datagram, Tools::Timestamp nicTimestamp)
	{
		if (_staleCount == 0)
			return;
		PacketWalker walker(datagram);
		if (!walker.Valid())
			return;
		const Tools::Timestamp sendingTimestamp(static_cast<int64_t>(walker.Header().SendingTime));
		MessageView message;
		while (walker.TryNext(message))
		{
			if (message.TemplateId == SnapshotFullRefresh::TemplateId)
				RebuildFromSnapshot(*message.As<SnapshotFullRefresh>(), sendingTimestamp, nicTimestamp);
		}
	}

private:
	// The subscription slot for an exchange id, or -1 if not subscribed. Binary search over the
	// small sorted index: no hashing, a handful of predictable compares.
	int32_t Find(int32_t securityId) const
	{
		int32_t low = 0;
		int32_t high = static_cast<int32_t>(_bySecurityId.size()) - 1;
		while (low <= high)
		{
			const int32_t middle = (low + high) / 2;
			const int32_t slot = _bySecurityId[static_cast<size_t>(middle)];
			const int32_t at = _instruments[static_cast<size_t>(slot)].SecurityID;
			if (at == securityId)
				return slot;
			if (at < securityId)
				low = middle + 1;
			else
				high = middle - 1;
		}
		return -1;
	}

	// Gather one book-level change into its instrument's pending event state.
	void GatherLevel(const MDIncrementalRefreshBook_NoMDEntries& entry, uint64_t transactTime)
	{
		// Step 1: Real bid/offer levels only (implied and reset entries come with later phases).
		const bool isBid = entry.MDEntryType == MDEntryTypeBook::Bid;
		if (!isBid && entry.MDEntryType != MDEntryTypeBook::Offer)
			return;
		const int32_t slot = Find(entry.SecurityID);
		if (slot < 0)
			return;
		Instrument& instrument = _instruments[static_cast<size_t>(slot)];
		Pending& pending = _pending[static_cast<size_t>(slot)];

		// Step 2: The recovery gate. A stale book takes nothing until its snapshot; a synced one
		// takes only changes past its resync anchor (drops the overlap a fresh snapshot covers).
		if (!instrument.Synced)
		{
			++StaleDrops;
			return;
		}
		if (entry.RptSeq <= instrument.LastRptSeq)
			return;
		instrument.LastRptSeq = entry.RptSeq;

		// Step 3: A bulk window-edge delete names one price and clears everything at or beyond
		// it toward the top (delete-thru) or the bottom (delete-from); expand it against the
		// pending state and the current book into per-level removals.
		const int32_t ticks = static_cast<int32_t>(entry.MDEntryPx.Mantissa / instrument.GlobexTickMantissa);
		if (entry.MDUpdateAction == MDUpdateAction::DeleteThru || entry.MDUpdateAction == MDUpdateAction::DeleteFrom)
		{
			++BulkDeletes;
			const bool towardTop = entry.MDUpdateAction == MDUpdateAction::DeleteThru;
			auto inRange = [&](int32_t levelTicks)
			{
				const bool better = isBid ? levelTicks >= ticks : levelTicks <= ticks;
				return towardTop ? better : !better || levelTicks == ticks;
			};
			Data::Level* pendingLevels = isBid ? pending.Bids : pending.Asks;
			int32_t& pendingCount = isBid ? pending.BidsCount : pending.AsksCount;
			for (int32_t i = 0; i < pendingCount; ++i)
				if (inRange(pendingLevels[i].Ticks))
					pendingLevels[i].Quantity = 0;
			if (ReadSide)
			{
				Data::Level current[MaxTickLevels];
				const int32_t currentCount = ReadSide(instrument.InstrumentId, isBid, current, MaxTickLevels);
				for (int32_t i = 0; i < currentCount; ++i)
					if (inRange(current[i].Ticks))
						FoldPending(pending, isBid, current[i].Ticks, 0);
			}
			MarkDirty(slot, pending, transactTime);
			return;
		}

		// Step 4: The level's new absolute state — zero when deleted — folded into the pending
		// side; the same price twice in one event keeps the latest.
		const int32_t quantity = entry.MDUpdateAction == MDUpdateAction::Delete ? 0 : entry.MDEntrySize;
		FoldPending(pending, isBid, ticks, quantity);
		MarkDirty(slot, pending, transactTime);
	}

	// Fold one absolute level state into the pending side (the latest state per price wins).
	void FoldPending(Pending& pending, bool isBid, int32_t ticks, int32_t quantity)
	{
		Data::Level* levels = isBid ? pending.Bids : pending.Asks;
		int32_t& count = isBid ? pending.BidsCount : pending.AsksCount;
		for (int32_t i = 0; i < count; ++i)
		{
			if (levels[i].Ticks == ticks)
			{
				levels[i].Quantity = quantity;
				return;
			}
		}
		if (count < MaxLevelsPerEvent)
			levels[count++] = Data::Level{ticks, quantity};
	}

	// Note the instrument as touched by the current match event.
	void MarkDirty(int32_t slot, Pending& pending, uint64_t transactTime)
	{
		pending.TransactTime = transactTime;
		if (!pending.Dirty)
		{
			pending.Dirty = true;
			_dirty.push_back(slot);
		}
	}

	// Mark every instrument stale: their books take nothing until snapshots rebuild them.
	void StaleAll()
	{
		for (Instrument& instrument : _instruments)
		{
			if (instrument.Synced)
			{
				instrument.Synced = false;
				++_staleCount;
			}
		}
	}

	// Rebuild one stale instrument's book from its full refresh: the snapshot's levels as
	// absolutes, removals for levels the book holds that the snapshot no longer shows, all
	// through the same update path. Hands over the instrument's update sequence and syncs it.
	void RebuildFromSnapshot(const SnapshotFullRefresh& snapshot, Tools::Timestamp sendingTimestamp, Tools::Timestamp nicTimestamp)
	{
		// Step 1: Only subscribed instruments still awaiting their snapshot.
		const int32_t slot = Find(snapshot.SecurityID);
		if (slot < 0)
			return;
		Instrument& instrument = _instruments[static_cast<size_t>(slot)];
		if (instrument.Synced)
			return;

		// Step 2: Gather the snapshot's book levels (the order path's tick scale) per side.
		Data::Level bidLevels[MaxTickLevels];
		Data::Level askLevels[MaxTickLevels];
		int32_t bidsCount = 0;
		int32_t asksCount = 0;
		for (const SnapshotFullRefresh_NoMDEntries& entry : SnapshotFullRefreshGroups::Of(snapshot).NoMDEntries())
		{
			const bool isBid = entry.MDEntryType == MDEntryType::Bid;
			if (!isBid && entry.MDEntryType != MDEntryType::Offer)
				continue;
			Data::Level* side = isBid ? bidLevels : askLevels;
			int32_t& count = isBid ? bidsCount : asksCount;
			if (count < MaxTickLevels / 2)
				side[count++] = Data::Level{static_cast<int32_t>(entry.MDEntryPx.Mantissa / instrument.GlobexTickMantissa), entry.MDEntrySize};
		}

		// Step 3: Removals: whatever the book holds that the snapshot no longer shows.
		if (ReadSide)
		{
			for (const bool isBid : {true, false})
			{
				Data::Level current[MaxTickLevels];
				const int32_t currentCount = ReadSide(instrument.InstrumentId, isBid, current, MaxTickLevels);
				Data::Level* side = isBid ? bidLevels : askLevels;
				int32_t& count = isBid ? bidsCount : asksCount;
				for (int32_t i = 0; i < currentCount; ++i)
				{
					bool inSnapshot = false;
					for (int32_t k = 0; k < count && !inSnapshot; ++k)
						inSnapshot = side[k].Ticks == current[i].Ticks;
					if (!inSnapshot && count < MaxTickLevels)
						side[count++] = Data::Level{current[i].Ticks, 0};
				}
			}
		}

		// Step 4: Lay the tick out: header, then bids, then asks directly after them.
		Data::MarketByPrice& tick = *reinterpret_cast<Data::MarketByPrice*>(_tickBuffer);
		tick.TickHeader = Data::TickHeader
		{
			.TickType = Data::TickType::MarketByPriceUpdate,
			.InstrumentId = instrument.InstrumentId,
			.ExchangeTimestamp = sendingTimestamp,
			.SendingTimestamp = sendingTimestamp,
			.NicTimestamp = nicTimestamp,
		};
		tick.BidsCount = bidsCount;
		tick.AsksCount = asksCount;
		std::memcpy(Data::MarketByPrice::GetBidsPtr(&tick), bidLevels, sizeof(Data::Level) * static_cast<size_t>(bidsCount));
		std::memcpy(Data::MarketByPrice::GetAsksPtr(&tick), askLevels, sizeof(Data::Level) * static_cast<size_t>(asksCount));

		// Step 5: Publish, drop any half-gathered stale event, and sync at the snapshot's anchor.
		if (OnMarketByPrice && tick.BidsCount + tick.AsksCount > 0)
		{
			std::span<uint8_t> span(_tickBuffer, static_cast<size_t>(tick.SizeOf()));
			OnMarketByPrice(tick, span);
		}
		Pending& pending = _pending[static_cast<size_t>(slot)];
		pending.BidsCount = 0;
		pending.AsksCount = 0;
		instrument.LastRptSeq = snapshot.RptSeq;
		instrument.Synced = true;
		--_staleCount;
		++SnapshotResyncs;
	}

	// Publish removals for everything an instrument's book currently holds (a channel reset).
	void EmitRemovalsOnly(int32_t slot, Tools::Timestamp sendingTimestamp, Tools::Timestamp nicTimestamp)
	{
		Instrument& instrument = _instruments[static_cast<size_t>(slot)];
		if (!ReadSide)
			return;
		Data::MarketByPrice& tick = *reinterpret_cast<Data::MarketByPrice*>(_tickBuffer);
		tick.TickHeader = Data::TickHeader
		{
			.TickType = Data::TickType::MarketByPriceUpdate,
			.InstrumentId = instrument.InstrumentId,
			.ExchangeTimestamp = sendingTimestamp,
			.SendingTimestamp = sendingTimestamp,
			.NicTimestamp = nicTimestamp,
		};
		Data::Level current[MaxTickLevels];
		Data::Level askRemovals[MaxTickLevels];
		tick.BidsCount = ReadSide(instrument.InstrumentId, true, current, MaxTickLevels);
		for (int32_t i = 0; i < tick.BidsCount; ++i)
			current[i].Quantity = 0;
		std::memcpy(Data::MarketByPrice::GetBidsPtr(&tick), current, sizeof(Data::Level) * static_cast<size_t>(tick.BidsCount));
		tick.AsksCount = ReadSide(instrument.InstrumentId, false, askRemovals, MaxTickLevels);
		for (int32_t i = 0; i < tick.AsksCount; ++i)
			askRemovals[i].Quantity = 0;
		std::memcpy(Data::MarketByPrice::GetAsksPtr(&tick), askRemovals, sizeof(Data::Level) * static_cast<size_t>(tick.AsksCount));
		if (OnMarketByPrice && tick.BidsCount + tick.AsksCount > 0)
		{
			std::span<uint8_t> span(_tickBuffer, static_cast<size_t>(tick.SizeOf()));
			OnMarketByPrice(tick, span);
		}
		instrument.LastRptSeq = 0;
	}

	// Publish every instrument the finished event touched, then clear the event state.
	void Flush(Tools::Timestamp sendingTimestamp, Tools::Timestamp nicTimestamp)
	{
		for (const int32_t slot : _dirty)
		{
			Instrument& instrument = _instruments[static_cast<size_t>(slot)];
			Pending& pending = _pending[static_cast<size_t>(slot)];

			// Step 1: Assemble the update tick: header, then the gathered levels.
			Data::MarketByPrice& tick = *reinterpret_cast<Data::MarketByPrice*>(_tickBuffer);
			tick.TickHeader = Data::TickHeader
			{
				.TickType = Data::TickType::MarketByPriceUpdate,
				.InstrumentId = instrument.InstrumentId,
				.ExchangeTimestamp = Tools::Timestamp(static_cast<int64_t>(pending.TransactTime)),
				.SendingTimestamp = sendingTimestamp,
				.NicTimestamp = nicTimestamp,
			};
			tick.BidsCount = pending.BidsCount;
			tick.AsksCount = pending.AsksCount;
			std::span<uint8_t> span(_tickBuffer, static_cast<size_t>(tick.SizeOf()));
			std::memcpy(Data::MarketByPrice::GetBidsPtr(&tick), pending.Bids, sizeof(Data::Level) * static_cast<size_t>(pending.BidsCount));
			std::memcpy(Data::MarketByPrice::GetAsksPtr(&tick), pending.Asks, sizeof(Data::Level) * static_cast<size_t>(pending.AsksCount));

			// Step 2: Hand it to the owner and reset the slot for the next event.
			if (OnMarketByPrice)
				OnMarketByPrice(tick, span);
			pending.BidsCount = 0;
			pending.AsksCount = 0;
			pending.Dirty = false;
		}
		_dirty.clear();
	}

	// Feed the order events piggybacked on a book message to the queue tracker; each references
	// the level entry it belongs to, which names the instrument, side, and price.
	void FeedOrderEntries(MDIncrementalRefreshBookGroups& groups, Sbe::GroupReader<MDIncrementalRefreshBook_NoMDEntries>& entries)
	{
		for (const MDIncrementalRefreshBook_NoOrderIDEntries& orderEntry : groups.NoOrderIDEntries())
		{
			// Step 1: Resolve the referenced level entry (one-based; zero means unreferenced).
			if (orderEntry.ReferenceID == 0 || orderEntry.ReferenceID > entries.Count())
				continue;
			const MDIncrementalRefreshBook_NoMDEntries& level = entries[static_cast<uint16_t>(orderEntry.ReferenceID - 1)];
			const bool isBid = level.MDEntryType == MDEntryTypeBook::Bid;
			if (!isBid && level.MDEntryType != MDEntryTypeBook::Offer)
				continue;
			const int32_t slot = Find(level.SecurityID);
			if (slot < 0)
				continue;

			// Step 2: Hand it over in the order path's tick scale.
			Instrument& instrument = _instruments[static_cast<size_t>(slot)];
			const int32_t ticks = static_cast<int32_t>(level.MDEntryPx.Mantissa / instrument.GlobexTickMantissa);
			Queue.OnOrderEvent(level.SecurityID, isBid, ticks, orderEntry.OrderID,
				orderEntry.MDOrderPriority, orderEntry.MDDisplayQty, orderEntry.OrderUpdateAction);
		}
	}

	// Feed the standalone order events (the full-depth stream: changes below the visible window
	// arrive only here) to the queue tracker.
	void FeedStandaloneOrderEntries(const MDIncrementalRefreshOrderBook& orderBook)
	{
		for (const MDIncrementalRefreshOrderBook_NoMDEntries& entry : MDIncrementalRefreshOrderBookGroups::Of(orderBook).NoMDEntries())
		{
			const bool isBid = entry.MDEntryType == MDEntryTypeBook::Bid;
			if (!isBid && entry.MDEntryType != MDEntryTypeBook::Offer)
				continue;
			if (entry.MDEntryPx.Mantissa == INT64_MAX)
				continue;   // price not present
			const int32_t slot = Find(entry.SecurityID);
			if (slot < 0)
				continue;
			Instrument& instrument = _instruments[static_cast<size_t>(slot)];
			const int32_t ticks = static_cast<int32_t>(entry.MDEntryPx.Mantissa / instrument.GlobexTickMantissa);

			// This message speaks in level actions; translate to the order actions (the bulk
			// window-shift actions carry no order detail and do not occur here).
			OrderUpdateAction action;
			switch (entry.MDUpdateAction)
			{
				case MDUpdateAction::New:    action = OrderUpdateAction::New; break;
				case MDUpdateAction::Change: action = OrderUpdateAction::Update; break;
				case MDUpdateAction::Delete: action = OrderUpdateAction::Delete; break;
				default: continue;
			}
			Queue.OnOrderEvent(entry.SecurityID, isBid, ticks, entry.OrderID,
				entry.MDOrderPriority, entry.MDDisplayQty, action);
		}
	}

	// Publish one trade as the server's trade tick.
	void PublishTrade(const MDIncrementalRefreshTradeSummary_NoMDEntries& entry, uint64_t transactTime,
	                  Tools::Timestamp sendingTimestamp, Tools::Timestamp nicTimestamp)
	{
		const int32_t slot = Find(entry.SecurityID);
		if (slot < 0)
			return;
		Instrument& instrument = _instruments[static_cast<size_t>(slot)];

		Data::Trade trade{};
		trade.TickHeader = Data::TickHeader
		{
			.TickType = Data::TickType::Trade,
			.InstrumentId = instrument.InstrumentId,
			.ExchangeTimestamp = Tools::Timestamp(static_cast<int64_t>(transactTime)),
			.SendingTimestamp = sendingTimestamp,
			.NicTimestamp = nicTimestamp,
		};
		trade.Level.Ticks = static_cast<int32_t>(entry.MDEntryPx.Mantissa / instrument.GlobexTickMantissa);
		trade.Level.Quantity = entry.MDEntrySize;
		trade.Direction = entry.AggressorSide == AggressorSide::Buy ? int8_t(1)
		                : entry.AggressorSide == AggressorSide::Sell ? int8_t(-1) : int8_t(0);
		if (OnTrade)
			OnTrade(trade);

		// A buy aggressor consumes resting offers; a sell consumes resting bids. Feed the queue
		// tracker the resting side (no queue movement when the aggressor is unknown).
		if (Queue.HasOrders() && entry.AggressorSide != AggressorSide::NoAggressor)
			Queue.OnTradeEntry(entry.SecurityID, entry.AggressorSide == AggressorSide::Sell,
				trade.Level.Ticks, entry.MDEntrySize);
	}
};

} // namespace Mdp3
