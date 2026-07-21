// Proves the kernel-bypass runtime works on this machine: initialize the TCPDirect library,
// then allocate a stack on the named network interface — the step that requires a Solarflare
// port and the activated license. Each step reports pass or the exact failure, so a missing
// license, a wrong interface, or an unloaded driver is named instead of guessed at. No
// network traffic is generated.
//
//   ZfProbe [interface=p513p1]

#include <zf/zf.h>

#include <cstring>
#include <iostream>
#include <string>

int main(int argc, char** argv)
{
	const std::string interfaceName = (argc > 1) ? argv[1] : "p513p1";

	// Step 1: Initialize the library.
	int rc = zf_init();
	if (rc != 0)
	{
		std::cerr << "zf_init failed: " << std::strerror(-rc) << "\n";
		return 1;
	}
	std::cout << "zf_init ok\n";

	// Step 2: Default attributes, then aim them at the requested interface.
	zf_attr* attributes = nullptr;
	rc = zf_attr_alloc(&attributes);
	if (rc != 0)
	{
		std::cerr << "zf_attr_alloc failed: " << std::strerror(-rc) << "\n";
		return 1;
	}
	rc = zf_attr_set_str(attributes, "interface", interfaceName.c_str());
	if (rc != 0)
	{
		std::cerr << "zf_attr_set_str(interface=" << interfaceName << ") failed: " << std::strerror(-rc) << "\n";
		return 1;
	}
	std::cout << "attributes ok (interface=" << interfaceName << ")\n";

	// Step 3: The real test — a stack on that interface needs the Solarflare port and license.
	zf_stack* stack = nullptr;
	rc = zf_stack_alloc(attributes, &stack);
	if (rc != 0)
	{
		std::cerr << "zf_stack_alloc failed: " << std::strerror(-rc)
		          << "  (license missing, wrong interface, or driver not loaded?)\n";
		return 1;
	}
	std::cout << "zf_stack_alloc ok — kernel bypass is live on " << interfaceName << "\n";

	// Step 4: Tidy up.
	zf_stack_free(stack);
	zf_attr_free(attributes);
	zf_deinit();
	return 0;
}
