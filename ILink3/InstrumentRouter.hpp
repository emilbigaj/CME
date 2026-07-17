#pragma once

// Bridges the trading server and CME for one instrument: it turns the server's order intents
// (create / amend / cancel, expressed in ticks and lots) into iLink3 order messages, and turns
// CME's execution reports back into the server's order events (accepted, rejected, filled). One
// router exists per allocated instrument.
//
// The single thread that owns the segment's gateway drives this router for both directions —
// it drains order intents and sends, and it reads execution reports and reconciles — so the
// order book here needs no locking. Each live order is remembered by its client order id: the
// revision it is on, the price/size the server asked for, and (once accepted) the exchange's
// own order id for later amend or cancel.
//
// Prices cross a scale change at this boundary. The server works in ticks; the wire wants the
// exchange (Globex) price, which is a whole number of raw increments. One server tick equals a
// fixed Globex-price step, precomputed once as a nine-decimal mantissa, so turning a tick count
// into a wire price is a single multiply.

#include "ILink3Sbe.hpp"
#include "MarketSegmentGateway.hpp"
#include "Wire.hpp"
#include "Order.hpp"        // Execution::OrderTarget / OrderState / OrderRejected / Fill
#include "String.hpp"
#include "Timestamp.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <string>
#include <unordered_map>

namespace ILink3
{

class InstrumentRouter
{
	// ---- fixed identity + routing (set once) ----
	int32_t _instrumentId;                 // the server's compact id for this instrument
	int32_t _securityId;                   // CME's id, named on the wire
	MarketSegmentGateway* _gateway;        // the session this instrument's orders go out on
	int64_t _globexTickMantissa;           // wire price step (PRICE9 mantissa) per one server tick
	Tools::StringN<20> _senderId;          // operator id stamped on each order
	Tools::StringN<5> _location;           // desk location stamped on each order

	// ---- live order book (owned by the single driving thread) ----
	uint64_t _nextOrderRequestId = 1;      // a fresh request id per outbound order message

	// What we remember about one working order so an execution report can be reconciled to it.
	struct Record
	{
		int32_t Seq = 0;                       // the server revision this order is on
		Execution::OrderProfile OrderProfile;  // the price/size the server asked for
		uint64_t ExchangeOrderId = 0;          // CME's order id, known once the order is accepted
	};
	std::unordered_map<uint64_t, Record> _orders;   // keyed by client order id
	std::unordered_map<uint64_t, uint64_t> _clientOrderIdByExchangeOrderId;   // for reports keyed by exchange id

public:
	// Called with the server's events for this instrument once each execution report is decoded.
	// The owner wires these to the server (or, in a test, to a printer).
	std::function<void(const Execution::OrderState&)> OnOrderState;
	std::function<void(const Execution::OrderRejected&, const std::string&)> OnOrderRejected;
	std::function<void(const Execution::Fill&)> OnFill;

