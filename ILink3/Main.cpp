// A quick smoke test that the iLink 3 library is wired together: the generated wire structs
// compile (their compile-time size checks all pass), a message serializes to readable text,
// and the signing library is linked in. It builds nothing real — it just proves the pieces
// are present after a fresh build.

#include "Timestamp.hpp"
#include "ILink3Sbe.hpp"
#include <openssl/hmac.h>
#include <iostream>

// Print a few facts about the generated structs and confirm the signing library links.
int main()
{
	// Step 1: Show the build is alive and which schema version was generated.
	std::cout << "ILink3Tests up at " << Tools::Timestamp::UtcNow().ToString() << "\n";
	std::cout << "schema id " << ILink3::SchemaId << " version " << ILink3::SchemaVersion << "\n";

	// Step 2: Fill in a new-order message and serialize it, proving the struct and its
	// text mapping work. The price is scaled by ten-to-the-minus-nine, so this is 100.0.
	ILink3::NewOrderSingle newOrder{};
	newOrder.OrderQty = 5;
	newOrder.SecurityID = 894923;
	newOrder.Side = ILink3::SideReq::Buy;
	newOrder.Price.Mantissa = 100'000'000'000;
	newOrder.ClOrdID = "YZ734";
	std::cout << "NewOrderSingle (template " << ILink3::NewOrderSingle::TemplateId
	          << ", block " << ILink3::NewOrderSingle::BlockLength << "):\n"
	          << newOrder.ToString() << "\n";
	std::cout << "Price.Value() = " << newOrder.Price.Value() << "\n";

	// Step 3: Confirm the struct sizes equal the on-wire block lengths (also checked at
	// compile time, but printed here as a sanity glance).
	std::cout << "sizeof(Negotiate)=" << sizeof(ILink3::Negotiate)
	          << " sizeof(Establish)=" << sizeof(ILink3::Establish)
	          << " sizeof(NewOrderSingle)=" << sizeof(ILink3::NewOrderSingle) << "\n";

	// Step 4: The template-to-number lookup used to route received messages is generated too.
	std::cout << "Template::NewOrderSingle = "
	          << static_cast<int>(ILink3::Template::NewOrderSingle) << "\n";

	// Step 5: Prove the signing library is linked by computing a throwaway signature.
	unsigned char digest[32];
	unsigned int digestLength = 0;
	HMAC(EVP_sha256(), "secret", 6, reinterpret_cast<const unsigned char*>("canonical"), 9, digest, &digestLength);
	std::cout << "signing library linked, digest length " << digestLength << "\n";
	return 0;
}
