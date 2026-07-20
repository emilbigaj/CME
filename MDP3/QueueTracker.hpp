#pragma once

// Tracks the queue position of our own working orders — how many contracts rest ahead of each
// at its price — from the order-level events of the market-data feed. One tracker per channel,
// driven by the same market-data thread as the book builder; the answer is published as each
// order's quantity-ahead, the exchange-owned field the strategies read.
//
// The exchange matches first-in-first-out, which makes the position tractable with almost no
// state. Everything at the level when our order arrives is ahead of it; every later arrival
// joins behind (a modify that gains size or moves price re-queues at the back); so the set
// ahead of us is frozen the moment we join and only ever shrinks. Departures name themselves:
// a delete carries the dying order's quantity, and its priority tells us front or behind with
// one compare. No other order is ever stored.
//
// The order-entry acknowledgment usually arrives AFTER the feed has already shown our order,
// so arming happens in two phases: at send time the price is marked pending and a small log
// records what happens there; the acknowledgment then names our exchange order id, the log is
// searched for our own arrival — which fixes our priority and the exact amount ahead — and the
// entries after it are replayed. When the feed carries no order detail (some environments), the
// tracker falls back to an estimate: trades reduce the front exactly, cancellations reduce it
// in proportion.
//
// An order resting below the visible window cannot be seeded — the aggregated feed says
// nothing about its level — so it publishes "unknown" (never zero, which would falsely claim
// the front). Meanwhile the net quantity joining behind it is counted exactly, and the moment
// the market approaches and the level enters the window, its published total fixes the
// position: ahead = total − ours − behind. From then on it is exact.

#include "Mdp3Sbe.hpp"
#include "Timestamp.hpp"

#include <array>
#include <cstdint>
#include <functional>

namespace Mdp3
{

class QueueTracker
{
public:
	// Published while an order's position cannot be known (resting below the visible window).
	static constexpr int32_t UnknownQuantityAhead = -1;

	// The order lifecycle commands the execution thread sends across (single producer, single
	// consumer; the market-data thread applies them between packets).
	enum class CommandKind : uint8_t
	{
		PreArm = 1,       // order (or replace) sent: start logging at its price
		Arm = 2,          // acknowledged: reconcile the log with our exchange order id
		OwnQuantity = 3,  // partial fill: our remaining quantity changed
		Disarm = 4,       // done (filled, cancelled, rejected): free the slot
	};

	struct Command
	{
		CommandKind Kind = CommandKind::PreArm;
		int8_t IsBid = 0;
		int32_t SecurityID = 0;
		int32_t InstrumentId = 0;
		int32_t Ticks = 0;               // our order's price
		int32_t Quantity = 0;            // our displayed quantity
		uint64_t ClientOrderId = 0;
		uint64_t ExchangeOrderID = 0;    // Arm only
	};

	// Wired by the owner: the authoritative book's level total (this thread writes that book,
	// so reading it here is consistent); whether a price is inside the visible window; and the
	// publisher for a changed quantity-ahead.
	std::function<int32_t(int32_t instrumentId, bool isBid, int32_t ticks)> ReadLevelQuantity;
	std::function<bool(int32_t instrumentId, bool isBid, int32_t ticks)> IsPriceVisible;
	std::function<void(uint64_t clientOrderId, int32_t quantityAhead)> OnQuantityAhead;

	// Counters for judgement, not control: how often the inexact paths ran.
	uint64_t EstimatorSeeds = 0;      // armed without finding our own arrival on the feed
	uint64_t UpdateDrift = 0;         // ahead-order size changes we could not attribute exactly
	uint64_t LogOverflows = 0;        // pending log filled before the acknowledgment arrived

private:
	enum class SlotState : uint8_t { Free, Pending, Armed };

	// What happened at a pending price while we waited for the acknowledgment.
	struct LogEntry
	{
		uint64_t OrderID = 0;         // zero for trades and anonymous events
		uint64_t Priority = 0;
		int32_t QuantityDelta = 0;    // signed change to the level's total
		OrderUpdateAction Action = OrderUpdateAction::New;
	};

