#pragma once

// Signs the two iLink 3 logon messages (Negotiate and Establish) so the exchange accepts
// the connection. CME hands each customer a secret key; we prove we hold it by attaching a
// signature computed from the message contents and that key. A wrong signature is the most
// common reason a first connection is rejected, so this file is small, self-contained, and
// exercised by known-answer tests before it ever touches the network.
//
// The recipe, straight from CME's session-layer specification:
//   Step 1: Build the "canonical message" — the signed field values joined by newline
//           characters, in a fixed order, with no trailing newline.
//              Negotiate : RequestTimestamp, UUID, Session, Firm
//              Establish : the same four, then TradingSystemName, TradingSystemVersion,
//                          TradingSystemVendor, NextSeqNo, KeepAliveInterval
//   Step 2: Decode the secret key. CME delivers it in the url-safe base64 alphabet, so it
//           must be decoded back to raw bytes before use.
//   Step 3: Sign — the signature is a hashed authentication code (the SHA-256 variant) of
//           the canonical message under the decoded key, a raw 32-byte value that goes
//           into the message's signature field.

#include "ILink3Config.hpp"   // TradingSystemConfig

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ILink3
{

// Collection of pure signing helpers — no state, so every method is static. Together they
// turn a set of logon field values plus the account's secret key into the 32-byte signature
// the exchange checks. Split out from the message encoder so it can be tested in isolation.
class Hmac
{
public:
	static constexpr size_t SignatureLength = 32;

	// Decode the url-safe base64 text CME uses for the secret key back into raw bytes.
	static std::vector<uint8_t> Base64UrlDecode(std::string_view text)
	{
		// Step 1: Drop any trailing '=' padding — it carries no data and is optional here.
		while (!text.empty() && text.back() == '=')
			text.remove_suffix(1);

		// Step 2: Reject a length that can never be valid (one leftover character).
		if (text.size() % 4 == 1)
			throw std::invalid_argument("Hmac::Base64UrlDecode: invalid input length");

		std::vector<uint8_t> out;
		out.reserve(text.size() * 3 / 4 + 1);

		// Step 3: Walk the text, shifting each character's 6 bits into an accumulator and
		// emitting a byte whenever at least 8 bits have piled up.
		uint32_t accumulator = 0;
		int32_t bits = 0;
		for (char c : text)
		{
			accumulator = (accumulator << 6) | static_cast<uint32_t>(DecodeChar(c));
			bits += 6;
			if (bits >= 8)
			{
				bits -= 8;
				out.push_back(static_cast<uint8_t>((accumulator >> bits) & 0xFF));
			}
		}
		return out;
	}

	// Compute the 32-byte SHA-256 authentication code of `data` under `key`.
	static std::array<uint8_t, SignatureLength> HmacSha256(std::span<const uint8_t> key, std::string_view data)
	{
		std::array<uint8_t, SignatureLength> digest{};
		unsigned int digestLength = 0;

		// Step 1: Hand key and data to the crypto library, which fills `digest`.
		const unsigned char* result = HMAC(
			EVP_sha256(),
			key.data(), static_cast<int>(key.size()),
			reinterpret_cast<const unsigned char*>(data.data()), data.size(),
			digest.data(), &digestLength);

		// Step 2: Guard against a failed call or an unexpected digest size.
		if (result == nullptr || digestLength != SignatureLength)
			throw std::runtime_error("Hmac::HmacSha256: signing failed");

		return digest;
	}

	// One-call signer: decode the account secret, then sign the canonical message with it.
	static std::array<uint8_t, SignatureLength> Sign(std::string_view secretKeyBase64Url, std::string_view canonicalMessage)
	{
		// Step 1: Turn the encoded secret back into the raw key bytes.
		std::vector<uint8_t> key = Base64UrlDecode(secretKeyBase64Url);

		// Step 2: A missing key would silently produce a wrong signature — fail loudly instead.
		if (key.empty())
			throw std::invalid_argument("Hmac::Sign: empty secret key");

		// Step 3: Produce the signature.
		return HmacSha256(key, canonicalMessage);
	}

	// Assemble the four-line string signed for the Negotiate logon message.
	static std::string BuildNegotiateCanonicalMessage(uint64_t requestTimestamp, uint64_t uuid, const std::string& session, const std::string& firm)
	{
		// Step 1: Reserve enough room for the joined values.
		std::string out;
		out.reserve(64);

		// Step 2: Append the four values, each on its own line, with no trailing newline.
		out += std::to_string(requestTimestamp);
		out += '\n';
		out += std::to_string(uuid);
		out += '\n';
		out += session;
		out += '\n';
		out += firm;
		return out;
	}

	// Assemble the nine-line string signed for the Establish logon message.
	static std::string BuildEstablishCanonicalMessage(uint64_t requestTimestamp, uint64_t uuid, const std::string& session, const std::string& firm,
	                                                  const TradingSystemConfig& tradingSystem, uint32_t nextSeqNo, uint16_t keepAliveInterval)
	{
		// Step 1: Reserve room for the longer nine-value string.
		std::string out;
		out.reserve(128);

		// Step 2: The same four values Negotiate signs, in the same order...
		out += std::to_string(requestTimestamp);
		out += '\n';
		out += std::to_string(uuid);
		out += '\n';
		out += session;
		out += '\n';
		out += firm;
		out += '\n';

		// Step 3: ...then the trading-system identity and the two session parameters.
		out += tradingSystem.Name.ToString();
		out += '\n';
		out += tradingSystem.Version.ToString();
		out += '\n';
		out += tradingSystem.Vendor.ToString();
		out += '\n';
		out += std::to_string(nextSeqNo);
		out += '\n';
		out += std::to_string(keepAliveInterval);
		return out;
	}

private:
	// Map one url-safe base64 character to its 6-bit value; throws on anything unexpected.
	static int32_t DecodeChar(char c)
	{
		// Step 1: Letters, digits, then the two url-safe symbols each map to a fixed range.
		if (c >= 'A' && c <= 'Z')
			return c - 'A';
		if (c >= 'a' && c <= 'z')
			return c - 'a' + 26;
		if (c >= '0' && c <= '9')
			return c - '0' + 52;
		if (c == '-')
			return 62;
		if (c == '_')
			return 63;

		// Step 2: The two standard-base64 symbols ('+' and '/') and anything else are
		// rejected — accepting them would decode to a wrong key and an unexplained rejection.
		throw std::invalid_argument(std::string("Hmac::Base64UrlDecode: invalid character '") + c + "'");
	}
};

} // namespace ILink3
