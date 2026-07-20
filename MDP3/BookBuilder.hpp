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
#include <span>
#include <vector>

namespace Mdp3
{

class BookBuilder
{
	// The most price levels one match event can touch per side of one instrument. The visible
	// book is ten levels; an event that shifts the window touches at most the old and new sets.
	static constexpr int32_t MaxLevelsPerEvent = 24;

	// One subscribed instrument: its exchange identity, the server's id, and the price scale.
	struct Instrument
	{
		int32_t SecurityID = 0;
		int32_t InstrumentId = 0;
		int64_t GlobexTickMantissa = 0;   // price mantissa per one tick
		uint32_t LastRptSeq = 0;          // the instrument's own update sequence (gap detection)
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

	// The published tick is assembled here — big enough for a full event on both sides.
	alignas(Data::MarketByPrice) uint8_t _tickBuffer[static_cast<size_t>(Data::MarketByPrice::SizeOf(MaxLevelsPerEvent, MaxLevelsPerEvent))]{};

public:
	// Called with each finished book update (absolute quantities per price) and each trade.
	// The owner wires these to the server, which broadcasts to the subscribed strategies.
	std::function<void(Data::MarketByPrice&, std::span<uint8_t>)> OnMarketByPrice;
	std::function<void(const Data::Trade&)> OnTrade;

	// The queue positions of our own working orders, fed from the same walk (order events at
	// their prices) and settled after each book flush, when the level totals are current.
	QueueTracker Queue;

	// Counters for the recovery layer (read off-line; nothing reacts to them yet).
	uint64_t RptSeqGaps = 0;        // per-instrument update-sequence gaps seen
	uint64_t UnhandledActions = 0;  // window-shift deletes and book resets, not yet modelled

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
		// Step 1: Frame the packet.
		PacketWalker walker(datagram);
		if (!walker.Valid())
			return;
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
				default:
					break;   // statistics, order-book, and status messages: later phases
			}
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

		// Step 2: Track the instrument's own update sequence for the recovery layer.
		if (instrument.LastRptSeq != 0 && entry.RptSeq != instrument.LastRptSeq + 1)
			++RptSeqGaps;
		instrument.LastRptSeq = entry.RptSeq;

		// Step 3: The level's new absolute state: its price in ticks, and its full quantity —
		// zero when the level is deleted. Window-shift bulk deletes are counted, not applied.
		if (entry.MDUpdateAction == MDUpdateAction::DeleteThru || entry.MDUpdateAction == MDUpdateAction::DeleteFrom)
		{
			++UnhandledActions;
			return;
		}
		const int32_t ticks = static_cast<int32_t>(entry.MDEntryPx.Mantissa / instrument.GlobexTickMantissa);
		const int32_t quantity = entry.MDUpdateAction == MDUpdateAction::Delete ? 0 : entry.MDEntrySize;

		// Step 4: Fold into the pending side: same price twice in one event keeps the latest.
		Data::Level* levels = isBid ? pending.Bids : pending.Asks;
		int32_t& count = isBid ? pending.BidsCount : pending.AsksCount;
		for (int32_t i = 0; i < count; ++i)
		{
			if (levels[i].Ticks == ticks)
			{
				levels[i].Quantity = quantity;
				goto gathered;
			}
		}
		if (count < MaxLevelsPerEvent)
			levels[count++] = Data::Level{ticks, quantity};
	gathered:
		pending.TransactTime = transactTime;
		if (!pending.Dirty)
		{
			pending.Dirty = true;
			_dirty.push_back(slot);
		}
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