	// One working order of ours. ~200 bytes, touched only when events hit its price.
	struct OwnOrder
	{
		static constexpr int32_t LogCapacity = 16;

		uint64_t ClientOrderId = 0;
		uint64_t ExchangeOrderID = 0;
		uint64_t Priority = 0;            // captured from our own arrival on the feed
		int32_t SecurityID = 0;
		int32_t InstrumentId = 0;
		int32_t Ticks = 0;
		int32_t OwnQuantity = 0;
		int32_t QueueAhead = 0;
		int32_t NetBehind = 0;            // exact net joins behind us (for the window-entry fix)
		int32_t LastLevelQuantity = 0;    // the level's total at the last event end
		int32_t EventTraded = 0;          // traded at our price within the current event
		int32_t PublishedAhead = INT32_MIN;   // "never published": the first publish always lands
		SlotState State = SlotState::Free;
		int8_t IsBid = 0;
		bool Seeded = false;              // false: below the window at arm — publish unknown
		bool Exact = false;               // true: our priority is known — order-event arithmetic
		bool Touched = false;             // an event hit this order's price this match event
		LogEntry Log[LogCapacity];
		int32_t LogCount = 0;
		int32_t PreArmLevelQuantity = 0;  // the level's total when the send happened
		bool LogOverflow = false;
	};

	static constexpr size_t Capacity = 64;
	std::array<OwnOrder, Capacity> _orders{};
	int32_t _activeCount = 0;

public:
	// True when any order is pending or armed — the one-branch skip for the hot path.
	bool HasOrders() const { return _activeCount != 0; }

	// Apply one lifecycle command from the execution thread (called between packets).
	void Apply(const Command& command)
	{
		switch (command.Kind)
		{
			case CommandKind::PreArm:      PreArm(command); break;
			case CommandKind::Arm:         Arm(command); break;
			case CommandKind::OwnQuantity: OwnQuantity(command); break;
			case CommandKind::Disarm:      Disarm(command); break;
		}
	}

	// One order-level event from the feed (piggybacked on a book message or standalone).
	void OnOrderEvent(int32_t securityId, bool isBid, int32_t ticks, uint64_t orderId,
	                  uint64_t priority, int32_t quantity, OrderUpdateAction action)
	{
		for (OwnOrder& order : _orders)
		{
			if (order.State == SlotState::Free || order.SecurityID != securityId
			 || order.Ticks != ticks || (order.IsBid != 0) != isBid)
				continue;
			order.Touched = true;

			// Step 1: While pending, everything at the price goes to the log for the reconcile.
			if (order.State == SlotState::Pending)
			{
				AppendLog(order, orderId, priority, SignedDelta(quantity, action), action);
				continue;
			}

			// Step 2: Our own order's events are bookkeeping, not queue movement. Seeing our
			// arrival (some environments only name us here) upgrades the slot to exact.
			if (orderId != 0 && orderId == order.ExchangeOrderID)
			{
				if (!order.Exact && priority != 0)
				{
					order.Priority = priority;
					order.Exact = true;
				}
				continue;
			}

			// Step 3: Armed and exact: one compare decides front or behind.
			if (order.Exact)
			{
				const bool ahead = priority < order.Priority;
				switch (action)
				{
					case OrderUpdateAction::New:
						if (ahead)
							order.QueueAhead += quantity;   // defensive: arrivals join behind
						else
							order.NetBehind += quantity;
						break;
					case OrderUpdateAction::Delete:
						if (ahead)
							order.QueueAhead -= quantity;   // the wire carries the dying quantity
						else
							order.NetBehind -= quantity;
						break;
					case OrderUpdateAction::Update:
						++UpdateDrift;   // new size only on the wire; the level clamp repairs it
						break;
				}
				MarkChanged(order);
			}
			// Estimator slots take their movement from trades and level totals at event end.
		}
	}

