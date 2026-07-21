// Exercises the kernel-bypass connection against a real wire: connect to the given address,
// say exactly how far it got (established, refused, or timed out), and close. Even a refusal
// is a full-path proof — the connection request left through the Solarflare port and the
// answer came back through it, all in user space. The Solarflare interface comes from the
// ZF_ATTR environment variable, exactly as it will in production.
//
//   ZF_ATTR="interface=p513p1" ZfTcp <ip> <port> [connectTimeoutMs=2000]

#include "ZfConnection.hpp"

#include <cstdint>
#include <iostream>
#include <string>

int main(int argc, char** argv)
{
	if (argc < 3)
	{
		std::cerr << "usage: ZF_ATTR=\"interface=<port>\" ZfTcp <ip> <port> [connectTimeoutMs=2000]\n";
		return 2;
	}
	const std::string ip = argv[1];
	const uint16_t port = static_cast<uint16_t>(std::stoi(argv[2]));
	const int connectTimeoutMs = (argc > 3) ? std::stoi(argv[3]) : 2000;

	try
	{
		// Step 1: Attempt the connection; success or the failure text tells the story.
		ILink3::ZfConnection connection;
		connection.Connect(ip, port, /*recvTimeoutSeconds*/ 1, connectTimeoutMs);
		std::cout << "ESTABLISHED to " << ip << ":" << port << " over kernel bypass.\n";

		// Step 2: Close politely; nothing was sent.
		connection.Close();
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cout << "Connect outcome: " << error.what() << "\n";
		return 1;
	}
}
