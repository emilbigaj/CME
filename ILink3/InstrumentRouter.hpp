#pragma once

// Bridges the trading server and CME for one instrument: it turns the server's order intents
// (create / amend / cancel, expressed in ticks and lots) into iLink3 order messages, and turns
// CME's execution reports back into the server's order events (accepted, rejected, filled). One
// router exists per allocated instrument.
//
// The single thread that owns the segment's gateway drives this router for both directions —
// it drains order intents and sends, and it reads execution reports and reconciles — so no
// locking is needed anywhere here.
//
// The order book is a dense slot array, not a map. A client order id is a packed value whose
// low bits are the order's global index — the same index the server uses for its own order
// arrays — so a record is found by masking the id: no hashing, no allocation, one predictable
// cache line per order. The slot remembers the full id it belongs to; the generation bits in
// the id make a stale report against a reused slot detectably mismatch. The id also rides to
// the exchange and back as the order's text ClOrdID, formatted and parsed on the stack.
//
// Prices cross a scale change at this boundary. The server works in ticks; the wire wants the
// exchange (Globex) price, which is a whole number of raw increments. One server tick equals a
// fixed Globex-price step, precomputed once as a nine-decimal mantissa, so turning a tick count
// into a wire price is a single multiply.

#include "ILink3Sbe.hpp"
#include "MarketSegmentGateway.hpp"
#include "Wire.hpp"
#include "Order.hpp"             // Execution::OrderTarget / OrderState / OrderRejected / Fill
#include "OrderIdAllocator.hpp"  // the client order id <-> global index packing
#include "String.hpp"
#include "Timestamp.hpp"

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace ILink3
{

class InstrumentRouter
{
	// ---- fixed identity + routing (set once) ----
	int32_t _instrumentId;                 // the server's compact id for this instrument
	int32_t _securityId;                   // CME's id, named on the wire
	MarketSegmentGateway* _gateway;        // the session this instrument's orders go out on
	int64_t _globexTickMantissa;           // wire price step (PRICE9 mantissa) per one server tick
	std::string _senderId;                 // operator id stamped on each order (built once)
	std::string _location;                 // desk location stamped on each order (built once)

	// ---- live order book (owned by the single driving thread) ----
	uint64_t _nextOrderRequestId = 1;      // a fresh request id per outbound order message

	// One slot per order index. ClientOrderId is the full packed id the slot currently belongs
	// to (zero = empty); its generation bits make a report against a reused slot mismatch.
	struct Record
	{
		uint64_t ClientOrderId = 0;
		uint64_t ExchangeOrderId = 0;          // CME's order id, known once the order is accepted
		int32_t Seq = 0;                       // the server revision this order is on
		Execution::OrderProfile OrderProfile;  // the price/size the server asked for
	};
	std::vector<Record> _orders;   // indexed by the global order index packed into the id

public:
	// Called with the server's events for this instrument once each execution report is decoded.
	// The owner wires these to the server (or, in a test, to a printer).
	std::function<void(const Execution::OrderState&)> OnOrderState;
	std::function<void(const Execution::OrderRejected&, const std::string&)> OnOrderRejected;
	std::function<void(const Execution::Fill&)> OnFill;

	// The order-lifecycle moments the queue tracking cares about: an order (or new price) going
	// to the wire, the exchange naming it, our remaining size changing, and it being done.
	std::function<void(uint64_t clientOrderId, int32_t ticks, int32_t quantity, bool isBid)> OnOrderSent;
	std::function<void(uint64_t clientOrderId, uint64_t exchangeOrderId)> OnOrderLive;
	std::function<void(uint64_t clientOrderId, int32_t remainingQuantity)> OnOrderQuantity;
	std::function<void(uint64_t clientOrderId)> OnOrderDone;

	InstrumentRouter(int32_t instrumentId, int32_t securityId, MarketSegmentGateway* gateway,
	                 double tickSize, double displayFactor, const std::string& senderId,
	                 const std::string& location, int32_t ordersCapacity)
		: _instrumentId(instrumentId),
		  _securityId(securityId),
		  _gateway(gateway),
		  // One server tick = tickSize display units = tickSize/displayFactor raw Globex units;
		  // times a billion gives the nine-decimal mantissa the wire carries.
		  _globexTickMantissa(static_cast<int64_t>(std::llround(tickSize / displayFactor)) * 1'000'000'000LL),
		  _senderId(senderId),
		  _location(location),
		  _orders(static_cast<size_t>(ordersCapacity))
	{
	}

	int32_t InstrumentId() const { return _instrumentId; }
	int32_t SecurityID() const { return _securityId; }
	const MarketSegmentGateway* Gateway() const { return _gateway; }

	// After a reconnect has recovered everything the exchange published, any slot still
	// waiting for its exchange id belongs to an order whose send died with the old connection
	// — the exchange never saw it (an acknowledged one would have been replayed by now). Fail
	// each back to the strategy so it can decide again. Returns how many were failed.
	int32_t SweepUnacknowledged()
	{
		int32_t swept = 0;
		for (Record& record : _orders)
			if (record.ClientOrderId != 0 && record.ExchangeOrderId == 0)
			{
				FailUnsent(record);
				++swept;
			}
		return swept;
	}

	// Route one order intent from the server to CME.
	void OnOrderTarget(const Execution::OrderTarget& target)
	{
		switch (target.OrderTargetAction)
		{
			case Execution::OrderTargetAction::Create: SendNew(target); break;
			case Execution::OrderTargetAction::Amend:  SendReplace(target); break;
			case Execution::OrderTargetAction::Cancel: SendCancel(target); break;
		}
	}

	// Pull the client order id out of any execution report, so the owner can route the message
	// to the right instrument's router (the instrument id is packed inside the client order id).
	// Returns false for message kinds that carry no per-order id.
	static bool TryGetClientOrderId(const FramedMessage& message, uint64_t& clientOrderId)
	{
		switch (message.TemplateId)
		{
			case ExecutionReportNew::TemplateId:           return TryParseClientOrderId(message.As<ExecutionReportNew>()->ClOrdID, clientOrderId);
			case ExecutionReportReject::TemplateId:        return TryParseClientOrderId(message.As<ExecutionReportReject>()->ClOrdID, clientOrderId);
			case ExecutionReportCancel::TemplateId:        return TryParseClientOrderId(message.As<ExecutionReportCancel>()->ClOrdID, clientOrderId);
			case ExecutionReportModify::TemplateId:        return TryParseClientOrderId(message.As<ExecutionReportModify>()->ClOrdID, clientOrderId);
			case ExecutionReportTradeOutright::TemplateId: return TryParseClientOrderId(message.As<ExecutionReportTradeOutright>()->ClOrdID, clientOrderId);
			case OrderCancelReject::TemplateId:            return TryParseClientOrderId(message.As<OrderCancelReject>()->ClOrdID, clientOrderId);
			case OrderCancelReplaceReject::TemplateId:     return TryParseClientOrderId(message.As<OrderCancelReplaceReject>()->ClOrdID, clientOrderId);
			default: return false;
		}
	}

	// Reconcile one decoded execution report and publish the resulting server event.
	void OnExecutionReport(const FramedMessage& message)
	{
		switch (message.TemplateId)
		{
			case ExecutionReportNew::TemplateId:           HandleNew(*message.As<ExecutionReportNew>()); break;
			case ExecutionReportReject::TemplateId:        HandleReject(*message.As<ExecutionReportReject>()); break;
			case ExecutionReportCancel::TemplateId:        HandleCancel(*message.As<ExecutionReportCancel>()); break;
			case ExecutionReportModify::TemplateId:        HandleModify(*message.As<ExecutionReportModify>()); break;
			case ExecutionReportTradeOutright::TemplateId: HandleFill(*message.As<ExecutionReportTradeOutright>()); break;
			case OrderCancelReject::TemplateId:            HandleModifyReject(message.As<OrderCancelReject>()->ClOrdID, message.As<OrderCancelReject>()->Text, Execution::OrderTargetAction::Cancel); break;
			case OrderCancelReplaceReject::TemplateId:     HandleModifyReject(message.As<OrderCancelReplaceReject>()->ClOrdID, message.As<OrderCancelReplaceReject>()->Text, Execution::OrderTargetAction::Amend); break;
			default: break;
		}
	}

private:
	// ---- the slot array ----

	// The slot an id maps to, or null if the id's index is out of range. The caller checks
	// whether the slot currently belongs to that id.
	Record* Slot(uint64_t clientOrderId)
	{
		const size_t index = static_cast<size_t>(Execution::OrderIdAllocator::GetGlobalIndex(clientOrderId));
		return index < _orders.size() ? &_orders[index] : nullptr;
	}

	// The slot currently owned by this exact id (generation included), or null.
	Record* Find(uint64_t clientOrderId)
	{
		Record* record = Slot(clientOrderId);
		return record != nullptr && record->ClientOrderId == clientOrderId ? record : nullptr;
	}

	// ---- outbound ----

	// Send a new order and claim its slot.
	void SendNew(const Execution::OrderTarget& target)
	{
		const uint64_t clientOrderId = target.OrderHeader.ClientOrderId;
		Record* record = Slot(clientOrderId);
		if (record == nullptr)
			return;

		const SideReq side = target.OrderProfile.Sign() >= 0 ? SideReq::Buy : SideReq::Sell;
		const uint32_t quantity = static_cast<uint32_t>(std::abs(target.OrderProfile.Quantity));
		const int64_t priceMantissa = static_cast<int64_t>(target.OrderProfile.Ticks) * _globexTickMantissa;

		NewOrderSingle order = NewLimitOrder(_securityId, side, quantity, priceMantissa,
			std::string(), _senderId, 0, _nextOrderRequestId++, _location);
		FormatClientOrderId(clientOrderId, order.ClOrdID);
		*record = Record{clientOrderId, 0, target.OrderHeader.Seq, target.OrderProfile};
		if (OnOrderSent)
			OnOrderSent(clientOrderId, target.OrderProfile.Ticks, static_cast<int32_t>(quantity), side == SideReq::Buy);
		if (!_gateway->SendNewOrderSingle(order))
			FailUnsent(*record);   // the session is down; the exchange never saw this order
	}

	// Cancel a working order, referencing it by the exchange id we kept from its acceptance.
	void SendCancel(const Execution::OrderTarget& target)
	{
		Record* record = Find(target.OrderHeader.ClientOrderId);
		if (record == nullptr || record->ExchangeOrderId == 0)
			return;   // nothing working to cancel

		const SideReq side = record->OrderProfile.Sign() >= 0 ? SideReq::Buy : SideReq::Sell;
		OrderCancelRequest cancel = NewCancel(_securityId, side, record->ExchangeOrderId,
			std::string(), _senderId, _nextOrderRequestId++, _location);
		FormatClientOrderId(target.OrderHeader.ClientOrderId, cancel.ClOrdID);
		record->Seq = target.OrderHeader.Seq;   // the cancel is this revision
		if (!_gateway->SendOrderCancel(cancel))
			PublishModifyReject(target.OrderHeader.ClientOrderId, record,
				Execution::OrderTargetAction::Cancel, "session down: cancel not sent");
	}

	// Move a working order to a new price/size, referencing it by the exchange id. The slot's
	// profile is only updated when the exchange confirms the modify, so a rejected replace
	// leaves the record true to the order still working at its old price.
	void SendReplace(const Execution::OrderTarget& target)
	{
		Record* record = Find(target.OrderHeader.ClientOrderId);
		if (record == nullptr || record->ExchangeOrderId == 0)
			return;   // nothing working to replace

		const SideReq side = record->OrderProfile.Sign() >= 0 ? SideReq::Buy : SideReq::Sell;
		const uint32_t quantity = static_cast<uint32_t>(std::abs(target.OrderProfile.Quantity));
		const int64_t priceMantissa = static_cast<int64_t>(target.OrderProfile.Ticks) * _globexTickMantissa;

		OrderCancelReplaceRequest replace = NewReplace(_securityId, side, record->ExchangeOrderId,
			quantity, priceMantissa, _senderId, _nextOrderRequestId++, _location);
		FormatClientOrderId(target.OrderHeader.ClientOrderId, replace.ClOrdID);
		record->Seq = target.OrderHeader.Seq;   // the replace is this revision
		if (OnOrderSent)
			OnOrderSent(target.OrderHeader.ClientOrderId, target.OrderProfile.Ticks,
				static_cast<int32_t>(quantity), side == SideReq::Buy);
		if (!_gateway->SendOrderCancelReplace(replace))
			PublishModifyReject(target.OrderHeader.ClientOrderId, record,
				Execution::OrderTargetAction::Amend, "session down: replace not sent");
	}

	// ---- inbound ----

	// An order was accepted and is now working: remember the exchange id and publish it active.
	void HandleNew(const ExecutionReportNew& report)
	{
		uint64_t clientOrderId = 0;
		if (!TryParseClientOrderId(report.ClOrdID, clientOrderId))
			return;
		Record* record = Find(clientOrderId);
		if (record != nullptr)
			record->ExchangeOrderId = report.OrderID;
		if (OnOrderLive)
			OnOrderLive(clientOrderId, report.OrderID);

		Execution::OrderState state{};
		state.OrderHeader.ClientOrderId = clientOrderId;   // the server fills in client/strategy ids
		state.OrderHeader.InstrumentId = _instrumentId;
		state.OrderHeader.Seq = record != nullptr ? record->Seq : 0;
		state.OrderHeader.ExchangeTimestamp = Tools::Timestamp(static_cast<int64_t>(report.TransactTime));
		state.OrderStateStatus = Execution::OrderStateStatus::Active;
		state.OrderProfile = record != nullptr ? record->OrderProfile : ProfileFrom(report.Price.Mantissa, report.OrderQty, report.Side);
		state.QuantityFilled = 0;
		if (OnOrderState)
			OnOrderState(state);
	}

	// An order was rejected by the exchange: publish the rejection with the reason text.
	void HandleReject(const ExecutionReportReject& report)
	{
		uint64_t clientOrderId = 0;
		if (!TryParseClientOrderId(report.ClOrdID, clientOrderId))
			return;
		Record* record = Find(clientOrderId);

		Execution::OrderRejected rejected{};
		rejected.OrderHeader.ClientOrderId = clientOrderId;
		rejected.OrderHeader.InstrumentId = _instrumentId;
		rejected.OrderHeader.Seq = record != nullptr ? record->Seq : 0;
		rejected.OrderTargetAction = Execution::OrderTargetAction::Create;
		rejected.OrderRejectedSource = Execution::OrderRejectedSource::Exchange;
		rejected.OrderProfile = record != nullptr ? record->OrderProfile : ProfileFrom(report.Price.Mantissa, report.OrderQty, report.Side);
		if (record != nullptr)
			record->ClientOrderId = 0;   // the order never worked, so free its slot
		if (OnOrderDone)
			OnOrderDone(clientOrderId);
		if (OnOrderRejected)
			OnOrderRejected(rejected, report.Text.ToString());
	}

	// An order was cancelled: publish it done and free its slot.
	void HandleCancel(const ExecutionReportCancel& report)
	{
		uint64_t clientOrderId = 0;
		if (!TryParseClientOrderId(report.ClOrdID, clientOrderId))
			return;
		Record* record = Find(clientOrderId);

		Execution::OrderState state{};
		state.OrderHeader.ClientOrderId = clientOrderId;
		state.OrderHeader.InstrumentId = _instrumentId;
		state.OrderHeader.Seq = record != nullptr ? record->Seq : 0;
		state.OrderHeader.ExchangeTimestamp = Tools::Timestamp(static_cast<int64_t>(report.TransactTime));
		state.OrderStateStatus = Execution::OrderStateStatus::Done;
		state.OrderProfile = record != nullptr ? record->OrderProfile : Execution::OrderProfile{};
		state.QuantityFilled = 0;
		if (record != nullptr)
			record->ClientOrderId = 0;   // the order is done, so free its slot
		if (OnOrderDone)
			OnOrderDone(clientOrderId);
		if (OnOrderState)
			OnOrderState(state);
	}

	// An order was modified: adopt the confirmed price/size into the slot and publish it active.
	void HandleModify(const ExecutionReportModify& report)
	{
		uint64_t clientOrderId = 0;
		if (!TryParseClientOrderId(report.ClOrdID, clientOrderId))
			return;
		Record* record = Find(clientOrderId);

		const Execution::OrderProfile confirmed = ProfileFrom(report.Price.Mantissa, report.OrderQty, report.Side);
		if (record != nullptr)
			record->OrderProfile = confirmed;
		if (OnOrderLive)
			OnOrderLive(clientOrderId, report.OrderID);

		Execution::OrderState state{};
		state.OrderHeader.ClientOrderId = clientOrderId;
		state.OrderHeader.InstrumentId = _instrumentId;
		state.OrderHeader.Seq = record != nullptr ? record->Seq : 0;
		state.OrderHeader.ExchangeTimestamp = Tools::Timestamp(static_cast<int64_t>(report.TransactTime));
		state.OrderStateStatus = Execution::OrderStateStatus::Active;
		state.OrderProfile = confirmed;
		state.QuantityFilled = SignedQuantity(report.CumQty, report.Side);
		if (OnOrderState)
			OnOrderState(state);
	}

	// An order traded: publish the fill, then the order's new state — done when nothing is left
	// working, else still active with the filled amount.
	void HandleFill(const ExecutionReportTradeOutright& report)
	{
		uint64_t clientOrderId = 0;
		if (!TryParseClientOrderId(report.ClOrdID, clientOrderId))
			return;
		Record* record = Find(clientOrderId);

		// Step 1: The fill itself: the traded price/size, maker or taker by the aggressor flag.
		Execution::Fill fill{};
		fill.OrderHeader.ClientOrderId = clientOrderId;
		fill.OrderHeader.InstrumentId = _instrumentId;
		fill.OrderHeader.Seq = record != nullptr ? record->Seq : 0;
		fill.OrderHeader.ExchangeTimestamp = Tools::Timestamp(static_cast<int64_t>(report.TransactTime));
		fill.FillType = report.AggressorIndicator == BooleanFlag::True ? Execution::FillType::Taker : Execution::FillType::Maker;
		fill.FillId = report.SecExecID;
		fill.OrderProfile = ProfileFrom(report.LastPx.Mantissa, report.LastQty, report.Side);
		if (OnFill)
			OnFill(fill);

		// Step 2: The order after the trade.
		const bool done = report.LeavesQty == 0;
		if (done)
		{
			if (OnOrderDone)
				OnOrderDone(clientOrderId);
		}
		else if (OnOrderQuantity)
			OnOrderQuantity(clientOrderId, static_cast<int32_t>(report.LeavesQty));
		Execution::OrderState state{};
		state.OrderHeader = fill.OrderHeader;
		state.OrderStateStatus = done ? Execution::OrderStateStatus::Done : Execution::OrderStateStatus::Active;
		state.OrderProfile = ProfileFrom(report.Price.Mantissa, report.OrderQty, report.Side);
		state.QuantityFilled = SignedQuantity(report.CumQty, report.Side);
		if (record != nullptr && done)
			record->ClientOrderId = 0;   // fully filled, so free its slot
		if (OnOrderState)
			OnOrderState(state);
	}

	// A cancel or replace was refused by the exchange: the order stays working as it was.
	void HandleModifyReject(const Tools::StringN<20>& clOrdId, const Tools::StringN<256>& text,
	                        Execution::OrderTargetAction action)
	{
		uint64_t clientOrderId = 0;
		if (!TryParseClientOrderId(clOrdId, clientOrderId))
			return;
		PublishModifyReject(clientOrderId, Find(clientOrderId), action, text.ToString());
	}

	// A refused cancel or replace, from the exchange or from a dead session alike: the order
	// stays working as it was, so keep the slot and publish the rejection with the reason.
	void PublishModifyReject(uint64_t clientOrderId, Record* record,
	                         Execution::OrderTargetAction action, const std::string& text)
	{
		Execution::OrderRejected rejected{};
		rejected.OrderHeader.ClientOrderId = clientOrderId;
		rejected.OrderHeader.InstrumentId = _instrumentId;
		rejected.OrderHeader.Seq = record != nullptr ? record->Seq : 0;
		rejected.OrderTargetAction = action;
		rejected.OrderRejectedSource = Execution::OrderRejectedSource::Exchange;
		rejected.OrderProfile = record != nullptr ? record->OrderProfile : Execution::OrderProfile{};
		if (action == Execution::OrderTargetAction::Amend && record != nullptr && record->ExchangeOrderId != 0)
		{
			// The replace was refused: the order still works at its old price. Re-arm the queue
			// tracking there (an estimate-grade reseed; the pending state at the new price is stale).
			if (OnOrderSent)
				OnOrderSent(clientOrderId, record->OrderProfile.Ticks,
					std::abs(record->OrderProfile.Quantity), record->OrderProfile.Sign() >= 0);
			if (OnOrderLive)
				OnOrderLive(clientOrderId, record->ExchangeOrderId);
		}
		if (OnOrderRejected)
			OnOrderRejected(rejected, text);
	}

	// An order the exchange never saw (its send died with the connection): free the slot,
	// disarm the queue tracking, and reject it back so the strategy can decide again.
	void FailUnsent(Record& record)
	{
		const uint64_t clientOrderId = record.ClientOrderId;
		Execution::OrderRejected rejected{};
		rejected.OrderHeader.ClientOrderId = clientOrderId;
		rejected.OrderHeader.InstrumentId = _instrumentId;
		rejected.OrderHeader.Seq = record.Seq;
		rejected.OrderTargetAction = Execution::OrderTargetAction::Create;
		rejected.OrderRejectedSource = Execution::OrderRejectedSource::Server;
		rejected.OrderProfile = record.OrderProfile;
		record.ClientOrderId = 0;
		if (OnOrderDone)
			OnOrderDone(clientOrderId);
		if (OnOrderRejected)
			OnOrderRejected(rejected, "session down: order never reached the exchange");
	}

	// ---- helpers ----

	// A wire quantity as the server's signed convention: positive buys, negative sells.
	static int32_t SignedQuantity(uint32_t quantity, SideReq side)
	{
		return side == SideReq::Buy ? static_cast<int32_t>(quantity) : -static_cast<int32_t>(quantity);
	}

	// Rebuild the server's tick/lot view of an order from an execution report's price and size.
	Execution::OrderProfile ProfileFrom(int64_t priceMantissa, uint32_t orderQty, SideReq side) const
	{
		Execution::OrderProfile profile{};
		profile.Ticks = _globexTickMantissa != 0 ? static_cast<int32_t>(priceMantissa / _globexTickMantissa) : 0;
		profile.Quantity = SignedQuantity(orderQty, side);
		return profile;
	}

	// Write the id into an order's text ClOrdID field, via the stack (no allocation).
	static void FormatClientOrderId(uint64_t clientOrderId, Tools::StringN<20>& clOrdId)
	{
		char buffer[24];
		char* end = std::to_chars(buffer, buffer + sizeof(buffer) - 1, clientOrderId).ptr;
		*end = '\0';
		clOrdId.Set(buffer);
	}

	// Read the id back out of a report's text ClOrdID field, straight from the fixed chars.
	static bool TryParseClientOrderId(const Tools::StringN<20>& clOrdId, uint64_t& value)
	{
		const char* begin = reinterpret_cast<const char*>(clOrdId.Chars);
		size_t length = 0;
		while (length < clOrdId.Capacity && clOrdId.Chars[length] != 0)
			++length;
		return length > 0 && std::from_chars(begin, begin + length, value).ec == std::errc{};
	}
};

} // namespace ILink3