	// One trade entry at a price (the resting side is the aggressor's opposite).
	void OnTradeEntry(int32_t securityId, bool restingIsBid, int32_t ticks, int32_t quantity)
	{
		for (OwnOrder& order : _orders)
		{
			if (order.State == SlotState::Free || order.SecurityID != securityId
			 || order.Ticks != ticks || (order.IsBid != 0) != restingIsBid)
				continue;
			order.Touched = true;
			if (order.State == SlotState::Pending)
				AppendLog(order, 0, 0, -quantity, OrderUpdateAction::Delete);
			else
				order.EventTraded += quantity;
		}
	}

	// The match event ended: settle every touched order against its level and publish changes.
	void OnEndOfEvent()
	{
		for (OwnOrder& order : _orders)
		{
			if (order.State != SlotState::Armed || !order.Touched)
				continue;
			order.Touched = false;
			const int32_t level = ReadLevelQuantity(order.InstrumentId, order.IsBid != 0, order.Ticks);

			// Step 1: Below the window and unseeded: the moment the level becomes visible, its
			// total fixes the position exactly: ahead = total − ours − joined-behind.
			if (!order.Seeded)
			{
				if (level > 0 && IsPriceVisible(order.InstrumentId, order.IsBid != 0, order.Ticks))
				{
					order.QueueAhead = Clamp(level - order.OwnQuantity - order.NetBehind, level);
					order.Seeded = true;
				}
			}
			else if (!order.Exact)
			{
				// Step 2: Estimator: trades consume the front exactly; the remaining decrease is
				// cancellation, taken from the front in proportion.
				int32_t decrease = order.LastLevelQuantity - level;
				const int32_t traded = order.EventTraded < decrease ? order.EventTraded : decrease;
				order.QueueAhead -= traded;
				const int32_t cancelled = decrease - traded;
				if (cancelled > 0 && order.LastLevelQuantity > 0)
					order.QueueAhead -= static_cast<int32_t>(static_cast<int64_t>(cancelled) * order.QueueAhead / order.LastLevelQuantity);
			}

			// Step 3: The level total bounds the answer from both sides, whatever the mode.
			if (order.Seeded)
				order.QueueAhead = Clamp(order.QueueAhead, level);
			order.LastLevelQuantity = level;
			order.EventTraded = 0;
			Publish(order);
		}
	}

private:
	// Start logging at the order's price the moment the send happens.
	void PreArm(const Command& command)
	{
		// Step 1: A replace re-arms in place; otherwise claim a free slot.
		OwnOrder* order = Find(command.ClientOrderId);
		if (order == nullptr)
		{
			order = FindFree();
			if (order == nullptr)
				return;
			++_activeCount;
		}

		// Step 2: Fresh state for this (possibly new) price, and the level total at send time —
		// the anchor the reconcile builds on.
		*order = OwnOrder{};
		order->ClientOrderId = command.ClientOrderId;
		order->SecurityID = command.SecurityID;
		order->InstrumentId = command.InstrumentId;
		order->Ticks = command.Ticks;
		order->OwnQuantity = command.Quantity;
		order->IsBid = command.IsBid;
		order->State = SlotState::Pending;
		order->PreArmLevelQuantity = ReadLevelQuantity(command.InstrumentId, command.IsBid != 0, command.Ticks);
	}

