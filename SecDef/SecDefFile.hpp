#pragma once

// Loads the CME security-definition ("secdef") flat file and turns each tradeable instrument
// into the HFT library's own instrument header, ready to be committed to the trading server's
// catalog. We keep only the two kinds the strategy trades: outright futures and the simple
// one-to-one calendar spreads built from two of them. Options and the more exotic multi-leg
// strategies are skipped.
//
// The file is a line per instrument, each line a list of FIX tag=value fields separated by the
// start-of-heading byte (0x01). We read it once at start-up (a cold path), so clarity beats
// speed here. Futures are built as we read them; a spread is resolved afterwards by looking up
// its two legs among the futures we already parsed, so the spread header can carry each leg's
// expiry.
//
// The HFT header does not carry the exchange's own ids, so each loaded record keeps the three
// CME-specific values the order and market-data paths need: the SecurityID that names the
// instrument on the wire, the MarketSegmentID that says which order gateway to route to, and
// the market-data Channel it publishes on.

#include "Instrument.hpp"   // Data::InstrumentHeader128 / FutureHeader / SpreadHeader / ExpiryType
#include "String.hpp"       // Tools::String8
#include "Timestamp.hpp"    // Tools::Timestamp

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace SecDef
{

// One instrument from the file: the HFT header the server will commit, plus the exchange ids
// that the header itself does not carry (routing and market-data identity). For a spread the
// two leg SecurityIDs are kept as well, so its legs can be resolved to allocated instruments
// later.
struct Loaded
{
	Data::InstrumentHeader128 Header;   // the Future or Spread variant, ready for the catalog
	int32_t SecurityID = 0;             // CME tag 48 — names the instrument on every order (wire)
	int32_t MarketSegmentID = 0;        // CME tag 1300 — which order-entry gateway to route to
	int32_t Channel = 0;                // CME tag 1180 — the market-data channel it publishes on
	int32_t LongLegSecurityID = 0;      // spread only: the buy leg's SecurityID
	int32_t ShortLegSecurityID = 0;     // spread only: the sell leg's SecurityID

	// Today's price band (conventional prices). An order outside [Low, High] is rejected; the
	// reference is the prior settlement, a rough anchor for where the market is.
	double LowLimitPrice = 0.0;         // tag 1148 — lowest valid order price today
	double HighLimitPrice = 0.0;        // tag 1149 — highest valid order price today
	double ReferencePrice = 0.0;        // tag 1150 — prior settlement price

	// The order wire carries the exchange (Globex) price, not the conventional one:
	// conventionalPrice = GlobexPrice x DisplayFactor. Divide a conventional price by this to get
	// the Globex price the order must send (a whole number of raw ticks).
	double DisplayFactor = 1.0;         // tag 9787
};

class SecDefFile
{
	std::vector<Loaded> _instruments;
	std::unordered_map<int32_t, size_t> _bySecurityId;    // SecurityID -> index into _instruments
	std::unordered_map<std::string, int32_t> _bySymbol;   // exchange symbol -> SecurityID
	size_t _futureCount = 0;
	size_t _spreadCount = 0;

public:
	// Read and parse the whole file. Throws if it cannot be opened.
	static SecDefFile Load(const std::filesystem::path& filePath)
	{
		// Step 1: Open the file.
		std::ifstream stream(filePath);
		if (!stream.is_open())
			throw std::runtime_error("SecDefFile::Load: cannot open " + filePath.string());

		SecDefFile file;

		// Step 2: First pass — build every future immediately, and set the spread lines aside
		// (a spread needs its legs' expiries, which come from the futures).
		std::vector<Loaded> pendingSpreads;
		std::string line;
		while (std::getline(stream, line))
		{
			std::optional<Parsed> parsed = ParseLine(line);
			if (!parsed)
				continue;
			if (parsed->Kind == InstrumentKind::Future)
				file.AddInstrument(BuildFuture(*parsed), parsed->Symbol);
			else
				pendingSpreads.push_back(BuildSpreadShell(*parsed));
		}

		// Step 3: Second pass — resolve each spread's legs against the futures, then add it.
		for (Loaded& spread : pendingSpreads)
			file.ResolveAndAddSpread(spread);

		return file;
	}

	const std::vector<Loaded>& Instruments() const { return _instruments; }
	size_t FutureCount() const { return _futureCount; }
	size_t SpreadCount() const { return _spreadCount; }

	// Find an instrument by its exchange SecurityID, or null if we did not load it.
	const Loaded* FindBySecurityId(int32_t securityId) const
	{
		auto it = _bySecurityId.find(securityId);
		return it == _bySecurityId.end() ? nullptr : &_instruments[it->second];
	}

	// Find an instrument by its exchange symbol (e.g. "ESZ5"), or null if not present.
	const Loaded* FindBySymbol(const std::string& symbol) const
	{
		auto it = _bySymbol.find(symbol);
		return it == _bySymbol.end() ? nullptr : FindBySecurityId(it->second);
	}

private:
	enum class InstrumentKind { Future, Spread };

	// One leg of a spread as read from the file.
	struct ParsedLeg
	{
		int32_t SecurityID = 0;
		int32_t Side = 0;      // 1 buy, 2 sell
		int32_t Ratio = 0;
	};

	// The raw fields we pull off one line, before deciding what to build from them.
	struct Parsed
	{
		InstrumentKind Kind = InstrumentKind::Future;
		int32_t SecurityID = 0;
		int32_t MarketSegmentID = 0;
		int32_t Channel = 0;
		std::string Symbol;
		std::string Exchange;
		std::string Root;
		double MinPriceIncrement = 0.0;   // tag 969, in the instrument's raw price units
		double DisplayFactor = 1.0;       // tag 9787, raw price x this = conventional (display) price
		double Multiplier = 0.0;
		double LowLimitRaw = 0.0;         // tag 1148, raw
		double HighLimitRaw = 0.0;        // tag 1149, raw
		double ReferenceRaw = 0.0;        // tag 1150, raw
		Tools::Timestamp Expiry{};
		Data::ExpiryType ExpiryType = Data::ExpiryType::Month;
		std::vector<ParsedLeg> Legs;
	};

	// Parse one line into its fields; return nothing for the kinds we do not trade (options and
	// non-calendar strategies).
	static std::optional<Parsed> ParseLine(const std::string& line)
	{
		// Step 1: Walk the tag=value fields, splitting on the start-of-heading byte.
		Parsed p;
		std::string securityType, subType, maturity;
		int32_t currentEventType = 0;
		const char* at = line.data();
		const char* end = at + line.size();
		while (at < end)
		{
			const char* fieldEnd = at;
			while (fieldEnd < end && *fieldEnd != '\x01')
				++fieldEnd;
			const char* eq = at;
			while (eq < fieldEnd && *eq != '=')
				++eq;
			if (eq < fieldEnd)
			{
				const int32_t tag = static_cast<int32_t>(ParseInt(std::string_view(at, static_cast<size_t>(eq - at))));
				const std::string_view value(eq + 1, static_cast<size_t>(fieldEnd - eq - 1));
				ApplyField(p, tag, value, securityType, subType, maturity, currentEventType);
			}
			at = fieldEnd + 1;
		}

		// Step 2: Keep only futures and one-to-one calendar spreads.
		if (securityType != "FUT")
			return std::nullopt;   // options (OOF) and the like
		if (p.Legs.empty())
		{
			p.Kind = InstrumentKind::Future;
		}
		else
		{
			if (!IsOneToOneCalendar(p.Legs))
				return std::nullopt;   // butterfly, strip, and other multi-leg strategies
			p.Kind = InstrumentKind::Spread;
		}

		// Step 3: The header's symbology needs an exchange and a root; drop anything missing them.
		if (p.Exchange.empty() || p.Root.empty())
			return std::nullopt;

		// Step 4: Fall back to the contract month for the expiry if no expiration event was given.
		if (p.Expiry.NanosSinceEpoch == 0 && maturity.size() >= 6)
			p.Expiry = Tools::Timestamp(static_cast<int32_t>(ParseInt(std::string_view(maturity).substr(0, 4))),
			                            static_cast<int32_t>(ParseInt(std::string_view(maturity).substr(4, 2))), 1);
		return p;
	}

	// Copy one field into the parse state, keyed by its FIX tag.
	static void ApplyField(Parsed& p, int32_t tag, std::string_view value, std::string& securityType,
	                       std::string& subType, std::string& maturity, int32_t& currentEventType)
	{
		switch (tag)
		{
			case 48:   p.SecurityID = static_cast<int32_t>(ParseInt(value)); break;      // SecurityID
			case 1300: p.MarketSegmentID = static_cast<int32_t>(ParseInt(value)); break; // MarketSegmentID
			case 1180: p.Channel = static_cast<int32_t>(ParseInt(value)); break;         // ApplID (channel)
			case 55:   p.Symbol.assign(value); break;                                    // Symbol
			case 207:  p.Exchange.assign(value); break;                                  // SecurityExchange
			case 6937: p.Root.assign(value); break;                                      // Asset (root)
			case 167:  securityType.assign(value); break;                                // SecurityType
			case 762:  subType.assign(value); break;                                     // SecuritySubType
			case 200:  maturity.assign(value); break;                                    // MaturityMonthYear
			case 969:  p.MinPriceIncrement = ParseDouble(value); break;                  // MinPriceIncrement (raw)
			case 9787: p.DisplayFactor = ParseDouble(value); break;                      // DisplayFactor (raw->display)
			case 1147: p.Multiplier = ParseDouble(value); break;                         // UnitOfMeasureQty
			case 1148: p.LowLimitRaw = ParseDouble(value); break;                        // LowLimitPrice (raw)
			case 1149: p.HighLimitRaw = ParseDouble(value); break;                       // HighLimitPrice (raw)
			case 1150: p.ReferenceRaw = ParseDouble(value); break;                       // TradingReferencePrice (raw)
			case 865:  currentEventType = static_cast<int32_t>(ParseInt(value)); break;  // EventType
			case 1145: if (currentEventType == 7) p.Expiry = ParseEventTime(value); break;  // EventTime -> expiry
			case 602:  p.Legs.push_back(ParsedLeg{static_cast<int32_t>(ParseInt(value)), 0, 0}); break;  // LegSecurityID
			case 624:  if (!p.Legs.empty()) p.Legs.back().Side = static_cast<int32_t>(ParseInt(value)); break;   // LegSide
			case 623:  if (!p.Legs.empty()) p.Legs.back().Ratio = static_cast<int32_t>(ParseInt(value)); break;  // LegRatioQty
			default:   break;
		}
	}

	// A one-to-one calendar spread is exactly two legs, each ratio one, one bought and one sold.
	static bool IsOneToOneCalendar(const std::vector<ParsedLeg>& legs)
	{
		if (legs.size() != 2)
			return false;
		if (legs[0].Ratio != 1 || legs[1].Ratio != 1)
			return false;
		return (legs[0].Side == 1 && legs[1].Side == 2) || (legs[0].Side == 2 && legs[1].Side == 1);
	}

	// Fill the shared part of an instrument header (identity, venue, price scale).
	static void FillCommon(Data::InstrumentHeader& header, const Parsed& p, Data::InstrumentType type)
	{
		header.Header = Data::Header<Data::InstrumentType>(Data::InstrumentType::Instrument);
		header.InstrumentType = type;
		header.CoreGroupId = 0;                 // assigned per server segment map at commit time
		header.Exchange = Tools::String8(p.Exchange);
		header.Root = Tools::String8(p.Root);
		header.InstrumentHeaderId = -1;         // assigned when committed to the server catalog
		header.InstrumentId = -1;               // assigned when a client allocates it
		// The conventional (display) tick is the raw increment scaled by the display factor:
		// e.g. E-mini S&P 500 is 25 raw x 0.01 = 0.25 index points.
		const double tickSize = p.MinPriceIncrement * p.DisplayFactor;
		header.TickSize = tickSize;
		header.InverseTickSize = tickSize > 0.0 ? 1.0 / tickSize : 0.0;
		header.IsInSession = true;
	}

	// Copy the exchange ids and today's price band (scaled to conventional prices) onto a record.
	static void FillLoadedIds(Loaded& loaded, const Parsed& p)
	{
		loaded.SecurityID = p.SecurityID;
		loaded.MarketSegmentID = p.MarketSegmentID;
		loaded.Channel = p.Channel;
		loaded.LowLimitPrice = p.LowLimitRaw * p.DisplayFactor;
		loaded.HighLimitPrice = p.HighLimitRaw * p.DisplayFactor;
		loaded.ReferencePrice = p.ReferenceRaw * p.DisplayFactor;
		loaded.DisplayFactor = p.DisplayFactor;
	}

	// Build a future header from a parsed line.
	static Loaded BuildFuture(const Parsed& p)
	{
		Loaded loaded;
		FillLoadedIds(loaded, p);

		Data::FutureHeader& future = loaded.Header.AsFuture();
		FillCommon(future.InstrumentHeader, p, Data::InstrumentType::Future);
		future.Multiplier = p.Multiplier;
		future.ExpiryDate = p.Expiry;
		future.ExpiryType = p.ExpiryType;
		return loaded;
	}

	// Build the parts of a spread header we know from its own line; its legs' expiries are filled
	// in later once the legs are resolved.
	static Loaded BuildSpreadShell(const Parsed& p)
	{
		Loaded loaded;
		FillLoadedIds(loaded, p);
		loaded.LongLegSecurityID = p.Legs[0].Side == 1 ? p.Legs[0].SecurityID : p.Legs[1].SecurityID;
		loaded.ShortLegSecurityID = p.Legs[0].Side == 2 ? p.Legs[0].SecurityID : p.Legs[1].SecurityID;

		Data::SpreadHeader& spread = loaded.Header.AsSpread();
		FillCommon(spread.InstrumentHeader, p, Data::InstrumentType::Spread);
		spread.Multiplier = p.Multiplier;
		spread.LongInstrumentId = -1;   // resolved to an allocated instrument later
		spread.ShortInstrumentId = -1;
		spread.LongExpiryType = Data::ExpiryType::Month;
		spread.ShortExpiryType = Data::ExpiryType::Month;
		return loaded;
	}

	// Add a built instrument and index it by SecurityID and symbol.
	void AddInstrument(Loaded loaded, const std::string& symbol)
	{
		const int32_t securityId = loaded.SecurityID;
		const size_t index = _instruments.size();
		if (loaded.Header.Base.InstrumentType == Data::InstrumentType::Future)
			++_futureCount;
		else
			++_spreadCount;
		_instruments.push_back(std::move(loaded));
		_bySecurityId.emplace(securityId, index);
		if (!symbol.empty())
			_bySymbol.emplace(symbol, securityId);
	}

	// Fill a spread's leg expiries from the already-loaded leg futures, then add it. Spreads whose
	// legs are not both present as futures are dropped.
	void ResolveAndAddSpread(Loaded& spread)
	{
		const Loaded* longLeg = FindBySecurityId(spread.LongLegSecurityID);
		const Loaded* shortLeg = FindBySecurityId(spread.ShortLegSecurityID);
		if (longLeg == nullptr || shortLeg == nullptr)
			return;

		Data::SpreadHeader& header = spread.Header.AsSpread();
		header.LongExpiryDate = longLeg->Header.AsFuture().ExpiryDate;
		header.ShortExpiryDate = shortLeg->Header.AsFuture().ExpiryDate;
		header.LongExpiryType = longLeg->Header.AsFuture().ExpiryType;
		header.ShortExpiryType = shortLeg->Header.AsFuture().ExpiryType;
		AddInstrument(std::move(spread), "");   // spread symbols carry punctuation; look up by SecurityID
	}

	// ---- small parse helpers ----

	static int64_t ParseInt(std::string_view s)
	{
		int64_t value = 0;
		std::from_chars(s.data(), s.data() + s.size(), value);
		return value;
	}

	static double ParseDouble(std::string_view s)
	{
		double value = 0.0;
		std::from_chars(s.data(), s.data() + s.size(), value);
		return value;
	}

	// Turn a CME event timestamp ("YYYYMMDDHHMMSS...") into a Timestamp; only the parts present
	// are used.
	static Tools::Timestamp ParseEventTime(std::string_view s)
	{
		if (s.size() < 8)
			return Tools::Timestamp{};
		auto part = [&](size_t offset, size_t length) -> int32_t
		{
			return static_cast<int32_t>(ParseInt(s.substr(offset, length)));
		};
		const int32_t year = part(0, 4), month = part(4, 2), day = part(6, 2);
		const int32_t hour = s.size() >= 10 ? part(8, 2) : 0;
		const int32_t minute = s.size() >= 12 ? part(10, 2) : 0;
		const int32_t second = s.size() >= 14 ? part(12, 2) : 0;
		return Tools::Timestamp(year, month, day, hour, minute, second);
	}
};

} // namespace SecDef
