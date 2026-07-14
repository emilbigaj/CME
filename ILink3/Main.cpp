#include "Timestamp.hpp"
#include "ILink3Sbe.hpp"
#include <openssl/hmac.h>
#include <iostream>

// Smoke test: the generated SBE header compiles (all 56 sizeof==blockLength static_asserts
// fire at compile time), glaze ToString works, and OpenSSL HMAC is linked.
int main()
{
	std::cout << "ILink3Tests up at " << Tools::Timestamp::UtcNow().ToString() << "\n";
	std::cout << "SBE schema id " << ILink3::SchemaId << " version " << ILink3::SchemaVersion << "\n";

	// A NewOrderSingle we could cast straight onto a TX buffer.
	ILink3::NewOrderSingle nos{};
	nos.OrderQty = 5;
	nos.SecurityID = 894923;
	nos.Side = ILink3::SideReq::Buy;
	nos.Price.Mantissa = 100'000'000'000;   // PRICE9: ×10^-9  -> 100.0
	nos.ClOrdID = "YZ734";
	std::cout << "NewOrderSingle (template " << ILink3::NewOrderSingle::TemplateId
	          << ", block " << ILink3::NewOrderSingle::BlockLength << "):\n"
	          << nos.ToString() << "\n";
	std::cout << "Price.Value() = " << nos.Price.Value() << "\n";

	// Sizes match the wire block lengths (also enforced at compile time).
	std::cout << "sizeof(Negotiate)=" << sizeof(ILink3::Negotiate)
	          << " sizeof(Establish)=" << sizeof(ILink3::Establish)
	          << " sizeof(NewOrderSingle)=" << sizeof(ILink3::NewOrderSingle) << "\n";

	// Template dispatch enum is generated too.
	std::cout << "Template::NewOrderSingle = "
	          << static_cast<int>(ILink3::Template::NewOrderSingle) << "\n";

	// HMAC-SHA256 available for FIXP Negotiate/Establish signing.
	unsigned char digest[32];
	unsigned int len = 0;
	HMAC(EVP_sha256(), "secret", 6, reinterpret_cast<const unsigned char*>("canonical"), 9, digest, &len);
	std::cout << "HMAC-SHA256 ok, digest length " << len << "\n";
	return 0;
}