	InstrumentRouter(int32_t instrumentId, int32_t securityId, MarketSegmentGateway* gateway, double tickSize, double displayFactor, const std::string& senderId, const std::string& location)
		: _instrumentId(instrumentId),
		  _securityId(securityId),
		  _gateway(gateway),
		  // One server tick = tickSize display units = tickSize/displayFactor raw Globex units;
		  // times a billion gives the nine-decimal mantissa the wire carries.
		  _globexTickMantissa(static_cast<int64_t>(std::llround(tickSize / displayFactor)) * 1'000'000'000LL),
		  _senderId(senderId),
		  _location(location)
	{
	}

	int32_t InstrumentId() const { return _instrumentId; }
	int32_t SecurityID() const { return _securityId; }

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

	// Reconcile one decoded execution report and publish the resulting server event.
	void OnExecutionReport(const FramedMessage& message)
	{
		switch (message.TemplateId)
		{
			case ExecutionReportNew::TemplateId:    HandleNew(*message.As<ExecutionReportNew>()); break;
			case ExecutionReportReject::TemplateId: HandleReject(*message.As<ExecutionReportReject>()); break;
			case ExecutionReportCancel::TemplateId: HandleCancel(*message.As<ExecutionReportCancel>()); break;
			default: break;   // fills and modifies handled next
		}
	}

private:
	// ---- outbound ----

	// Send a new order and remember it under its client order id.
	void SendNew(const Execution::OrderTarget& target)
	{
		const uint64_t clientOrderId = target.OrderHeader.ClientOrderId;
		const int32_t sign = target.OrderProfile.Sign();
		const SideReq side = sign >= 0 ? SideReq::Buy : SideReq::Sell;
		const uint32_t quantity = static_cast<uint32_t>(std::abs(target.OrderProfile.Quantity));
		const int64_t priceMantissa = static_cast<int64_t>(target.OrderProfile.Ticks) * _globexTickMantissa;

		NewOrderSingle order = NewLimitOrder(_securityId, side, quantity, priceMantissa,
			std::to_string(clientOrderId), _senderId.ToString(), 0, _nextOrderRequestId++, _location.ToString());
		_orders[clientOrderId] = Record{target.OrderHeader.Seq, target.OrderProfile, 0};
		_gateway->SendNewOrderSingle(order);
	}

	// Cancel a working order, referencing it by the exchange id we kept from its acceptance.
	void SendCancel(const Execution::OrderTarget& target)
	{
		auto it = _orders.find(target.OrderHeader.ClientOrderId);
		if (it == _orders.end() || it->second.ExchangeOrderId == 0)
			return;   // nothing working to cancel

		const SideReq side = it->second.OrderProfile.Sign() >= 0 ? SideReq::Buy : SideReq::Sell;
		OrderCancelRequest cancel = NewCancel(_securityId, side, it->second.ExchangeOrderId,
			std::to_string(target.OrderHeader.ClientOrderId), _senderId.ToString(), _nextOrderRequestId++, _location.ToString());
		it->second.Seq = target.OrderHeader.Seq;   // the cancel is this revision
		_gateway->SendOrderCancel(cancel);
	}

	// Amend goes out next.
	void SendReplace(const Execution::OrderTarget&) {}

	// ---- inbound ----

	// An order was accepted and is now working: remember the exchange id and publish it active.
	void HandleNew(const ExecutionReportNew& report)
	{
		uint64_t clientOrderId = 0;
		if (!TryParseClientOrderId(report.ClOrdID, clientOrderId))
			return;

		auto it = _orders.find(clientOrderId);
		if (it != _orders.end())
		{
			it->second.ExchangeOrderId = report.OrderID;
			_clientOrderIdByExchangeOrderId[report.OrderID] = clientOrderId;   // for cancel/fill reports
		}

		Execution::OrderState state{};
		state.OrderHeader.ClientOrderId = clientOrderId;   // the server fills in client/strategy ids
		state.OrderHeader.InstrumentId = _instrumentId;
		state.OrderHeader.Seq = it != _orders.end() ? it->second.Seq : 0;
		state.OrderHeader.ExchangeTimestamp = Tools::Timestamp(static_cast<int64_t>(report.TransactTime));
		state.OrderStateStatus = Execution::OrderStateStatus::Active;
		state.OrderProfile = it != _orders.end() ? it->second.OrderProfile : ProfileFrom(report.Price.Mantissa, report.OrderQty, report.Side);
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

		auto it = _orders.find(clientOrderId);
		Execution::OrderProfile profile = it != _orders.end()
			? it->second.OrderProfile
			: ProfileFrom(report.Price.Mantissa, report.OrderQty, report.Side);

		Execution::OrderRejected rejected{};
		rejected.OrderHeader.ClientOrderId = clientOrderId;
		rejected.OrderHeader.InstrumentId = _instrumentId;
		rejected.OrderHeader.Seq = it != _orders.end() ? it->second.Seq : 0;
		rejected.OrderTargetAction = Execution::OrderTargetAction::Create;
		rejected.OrderRejectedSource = Execution::OrderRejectedSource::Exchange;
		rejected.OrderProfile = profile;
		if (it != _orders.end())
			_orders.erase(it);   // the order never worked, so drop it from the book
		if (OnOrderRejected)
			OnOrderRejected(rejected, report.Text.ToString());
	}

	// An order was cancelled: publish it done and drop it from the book. The report names the
	// order by the exchange id, so we resolve back to our client order id through that.
	void HandleCancel(const ExecutionReportCancel& report)
	{
		auto lookup = _clientOrderIdByExchangeOrderId.find(report.OrderID);
		if (lookup == _clientOrderIdByExchangeOrderId.end())
			return;
		const uint64_t clientOrderId = lookup->second;
		auto it = _orders.find(clientOrderId);

		Execution::OrderState state{};
		state.OrderHeader.ClientOrderId = clientOrderId;
		state.OrderHeader.InstrumentId = _instrumentId;
		state.OrderHeader.Seq = it != _orders.end() ? it->second.Seq : 0;
		state.OrderHeader.ExchangeTimestamp = Tools::Timestamp(static_cast<int64_t>(report.TransactTime));
		state.OrderStateStatus = Execution::OrderStateStatus::Done;
		state.OrderProfile = it != _orders.end() ? it->second.OrderProfile : Execution::OrderProfile{};
		state.QuantityFilled = 0;

		_clientOrderIdByExchangeOrderId.erase(lookup);
		if (it != _orders.end())
			_orders.erase(it);
		if (OnOrderState)
			OnOrderState(state);
	}

	// ---- helpers ----

	// Rebuild the server's tick/lot view of an order from an execution report's price and size.
	Execution::OrderProfile ProfileFrom(int64_t priceMantissa, uint32_t orderQty, SideReq side) const
	{
		Execution::OrderProfile profile{};
		profile.Ticks = _globexTickMantissa != 0 ? static_cast<int32_t>(priceMantissa / _globexTickMantissa) : 0;
		const int32_t sign = side == SideReq::Buy ? 1 : -1;
		profile.Quantity = static_cast<int32_t>(orderQty) * sign;
		return profile;
	}

	// The client order id is carried as text in ClOrdID; parse it back to a number.
	static bool TryParseClientOrderId(const Tools::StringN<20>& clOrdId, uint64_t& value)
	{
		const std::string text = clOrdId.ToString();
		return std::from_chars(text.data(), text.data() + text.size(), value).ec == std::errc{};
	}
};

} // namespace ILink3