	// The acknowledgment named our exchange order id: reconcile the log and publish the seed.
	void Arm(const Command& command)
	{
		OwnOrder* orderPointer = Find(command.ClientOrderId);
		if (orderPointer == nullptr)
			return;
		OwnOrder& order = *orderPointer;
		order.ExchangeOrderID = command.ExchangeOrderID;
		order.State = SlotState::Armed;

		// Step 1: Find our own arrival in the log. Before it, every change moved the level we
		// eventually joined behind; after it, only events ahead of our priority move us.
		int32_t ourArrival = -1;
		for (int32_t i = 0; i < order.LogCount; ++i)
		{
			if (order.Log[i].OrderID == command.ExchangeOrderID)
			{
				ourArrival = i;
				break;
			}
		}

		if (ourArrival >= 0 && !order.LogOverflow)
		{
			// Step 2: Exact: anchor at the level as it stood when we joined, then replay.
			order.Priority = order.Log[ourArrival].Priority;
			order.Exact = true;
			order.Seeded = true;
			int32_t ahead = order.PreArmLevelQuantity;
			for (int32_t i = 0; i < ourArrival; ++i)
				ahead += order.Log[i].QuantityDelta;
			for (int32_t i = ourArrival + 1; i < order.LogCount; ++i)
			{
				const LogEntry& entry = order.Log[i];
				const bool anonymousTrade = entry.OrderID == 0 && entry.Priority == 0;
				if (anonymousTrade || entry.Priority < order.Priority)
					ahead += entry.QuantityDelta < 0 ? entry.QuantityDelta : 0;
				else if (entry.QuantityDelta > 0)
					order.NetBehind += entry.QuantityDelta;
			}
			order.QueueAhead = ahead < 0 ? 0 : ahead;
		}
		else
		{
			// Step 3: Estimator: no order detail named us. Inside the window, everything at the
			// level less ourselves; below it, unknown until the level becomes visible.
			++EstimatorSeeds;
			if (order.LogOverflow)
				++LogOverflows;
			const int32_t level = ReadLevelQuantity(order.InstrumentId, order.IsBid != 0, order.Ticks);
			const bool visible = IsPriceVisible(order.InstrumentId, order.IsBid != 0, order.Ticks);
			if (level > 0 && visible)
			{
				order.QueueAhead = Clamp(level - order.OwnQuantity, level);
				order.Seeded = true;
			}
		}
		order.LogCount = 0;
		order.LastLevelQuantity = ReadLevelQuantity(order.InstrumentId, order.IsBid != 0, order.Ticks);
		Publish(order);
	}

	// A partial fill changed our remaining size (position among the others is unaffected).
	void OwnQuantity(const Command& command)
	{
		OwnOrder* order = Find(command.ClientOrderId);
		if (order != nullptr)
			order->OwnQuantity = command.Quantity;
	}

	// The order is done: free its slot.
	void Disarm(const Command& command)
	{
		OwnOrder* order = Find(command.ClientOrderId);
		if (order != nullptr)
		{
			order->State = SlotState::Free;
			order->ClientOrderId = 0;
			--_activeCount;
		}
	}

	// ---- helpers ----

	OwnOrder* Find(uint64_t clientOrderId)
	{
		for (OwnOrder& order : _orders)
			if (order.State != SlotState::Free && order.ClientOrderId == clientOrderId)
				return &order;
		return nullptr;
	}

	OwnOrder* FindFree()
	{
		for (OwnOrder& order : _orders)
			if (order.State == SlotState::Free)
				return &order;
		return nullptr;
	}

	// The signed change an order event applies to its level's total. An update's old size is
	// not on the wire, so its delta is unknown — logged as zero and counted when it matters.
	static int32_t SignedDelta(int32_t quantity, OrderUpdateAction action)
	{
		switch (action)
		{
			case OrderUpdateAction::New:    return quantity;
			case OrderUpdateAction::Delete: return -quantity;
			default:                        return 0;
		}
	}

	void AppendLog(OwnOrder& order, uint64_t orderId, uint64_t priority, int32_t quantityDelta, OrderUpdateAction action)
	{
		if (order.LogCount >= OwnOrder::LogCapacity)
		{
			order.LogOverflow = true;
			return;
		}
		order.Log[order.LogCount++] = LogEntry{orderId, priority, quantityDelta, action};
	}

	// Keep the answer inside what the level itself allows: never negative, never more than
	// everything at the level that is not us.
	static int32_t Clamp(int32_t queueAhead, int32_t levelQuantity)
	{
		if (queueAhead < 0)
			return 0;
		const int32_t most = levelQuantity > 0 ? levelQuantity : queueAhead;
		return queueAhead < most ? queueAhead : most;
	}

	void MarkChanged(OwnOrder& order)
	{
		order.Touched = true;
	}

	// Publish only when the number the strategy sees actually moved.
	void Publish(OwnOrder& order)
	{
		const int32_t value = order.Seeded ? order.QueueAhead : UnknownQuantityAhead;
		if (value == order.PublishedAhead)
			return;
		order.PublishedAhead = value;
		if (OnQuantityAhead)
			OnQuantityAhead(order.ClientOrderId, value);
	}
};

} // namespace Mdp3
