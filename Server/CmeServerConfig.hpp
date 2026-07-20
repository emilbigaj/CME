#pragma once

// The CME server's per-environment settings: its shared-memory name, the files it loads (the
// order-entry settings, the instrument universe, the market-data channel configuration, and
// the machine's market-segment layout), and the interface market data arrives on. There is
// one settings file per environment at /mnt/S/CME/<Environment>/Config/CmeServer.json; the segment
// layout it references is machine-wide and shared by every environment.

#include "Json.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace Server
{

struct CmeServerConfig
{
	std::string ServerName;               // the shared-memory identity strategies connect to
	std::string ILink3ConfigPath;         // order-entry settings (credentials, gateways, parties)
	std::string SecDefPath;               // the security-definition file (the instrument universe)
	std::string ChannelConfigPath;        // the market-data channel configuration file
	std::string MarketSegmentsPath;       // the machine's market-segment layout (shared by environments)
	std::string MarketDataInterfaceIp;    // the interface the multicast feeds are joined on
	bool StubLoggingServer = false;       // testing only: accept our own logging connections

	// Check every required field is present; throws with the exact reason if not.
	void Validate() const
	{
		if (ServerName.empty())
			throw std::invalid_argument("CmeServerConfig: ServerName is required");
		if (ILink3ConfigPath.empty() || SecDefPath.empty() || ChannelConfigPath.empty() || MarketSegmentsPath.empty())
			throw std::invalid_argument("CmeServerConfig: ILink3ConfigPath, SecDefPath, ChannelConfigPath, and MarketSegmentsPath are required");
		if (MarketDataInterfaceIp.empty())
			throw std::invalid_argument("CmeServerConfig: MarketDataInterfaceIp is required");
	}

	// Read a settings file, check it, and return the config; throws on a bad file.
	static CmeServerConfig Load(const std::filesystem::path& filePath)
	{
		// Step 1: Open and read the file whole.
		std::ifstream stream(filePath);
		if (!stream.is_open())
			throw std::runtime_error("CmeServerConfig::Load: cannot open " + filePath.string());
		std::stringstream buffer;
		buffer << stream.rdbuf();

		// Step 2: Parse and validate before handing it back.
		CmeServerConfig config = Tools::Json::Deserialize<CmeServerConfig>(buffer.str());
		config.Validate();
		return config;
	}

	std::string ToString() const
	{
		return Tools::Json::Serialize(*this);
	}

	struct glaze
	{
		using T = CmeServerConfig;
		static constexpr auto value = glz::object(
			"ServerName", &T::ServerName,
			"ILink3ConfigPath", &T::ILink3ConfigPath,
			"SecDefPath", &T::SecDefPath,
			"ChannelConfigPath", &T::ChannelConfigPath,
			"MarketSegmentsPath", &T::MarketSegmentsPath,
			"MarketDataInterfaceIp", &T::MarketDataInterfaceIp,
			"StubLoggingServer", &T::StubLoggingServer);
	};
};

} // namespace Server
