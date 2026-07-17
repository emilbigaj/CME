// Offline: encode a PartyDetailsDefinitionRequest with the given config and hexdump the exact
// wire bytes, annotating the SOFH, headers, root block, and each repeating group so the group
// framing can be checked by eye against the schema. No network.

#include "ILink3Config.hpp"
#include "ILink3Sbe.hpp"
#include "Wire.hpp"

#include <cstdint>
#include <cstdio>
#include <iostream>
#include <span>
#include <vector>

static void HexRange(const uint8_t* p, size_t n, const char* label)
{
	std::printf("%-28s [%3zu bytes]  ", label, n);
	for (size_t i = 0; i < n; ++i)
		std::printf("%02x ", p[i]);
	std::printf("\n");
}

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		std::cerr << "usage: DumpPartyDetails <settings.json>\n";
		return 2;
	}
	ILink3::ILink3Config config = ILink3::ILink3Config::Load(argv[1]);

	std::array<uint8_t, 512> buf{};
	size_t len = ILink3::EncodePartyDetailsDefinitionRequest(config.Parties, /*reqId*/ 1808566498ULL,
		/*seqNo*/ 1, /*sendingTime*/ 1784209733809641728ULL, buf);

	std::printf("Total framed length: %zu bytes\n\n", len);

	// SOFH(4) + MessageHeader(8) + root(147) + groups...
	const uint8_t* p = buf.data();
	HexRange(p + 0, 4, "SOFH");
	HexRange(p + 4, 8, "MessageHeader");
	const size_t rootOff = 12;
	HexRange(p + rootOff, ILink3::PartyDetailsDefinitionRequest::BlockLength, "root block (147)");

	size_t g = rootOff + ILink3::PartyDetailsDefinitionRequest::BlockLength;
	// NoPartyDetails dimension header {blockLength:u16, numInGroup:u8}
	uint16_t bl1 = static_cast<uint16_t>(p[g] | (p[g + 1] << 8));
	uint8_t n1 = p[g + 2];
	std::printf("\nNoPartyDetails header: blockLength=%u numInGroup=%u\n", bl1, n1);
	HexRange(p + g, 3, "  dim header");
	g += 3;
	for (uint8_t i = 0; i < n1; ++i)
	{
		HexRange(p + g, bl1, "  entry");
		g += bl1;
	}

	uint16_t bl2 = static_cast<uint16_t>(p[g] | (p[g + 1] << 8));
	uint8_t n2 = p[g + 2];
	std::printf("\nNoTrdRegPublications header: blockLength=%u numInGroup=%u\n", bl2, n2);
	HexRange(p + g, 3, "  dim header");
	g += 3;
	for (uint8_t i = 0; i < n2; ++i)
	{
		HexRange(p + g, bl2, "  entry");
		g += bl2;
	}

	std::printf("\nParsed end at %zu (framed len %zu)\n", g, len);
	return 0;
}
