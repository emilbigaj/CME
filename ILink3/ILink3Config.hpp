#pragma once

// iLink 3 FIXP session configuration: everything Negotiate(500)/Establish(503) need to
// sign on and where to connect. Cold path only — loaded once at startup from JSON.
//
// SECURITY: SecretKey deserializes normally (so config files load), but serializes
// REDACTED — ToString()/logging can never leak the key. Config files live under
// Config/<Environment>/ILink3.json; never commit one holding a real SecretKey.

#include "Json.hpp"      // Tools::Json + enum glaze meta (magic_enum)
#include "String.hpp"    // Tools::StringN
#include "Tools.hpp"

#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace ILink3
{

// CME environment this session points at. Drives which gateway/creds file is used;
// one build, environment picked by config (never by compile flag).
enum class Environment : uint8_t
{
	Cert = 0,
	NewRelease = 1,
	Production = 2,
};

// A secret that can be read from JSON but never written back out in clear.
// glaze read side = Set() (normal), write side = Masked() (first 4 chars + length only).
struct Secret
{
	std::string Value;

	void Set(const std::string& value)
	{
		Value = value;
	}

	std::string Masked() const
	{
		if (Value.empty())
			return "(unset)";
		return Value.substr(0, 4) + "****(redacted, length " + std::to_string(Value.size()) + ")";
	}

	bool IsSet() const
	{
		return !Value.empty();
	}

	struct glaze
	{
		static constexpr auto value = glz::custom<&Secret::Set, &Secret::Masked>;
	};
};

// One TCP endpoint of an iLink gateway.
struct Gateway
{
	std::string Host;   // IP or DNS name of the MSGW/CGW
	uint16_t Port = 0;

	std::string ToString() const
	{
		return Tools::Json::Serialize(*this);
	}

	struct glaze
	{
		using T = Gateway;
		static constexpr auto value = glz::object(
			"Host", &T::Host,
			"Port", &T::Port);
	};
};

struct ILink3Config
{
	ILink3::Environment Environment = ILink3::Environment::Cert;

	// ---- identity (wire widths match the schema: String3Req / String5Req / String20Req) ----
	Tools::StringN<3> SessionId;      // FIXP tag 39006 Session
	Tools::StringN<5> Firm;           // FIXP tag 39007 Firm
	Tools::StringN<20> AccessKeyId;   // FIXP tag 39004 AccessKeyID
	Secret SecretKey;                 // base64url encoded; HMAC key = base64url-decode(SecretKey)

	// ---- Establish(503) fields that are also part of its HMAC canonical string ----
	Tools::StringN<30> TradingSystemName = std::string("HFT");
	Tools::StringN<10> TradingSystemVersion = std::string("1.0");
	Tools::StringN<10> TradingSystemVendor = std::string("Internal");
	uint16_t KeepAliveIntervalMs = 30000;   // CME allows 5000..60000

	// ---- where to connect ----
	Gateway Primary;
	Gateway Backup;    // same UUID must be reused here for active-active fault tolerance

	// ---- validation (throws with a precise reason; call after loading) ----
	void Validate() const
	{
		if (SessionId.ToString().empty())
			throw std::invalid_argument("ILink3Config: SessionId is required (3 chars)");
		if (Firm.ToString().empty())
			throw std::invalid_argument("ILink3Config: Firm is required (up to 5 chars)");
		if (AccessKeyId.ToString().empty())
			throw std::invalid_argument("ILink3Config: AccessKeyId is required (20 chars)");
		if (!SecretKey.IsSet())
			throw std::invalid_argument("ILink3Config: SecretKey is required (base64url)");
		if (KeepAliveIntervalMs < 5000 || KeepAliveIntervalMs > 60000)
			throw std::invalid_argument("ILink3Config: KeepAliveIntervalMs must be 5000..60000");
		if (Primary.Host.empty() || Primary.Port == 0)
			throw std::invalid_argument("ILink3Config: Primary gateway host/port required");
	}

	static ILink3Config Load(const std::string& filePath)
	{
		std::ifstream stream(filePath);
		if (!stream.is_open())
			throw std::runtime_error("ILink3Config::Load: cannot open " + filePath);

		std::stringstream buffer;
		buffer << stream.rdbuf();

		ILink3Config config = Tools::Json::Deserialize<ILink3Config>(buffer.str());
		config.Validate();
		return config;
	}

	// Safe to log: SecretKey comes out masked.
	std::string ToString() const
	{
		return Tools::Json::Serialize(*this);
	}

	struct glaze
	{
		using T = ILink3Config;
		static constexpr auto value = glz::object(
			"Environment", &T::Environment,
			"SessionId", &T::SessionId,
			"Firm", &T::Firm,
			"AccessKeyId", &T::AccessKeyId,
			"SecretKey", &T::SecretKey,
			"TradingSystemName", &T::TradingSystemName,
			"TradingSystemVersion", &T::TradingSystemVersion,
			"TradingSystemVendor", &T::TradingSystemVendor,
			"KeepAliveIntervalMs", &T::KeepAliveIntervalMs,
			"Primary", &T::Primary,
			"Backup", &T::Backup);
	};
};

} // namespace ILink3
