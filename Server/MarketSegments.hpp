#pragma once

// The market segments this machine trades and how each maps onto the hardware. A market
// segment is the unit everything is organised around — one order-entry gateway, one
// market-data channel, one shared-memory core group, and the cores its threads pin to — so
// one segment's whole pipeline (market data, execution, and the strategy beside them) shares
// one core complex and its cache.
//
// The mapping is a property of the machine, not the environment — the same segment layout
// serves New Release and Production — so each environment keeps an identical copy beside its
// server settings (/mnt/S/CME/<Environment>/Config/MarketSegments.json), referenced from there.

#include "Json.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace Server
{

// One market segment: its name, the venue identities it joins, and the machine resources it
// owns.
struct MarketSegment
{
	std::string Name;               // what this segment is called, e.g. "S&P 500"
	int32_t MarketSegmentID = 0;    // the order-entry gateway (iLink3 market segment)
	int32_t Channel = 0;            // the market-data channel publishing its products
	int32_t CoreGroupId = 0;        // the shared-memory channel strategies reach it on (1..7)
	int32_t MarketDataCore = 0;     // the core its market-data thread pins to
	int32_t ExecutionCore = 0;      // the core its execution thread (gateway owner) pins to

	std::string ToString() const
	{
		return Tools::Json::Serialize(*this);
	}

	struct glaze
	{
		using T = MarketSegment;
		static constexpr auto value = glz::object(
			"Name", &T::Name,
			"MarketSegmentID", &T::MarketSegmentID,
			"Channel", &T::Channel,
			"CoreGroupId", &T::CoreGroupId,
			"MarketDataCore", &T::MarketDataCore,
			"ExecutionCore", &T::ExecutionCore);
	};
};

// The full segment list, loaded from its own settings file.
struct MarketSegments
{
	std::vector<MarketSegment> Segments;

	// Check every segment is fully specified and the core groups are distinct and in range.
	void Validate() const
	{
		// Step 1: There must be at least one segment.
		if (Segments.empty())
			throw std::invalid_argument("MarketSegments: at least one segment is required");

		// Step 2: Each fully specified, core groups distinct and in range.
		for (const MarketSegment& segment : Segments)
		{
			if (segment.Name.empty())
				throw std::invalid_argument("MarketSegments: a segment needs a Name");
			if (segment.MarketSegmentID <= 0 || segment.Channel <= 0)
				throw std::invalid_argument("MarketSegments: a segment needs MarketSegmentID and Channel");
			if (segment.CoreGroupId < 1 || segment.CoreGroupId > 7)
				throw std::invalid_argument("MarketSegments: CoreGroupId must be 1..7 (0 is admin)");
			for (const MarketSegment& other : Segments)
				if (&other != &segment && other.CoreGroupId == segment.CoreGroupId)
					throw std::invalid_argument("MarketSegments: CoreGroupId " + std::to_string(segment.CoreGroupId) + " used twice");
		}
	}

	// Read a settings file, check it, and return the segments; throws on a bad file.
	static MarketSegments Load(const std::filesystem::path& filePath)
	{
		// Step 1: Open and read the file whole.
		std::ifstream stream(filePath);
		if (!stream.is_open())
			throw std::runtime_error("MarketSegments::Load: cannot open " + filePath.string());
		std::stringstream buffer;
		buffer << stream.rdbuf();

		// Step 2: Parse and validate before handing it back.
		MarketSegments segments = Tools::Json::Deserialize<MarketSegments>(buffer.str());
		segments.Validate();
		return segments;
	}

	std::string ToString() const
	{
		return Tools::Json::Serialize(*this);
	}

	struct glaze
	{
		using T = MarketSegments;
		static constexpr auto value = glz::object(
			"MarketSegments", &T::Segments);
	};
};

} // namespace Server
