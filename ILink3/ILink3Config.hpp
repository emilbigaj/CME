#pragma once

// All the per-account settings needed to log in to CME and route orders: the identity and
// secret from the credential sheet CME issues, plus the list of market-segment gateways to
// connect to. It is loaded once at start-up from a settings file and never touched on the
// fast path.
//
// Field names deliberately match the words on CME's credential sheet exactly, so the
// settings file, this struct, and CME's paperwork all read the same. There is one settings
// file per environment, at Config/<Environment>/ILink3.json. Load() reads and checks a file;
// Save() writes it back in full (secret included, so those files stay out of version
// control); ToString() is the log-safe view, with the secret masked.

#include "Json.hpp"      // Tools::Json (reads/writes these structs as text)
#include "String.hpp"    // Tools::StringN (fixed-width text fields)

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace ILink3
{

// Which CME environment a settings file targets; the names match the sheet's own wording.
enum class Environment : uint8_t
{
	Cert = 0,
	NewRelease = 1,
	Production = 2,
};

// Holds the account secret. The trick here: it reads and writes to a settings file at full
// fidelity (so the real key survives a load/save round trip), but the owning config masks it
// in ToString(), so a stray log line can never leak the key.
struct Secret
{
	std::string Value;

	// Take the value read in from the settings file.
	void Set(const std::string& value)
	{
		Value = value;
	}

	// Hand the value back when writing the settings file.
	std::string Get() const
	{
		return Value;
	}

	// A safe-to-print form: first few characters plus the length, never the whole key.
	std::string Masked() const
	{
		if (Value.empty())
			return "(unset)";
		return Value.substr(0, 4) + "****(redacted, length " + std::to_string(Value.size()) + ")";
	}

	// True once a real key has been loaded.
	bool IsSet() const
	{
		return !Value.empty();
	}

	// Text mapping: read through Set(), write through Get().
	struct glaze
	{
		static constexpr auto value = glz::custom<&Secret::Set, &Secret::Get>;
	};
};

// One market-segment gateway from the credential sheet: an identifier, a human description,
// and the two addresses (a primary and a backup) that serve that group of products. In the
// direct-connection model each connection talks to exactly one of these.
struct MarketSegment
{
	int32_t MarketSegmentID = 0;
	std::string Description;
	std::string PrimaryIPAddress;
	std::string SecondaryIPAddress;

	// This segment as text.
	std::string ToString() const
	{
		return Tools::Json::Serialize(*this);
	}

	// Text mapping (keys equal the member names).
	struct glaze
	{
		using T = MarketSegment;
		static constexpr auto value = glz::object(
			"MarketSegmentID", &T::MarketSegmentID,
			"Description", &T::Description,
			"PrimaryIPAddress", &T::PrimaryIPAddress,
			"SecondaryIPAddress", &T::SecondaryIPAddress);
	};
};

// The name, version, and vendor of our trading application. CME wants this identity in the
// second logon message, and it is also part of that message's signing string.
struct TradingSystemConfig
{
	Tools::StringN<30> Name = std::string("CelerityMarkets");
	Tools::StringN<10> Version = std::string("1.0");
	Tools::StringN<10> Vendor = std::string("Internal");

	// This identity as text.
	std::string ToString() const
	{
		return Tools::Json::Serialize(*this);
	}

	// Text mapping (keys equal the member names).
	struct glaze
	{
		using T = TradingSystemConfig;
		static constexpr auto value = glz::object(
			"Name", &T::Name,
			"Version", &T::Version,
			"Vendor", &T::Vendor);
	};
};

// The full settings for one environment: identity, secret, gateways, and session tuning.
// Everything a session needs to connect and log in comes from one of these.
struct ILink3Config
{
	// ---- straight from the credential sheet, same names ----
	ILink3::Environment Environment = ILink3::Environment::Cert;
	Tools::StringN<5> GFID;          // Globex firm id, sent as the logon "Firm" field
	Tools::StringN<3> SessionID;     // sent as the logon "Session" field
	Tools::StringN<20> AccessID;     // sent as the logon "access key" field
	Secret SecretKey;                // decoded from url-safe base64 to get the signing key
	uint16_t Port = 0;               // one port shared by every gateway in the environment
	std::string CreationDate;
	std::string ExpirationDate;
	std::vector<MarketSegment> MarketSegments;

	// ---- session settings (the second logon message also signs over these) ----
	TradingSystemConfig TradingSystem;
	uint16_t KeepAliveIntervalMs = 30000;   // CME allows 5000..60000

	// Find the gateway entry for a market-segment id, or null if the config has none.
	const MarketSegment* TryFindMarketSegment(int32_t marketSegmentId) const
	{
		// Step 1: Linear scan — the list is short (a couple dozen entries at most).
		for (const MarketSegment& segment : MarketSegments)
			if (segment.MarketSegmentID == marketSegmentId)
				return &segment;

		// Step 2: No match.
		return nullptr;
	}

	// Check every required field is present and sane; throws with the exact reason if not.
	void Validate() const
	{
		// Step 1: The identity fields must all be filled in.
		if (GFID.ToString().empty())
			throw std::invalid_argument("ILink3Config: GFID is required (up to 5 chars)");
		if (SessionID.ToString().empty())
			throw std::invalid_argument("ILink3Config: SessionID is required (3 chars)");
		if (AccessID.ToString().empty())
			throw std::invalid_argument("ILink3Config: AccessID is required (20 chars)");
		if (!SecretKey.IsSet())
			throw std::invalid_argument("ILink3Config: SecretKey is required");

		// Step 2: There must be a port and at least one gateway to connect to.
		if (Port == 0)
			throw std::invalid_argument("ILink3Config: Port is required");
		if (MarketSegments.empty())
			throw std::invalid_argument("ILink3Config: at least one MarketSegment is required");

		// Step 3: Each gateway entry must have an id and a primary address.
		for (const MarketSegment& segment : MarketSegments)
		{
			if (segment.MarketSegmentID <= 0 || segment.PrimaryIPAddress.empty())
				throw std::invalid_argument("ILink3Config: MarketSegment " + std::to_string(segment.MarketSegmentID) + " needs MarketSegmentID and PrimaryIPAddress");
		}

		// Step 4: The keep-alive interval must be within CME's accepted range.
		if (KeepAliveIntervalMs < 5000 || KeepAliveIntervalMs > 60000)
			throw std::invalid_argument("ILink3Config: KeepAliveIntervalMs must be 5000..60000");
	}

	// Read a settings file, check it, and return the config; throws on a bad file.
	static ILink3Config Load(const std::filesystem::path& filePath)
	{
		// Step 1: Open the file.
		std::ifstream stream(filePath);
		if (!stream.is_open())
			throw std::runtime_error("ILink3Config::Load: cannot open " + filePath.string());

		// Step 2: Read it whole into memory.
		std::stringstream buffer;
		buffer << stream.rdbuf();

		// Step 3: Parse the text into a config and validate before handing it back.
		ILink3Config config = Tools::Json::Deserialize<ILink3Config>(buffer.str());
		config.Validate();
		return config;
	}

	// Write the config back to a file at full fidelity (secret in clear) — never for logs.
	void Save(const std::filesystem::path& filePath) const
	{
		// Step 1: Open (or truncate) the target file.
		std::ofstream stream(filePath, std::ios::out | std::ios::trunc);
		if (!stream.is_open())
			throw std::runtime_error("ILink3Config::Save: cannot open " + filePath.string());

		// Step 2: Serialize the whole config as text.
		stream << Tools::Json::Serialize(*this);
	}

	// A log-safe view of the config: identical to Save() output, but with the secret masked.
	std::string ToString() const
	{
		// Step 1: Copy, then overwrite the copy's secret with its masked form.
		ILink3Config copy = *this;
		copy.SecretKey.Value = SecretKey.Masked();

		// Step 2: Serialize the redacted copy.
		return Tools::Json::Serialize(copy);
	}

	// Text mapping (keys equal the member names, matching the credential sheet).
	struct glaze
	{
		using T = ILink3Config;
		static constexpr auto value = glz::object(
			"Environment", &T::Environment,
			"GFID", &T::GFID,
			"SessionID", &T::SessionID,
			"AccessID", &T::AccessID,
			"SecretKey", &T::SecretKey,
			"Port", &T::Port,
			"CreationDate", &T::CreationDate,
			"ExpirationDate", &T::ExpirationDate,
			"MarketSegments", &T::MarketSegments,
			"TradingSystem", &T::TradingSystem,
			"KeepAliveIntervalMs", &T::KeepAliveIntervalMs);
	};
};

} // namespace ILink3
