// Known-answer tests for the logon signer. Each check feeds a fixed input and compares the
// result against a value published by an independent authority, so any change that would
// break how we sign a logon is caught here — offline, before a live connection. It covers
// the signing algorithm, the key decoding, and the exact strings CME's specification signs.
// Exit code zero means every value matched.

#include "Hmac.hpp"
#include <cstdio>
#include <string>
#include <vector>

static int32_t s_failures = 0;

// Render bytes as lowercase hex so a computed value can be compared to a published one.
static std::string ToHex(std::span<const uint8_t> bytes)
{
	static const char* digits = "0123456789abcdef";
	std::string out;
	out.reserve(bytes.size() * 2);
	for (uint8_t b : bytes)
	{
		out += digits[b >> 4];
		out += digits[b & 0x0F];
	}
	return out;
}

// Record one check: print a pass/fail line and count the failures.
static void Check(bool condition, const std::string& what)
{
	if (!condition)
	{
		s_failures++;
		std::printf("FAIL  %s\n", what.c_str());
	}
	else
	{
		std::printf("ok    %s\n", what.c_str());
	}
}

int main()
{
	using ILink3::Hmac;

	// Step 1: The signing algorithm against its official published test vectors (from the
	// internet standard numbered 4231).
	{
		std::vector<uint8_t> key(20, 0x0b);
		Check(ToHex(Hmac::HmacSha256(key, "Hi There")) ==
			"b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", "signing vector 1");
	}
	{
		std::vector<uint8_t> key = {'J', 'e', 'f', 'e'};
		Check(ToHex(Hmac::HmacSha256(key, "what do ya want for nothing?")) ==
			"5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", "signing vector 2");
	}
	{
		std::vector<uint8_t> key(20, 0xaa);
		std::string data(50, static_cast<char>(0xdd));
		Check(ToHex(Hmac::HmacSha256(key, data)) ==
			"773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe", "signing vector 3");
	}
	{
		std::vector<uint8_t> key(131, 0xaa);   // a key longer than the hash's internal block
		Check(ToHex(Hmac::HmacSha256(key, "Test Using Larger Than Block-Size Key - Hash Key First")) ==
			"60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54", "signing vector 6");
	}

	// Step 2: Key decoding against the encoding standard's published vectors (numbered 4648),
	// covering padded and unpadded forms, the url-safe characters, and rejection of the two
	// standard-base64 symbols we must not accept.
	{
		auto decodesTo = [](std::string_view in, std::string_view expected)
		{
			std::vector<uint8_t> bytes = Hmac::Base64UrlDecode(in);
			return std::string(bytes.begin(), bytes.end()) == expected;
		};
		Check(decodesTo("", ""), "decode empty");
		Check(decodesTo("Zg", "f"), "decode Zg (unpadded)");
		Check(decodesTo("Zg==", "f"), "decode Zg== (padded)");
		Check(decodesTo("Zm8", "fo"), "decode Zm8");
		Check(decodesTo("Zm9v", "foo"), "decode Zm9v");
		Check(decodesTo("Zm9vYg", "foob"), "decode Zm9vYg");
		Check(decodesTo("Zm9vYmE", "fooba"), "decode Zm9vYmE");
		Check(decodesTo("Zm9vYmFy", "foobar"), "decode Zm9vYmFy");
		Check(decodesTo("TXkgU2VjcmV0IEtleQ", "My Secret Key"), "decode CME docs sample");

		// The url-safe symbols '-' and '_' must decode; here the bytes 0xFB 0xFF.
		std::vector<uint8_t> urlSafe = Hmac::Base64UrlDecode("-_8");
		Check(urlSafe.size() == 2 && urlSafe[0] == 0xFB && urlSafe[1] == 0xFF, "decode url-safe -_");

		// The standard-base64 symbols '+' and '/' must be rejected.
		bool threw = false;
		try { Hmac::Base64UrlDecode("a+b/"); }
		catch (const std::invalid_argument&) { threw = true; }
		Check(threw, "reject + and /");
	}

	// Step 3: The two canonical strings, against the worked sample values in CME's spec.
	{
		Check(Hmac::BuildNegotiateCanonicalMessage(1563720650008ULL, 1563720660068ULL, "ABC", "007") ==
			"1563720650008\n1563720660068\nABC\n007", "canonical Negotiate");

		ILink3::TradingSystemConfig tradingSystem;
		tradingSystem.Name = std::string("CelerityMarkets");
		tradingSystem.Version = std::string("1.0");
		tradingSystem.Vendor = std::string("Internal");
		Check(Hmac::BuildEstablishCanonicalMessage(1563720650008ULL, 1563720660068ULL, "ABC", "007",
			tradingSystem, 1, 30000) ==
			"1563720650008\n1563720660068\nABC\n007\nCelerityMarkets\n1.0\nInternal\n1\n30000", "canonical Establish");
	}

	// Step 4: End to end — signing the same input twice gives the same 32-byte result.
	{
		std::string canonical = Hmac::BuildNegotiateCanonicalMessage(1563720650008ULL, 1563720660068ULL, "ABC", "007");
		std::array<uint8_t, 32> a = Hmac::Sign("TXkgU2VjcmV0IEtleQ", canonical);
		std::array<uint8_t, 32> b = Hmac::Sign("TXkgU2VjcmV0IEtleQ", canonical);
		Check(a == b && a.size() == 32, "Sign() gives a stable 32-byte result");
	}

	// Step 5: Report the overall result.
	if (s_failures == 0)
	{
		std::printf("ALL HMAC TESTS PASSED\n");
		return 0;
	}
	std::printf("%d FAILURES\n", s_failures);
	return 1;
}
