#pragma once

// Reads CME's market-data channel configuration file, which lists every channel (a product
// grouping, e.g. "E-mini S&P 500 futures"), and for each channel the feeds it publishes: the
// two redundant incremental feeds (A and B), the snapshot and instrument-replay feeds used for
// recovery and late joins, and a TCP replay service. Each feed entry carries the multicast
// group (or host) address and port to join.
//
// The file is published per environment on CME's public FTP under
// SBEFix/<Environment>/Configuration/config.xml and refreshed daily. It is read once at
// start-up (a cold path), so the parser here is a small targeted scan of the exact shape CME
// publishes — not a general XML reader.

#include <cstdint>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace Mdp3
{

// One feed of a channel: what kind it is, which of the redundant pair it is, and where to
// listen. The type codes are CME's own: I incremental, S snapshot, N instrument replay,
// H historical replay (TCP), SMBO/NMBO for the order-book feeds we do not use yet.
struct Connection
{
	std::string Id;         // e.g. "310IA"
	std::string TypeCode;   // "I", "S", "N", "H", "SMBO", ...
	std::string TypeName;   // e.g. "Incremental"
	char Feed = 'A';        // 'A' or 'B'
	std::string Protocol;   // "UDP/IP" or "TCP/IP"
	std::string GroupIp;    // multicast group to join (empty for the TCP services)
	std::string HostIp;     // the publishing host (the TCP services' address)
	uint16_t Port = 0;

	bool IsUdp() const { return Protocol == "UDP/IP"; }
};

// One market-data channel: its identity, the product codes it carries, and its feeds.
struct Channel
{
	int32_t Id = 0;
	std::string Label;
	std::vector<std::string> Products;      // product codes, e.g. "ES"
	std::vector<Connection> Connections;

	// Find a feed by type code and side, or null if the channel has none.
	const Connection* Find(const std::string& typeCode, char feed) const
	{
		for (const Connection& connection : Connections)
			if (connection.TypeCode == typeCode && connection.Feed == feed)
				return &connection;
		return nullptr;
	}
};

class ChannelConfig
{
	std::vector<Channel> _channels;

public:
	// Read and parse the whole file; throws if it cannot be opened.
	static ChannelConfig Load(const std::filesystem::path& filePath)
	{
		// Step 1: Read the file whole — it is half a megabyte, parsed once.
		std::ifstream stream(filePath);
		if (!stream.is_open())
			throw std::runtime_error("ChannelConfig::Load: cannot open " + filePath.string());
		std::stringstream buffer;
		buffer << stream.rdbuf();
		const std::string xml = buffer.str();

		// Step 2: Walk the <channel ...> ... </channel> blocks.
		ChannelConfig config;
		size_t at = 0;
		while (true)
		{
			const size_t begin = xml.find("<channel ", at);
			if (begin == std::string::npos)
				break;
			const size_t end = xml.find("</channel>", begin);
			if (end == std::string::npos)
				break;
			config._channels.push_back(ParseChannel(std::string_view(xml).substr(begin, end - begin)));
			at = end + 10;
		}
		return config;
	}

	const std::vector<Channel>& Channels() const { return _channels; }

	// Find a channel by its id, or null if the file has none.
	const Channel* FindChannel(int32_t channelId) const
	{
		for (const Channel& channel : _channels)
			if (channel.Id == channelId)
				return &channel;
		return nullptr;
	}

private:
	// Parse one channel block: its attributes, product codes, and connection entries.
	static Channel ParseChannel(std::string_view block)
	{
		// Step 1: The channel's own attributes.
		Channel channel;
		channel.Id = static_cast<int32_t>(ParseInt(Attribute(block, "id")));
		channel.Label = std::string(Attribute(block, "label"));

		// Step 2: Every product code.
		size_t at = 0;
		while (true)
		{
			const size_t product = block.find("<product ", at);
			if (product == std::string::npos)
				break;
			channel.Products.emplace_back(Attribute(block.substr(product), "code"));
			at = product + 9;
		}

		// Step 3: Every connection entry.
		at = 0;
		while (true)
		{
			const size_t begin = block.find("<connection ", at);
			if (begin == std::string::npos)
				break;
			const size_t end = block.find("</connection>", begin);
			if (end == std::string::npos)
				break;
			channel.Connections.push_back(ParseConnection(block.substr(begin, end - begin)));
			at = end + 13;
		}
		return channel;
	}

	// Parse one connection entry from its child elements.
	static Connection ParseConnection(std::string_view block)
	{
		Connection connection;
		connection.Id = std::string(Attribute(block, "id"));
		connection.TypeCode = std::string(Attribute(block, "feed-type"));
		connection.TypeName = std::string(ElementText(block, "type"));
		connection.GroupIp = std::string(ElementText(block, "ip"));
		connection.HostIp = std::string(ElementText(block, "host-ip"));
		connection.Port = static_cast<uint16_t>(ParseInt(ElementText(block, "port")));
		connection.Protocol = std::string(ElementText(block, "protocol"));
		const std::string_view feed = ElementText(block, "feed");
		connection.Feed = feed.empty() ? 'A' : feed[0];
		return connection;
	}

	// The value of `name="value"` within the block, or empty if absent.
	static std::string_view Attribute(std::string_view block, std::string_view name)
	{
		const std::string key = std::string(name) + "=\"";
		const size_t begin = block.find(key);
		if (begin == std::string_view::npos)
			return {};
		const size_t valueBegin = begin + key.size();
		const size_t valueEnd = block.find('"', valueBegin);
		return valueEnd == std::string_view::npos ? std::string_view{} : block.substr(valueBegin, valueEnd - valueBegin);
	}

	// The text of the first `<name ...>text</name>` element within the block, or empty.
	static std::string_view ElementText(std::string_view block, std::string_view name)
	{
		const std::string open = "<" + std::string(name);
		const std::string close = "</" + std::string(name) + ">";
		const size_t begin = block.find(open);
		if (begin == std::string_view::npos)
			return {};
		const size_t textBegin = block.find('>', begin);
		const size_t textEnd = block.find(close, begin);
		if (textBegin == std::string_view::npos || textEnd == std::string_view::npos || textBegin + 1 > textEnd)
			return {};
		return block.substr(textBegin + 1, textEnd - textBegin - 1);
	}

	static int64_t ParseInt(std::string_view s)
	{
		int64_t value = 0;
		std::from_chars(s.data(), s.data() + s.size(), value);
		return value;
	}
};

} // namespace Mdp3
