// The CME trading server process. Everything is settings-driven: one file names the
// environment's pieces (credentials, instruments, market-data channels), the machine's
// market-segment layout, and the order-entry transport, and the process brings them up in
// order — settings, the trading server with its full instrument catalog, the alert wiring,
// then the venue sessions and the pinned per-segment threads. It then runs until asked to
// stop. The transport choice happens exactly once, here: the whole stack beneath it is
// stamped out statically for kernel sockets or for kernel bypass, with no indirection on
// the hot path.
//
//   CmeServer <CmeServer.json>

#include "CmeServer.hpp"
#include "ZfConnection.hpp"
#include "AlertManager.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <thread>

// Ctrl+C / termination ask the main loop to stop; shutdown itself happens on the main thread.
static std::atomic<bool> s_stopRequested{false};
static void OnStopSignal(int)
{
	s_stopRequested.store(true);
}

// Bring one server flavor up and run it until asked to stop: the trading server itself, the
// alert wiring, the venue sessions, and the pinned per-segment threads.
template <typename ServerType>
static int Run(const Server::CmeServerConfig& config, const Server::MarketSegments& marketSegments,
               const ILink3::ILink3Config& ilink3Config, const SecDef::SecDefFile& secdef,
               const Mdp3::ChannelConfig& channels)
{
	// Step 1: The trading server: the shared-memory hub strategies connect to, the whole
	// instrument universe in its catalog, and one gateway + market-data channel per segment.
	ServerType server(config, marketSegments, ilink3Config, secdef, channels);

	// Step 2: Alerts: service-thread exceptions and order rejections flow to the alert
	// manager, which fans them out to the operators.
	Provider::AlertManager alertManager(server.Server().Context());
	server.Exception = [&alertManager](const std::exception& error)
	{
		alertManager.OnException(error);
	};
	server.Server().OrderRejected = [&alertManager](const Execution::OrderRejected& rejected, const std::string& message)
	{
		alertManager.OnOrderRejected(rejected, message);
	};

	// Step 3: Open the order sessions, join the market-data feeds, and start the pinned
	// per-segment threads.
	server.Start();
	std::cout << "CmeServer running (" << config.Transport << " transport). Ctrl+C stops." << std::endl;

	// Step 4: Run until asked to stop, then shut down cleanly.
	std::signal(SIGINT, OnStopSignal);
	std::signal(SIGTERM, OnStopSignal);
	while (!s_stopRequested.load(std::memory_order_relaxed))
		std::this_thread::sleep_for(std::chrono::milliseconds(100));

	std::cout << "CmeServer stopping..." << std::endl;
	server.Stop();
	return 0;
}

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		std::cerr << "usage: CmeServer <CmeServer.json>\n";
		return 2;
	}

	// The stub outlives the try so a failed start still stops its listen thread — a joinable
	// thread in a destructor otherwise terminates the process during unwind.
	std::unique_ptr<Socket::ServerSocket> loggingStub;
	try
	{
		// Step 1: Settings — the server's own file, then everything it references.
		Server::CmeServerConfig config = Server::CmeServerConfig::Load(argv[1]);
		std::cout << "CmeServer settings: " << config.ToString() << "\n";
		Server::MarketSegments marketSegments = Server::MarketSegments::Load(config.MarketSegmentsPath);
		ILink3::ILink3Config ilink3Config = ILink3::ILink3Config::Load(config.ILink3ConfigPath);
		SecDef::SecDefFile secdef = SecDef::SecDefFile::Load(config.SecDefPath);
		Mdp3::ChannelConfig channels = Mdp3::ChannelConfig::Load(config.ChannelConfigPath);

		// Step 2: Testing only — a stub that accepts this server's logging connections when no
		// logging server is running beside it.
		if (config.StubLoggingServer)
		{
			const std::filesystem::path serverDirectory = Provider::ServerContext::GetDirectoryPath(config.ServerName);
			loggingStub = std::make_unique<Socket::ServerSocket>(
				Provider::Context::GetLoggingServerDirectoryPath(serverDirectory).string(), 8);
			loggingStub->Listen();
		}

		// Step 3: Run the transport the settings chose.
		const int result = config.Transport == "TcpDirect"
			? Run<Server::BasicCmeServer<ILink3::ZfConnection>>(config, marketSegments, ilink3Config, secdef, channels)
			: Run<Server::CmeServer>(config, marketSegments, ilink3Config, secdef, channels);
		if (loggingStub)
			loggingStub->Dispose();
		std::cout << "CmeServer stopped." << std::endl;
		return result;
	}
	catch (const std::exception& error)
	{
		std::cerr << "CmeServer failed: " << error.what() << "\n";
		if (loggingStub)
			loggingStub->Dispose();
		return 1;
	}
}
