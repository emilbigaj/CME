#include "ILink3Sbe.hpp"
#include <iostream>

int main()
{
	ILink3::NewOrderSingle newOrderSingle{};
	newOrderSingle.Price.Mantissa = 100'000'000'000;   // PRICE9 (x10^-9) -> 100.0
	newOrderSingle.OrderQty = 1;
	newOrderSingle.SecurityID = 894923;
	newOrderSingle.Side = ILink3::SideReq::Buy;
	newOrderSingle.SenderID = "Cucumber";
	newOrderSingle.ClOrdID = "YZ734";

	std::cout << newOrderSingle.ToString() << std::endl;
	return 0;
}
