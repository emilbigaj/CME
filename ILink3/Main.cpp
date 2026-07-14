#include "Timestamp.hpp"
#include "Bitset.hpp"
#include "Order.hpp"
#include <openssl/hmac.h>
#include <iostream>

// Smoke test: proves the CME project links the full HFT stack + OpenSSL.
int main()
{
	Tools::Timestamp now = Tools::Timestamp::UtcNow();
	std::cout << "ILink3Tests up at " << now.ToString() << std::endl;

	Execution::OrderTarget orderTarget{};
	orderTarget.OrderHeader.InstrumentId = 42;
	std::cout << "OrderTarget: " << orderTarget.ToString() << std::endl;

	// HMAC-SHA256 available (FIXP Negotiate/Establish signatures).
	unsigned char digest[32];
	unsigned int digestLength = 0;
	const char* key = "secret";
	const char* data = "canonical";
	HMAC(EVP_sha256(), key, 6, reinterpret_cast<const unsigned char*>(data), 9, digest, &digestLength);
	std::cout << "HMAC-SHA256 ok, digest length " << digestLength << std::endl;

	return 0;
}