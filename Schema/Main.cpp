// Tiny demo of the generated iLink 3 structs: build one new-order message and print it as
// text. Useful as a first look at how a wire message maps to readable fields.

#include "ILink3Sbe.hpp"
#include <iostream>

// Fill in a new-order message and print its text form.
int main()
{
	// Step 1: Set the order fields. The price is scaled by ten-to-the-minus-nine, so the
	// value below is 100.0.
	ILink3::NewOrderSingle newOrderSingle{};
	newOrderSingle.Price.Mantissa = 100'000'000'000;
	newOrderSingle.OrderQty = 1;
	newOrderSingle.SecurityID = 894923;
	newOrderSingle.Side = ILink3::SideReq::Buy;
	newOrderSingle.SenderID = "Cucumber";
	newOrderSingle.ClOrdID = "YZ734";

	// Step 2: Print the message as text.
	std::cout << newOrderSingle.ToString() << std::endl;
	return 0;
}
