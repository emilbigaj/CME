#pragma once

// The CME trading server: the composition that plugs native iLink3 order entry into the HFT
// library's trading-server seam. It owns one Provider::Server (the shared-memory hub the
// strategies connect to), one market-segment gateway per served segment, and one instrument
// router per allocated instrument. Strategies never see CME — they allocate instruments and
// send order targets exactly as they do against any other venue server.
//
// Start-up: the instrument catalog is built from the security-definition file (futures of the
// configured roots on the served segments) and committed to the server, each header stamped
// with its segment's core group so the strategy, the exchange connection, and the server's
// polling for that segment all stay on one core complex. When a strategy allocates an
// instrument, the callback builds that instrument's router and wires its events back into the
// server. When a strategy sends an order target, the server's callback routes it to the
// instrument's router, which speaks iLink3 on the segment's gateway.
//
// Threads: one admin thread services connections and allocations; one thread per served
// segment owns that segment's gateway completely — it drains the strategies' order targets
// (ReadExecution) and polls the connection for execution reports, so all session state stays
// on one core. Execution reports route back to their instrument's router through the client
// order id, which carries the instrument id inside it.
//
// Calendar spreads are not committed yet: a spread's header references its legs by allocated
// instrument id, so committing them needs the leg-allocation flow (planned).

#include "ILink3Config.hpp"
#include "CmeLogger.hpp"
#include "MarketSegmentGateway.hpp"
#include "InstrumentRouter.hpp"
#include "SecDefFile.hpp"
#include "Server.hpp"             // Provider::Server (the shared-memory hub)
#include "OrderIdAllocator.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace Server
{

// One market segment this server trades: which CME gateway, and which core group (= which
// shared-memory channel and, in production, which core complex) serves it.
struct ServedSegment
{
	int32_t MarketSegmentID = 0;
	int32_t CoreGroupId = 0;
};

class CmeServer
{
	// ---- configuration (set once) ----
	ILink3::ILink3Config _config;
	const SecDef::SecDefFile& _secdef;        // owned by the caller; outlives this server
	std::vector<ServedSegment> _segments;

	// ---- the seam and the venue ----
	Provider::Server _server;
	ILink3::CmeLoggerManager _loggerManager;
	std::array<std::unique_ptr<ILink3::MarketSegmentGateway>, 8> _gateways;   // by core group id
	std::vector<const SecDef::Loaded*> _loadedByHeaderId;   // catalog order == header id
	std::array<std::unique_ptr<ILink3::InstrumentRouter>, 64> _routers;       // by instrument id

	// ---- service threads ----
	std::vector<std::thread> _threads;
	std::atomic<bool> _running{false};

public:
	CmeServer(const ILink3::ILink3Config& config, const SecDef::SecDefFile& secdef,
	          const std::vector<ServedSegment>& segments, const std::vector<std::string>& roots,
	          const std::string& serverName)
		: _config(config), _secdef(secdef), _segments(segments),
		  _server(MakeServerHeader(serverName, segments))
	{
		// Step 1: Build and commit the instrument catalog: futures of the requested roots on the
		// served segments, each stamped with its segment's core group.
		CommitCatalog(roots);

		// Step 2: When a strategy allocates an instrument, build its router.
		_server.AllocateInstrument = [this](const Provider::AllocateInstrument& allocate)
		{
			BuildRouter(allocate);
		};

		// Step 3: When a strategy sends an order target (already risk-checked by the server),
		// route it to the instrument's router.
		_server.OrderTarget = [this](const Execution::OrderTarget& target)
		{
			ILink3::InstrumentRouter* router = _routers[static_cast<size_t>(target.OrderHeader.InstrumentId)].get();
			if (router != nullptr)
				router->OnOrderTarget(target);
		};

		// Step 4: One gateway (and one log) per served segment; execution reports route back to
		// their instrument's router by the client order id they carry.
		for (const ServedSegment& segment : _segments)
		{
			ILink3::CmeLogger& logger = _loggerManager.NewLogger(
				ILink3::CmeLoggerManager::LogDirectory("/mnt/S", _config.Environment, segment.MarketSegmentID), segment.MarketSegmentID);
			auto gateway = std::make_unique<ILink3::MarketSegmentGateway>(_config, segment.MarketSegmentID, &logger);
			gateway->OnBusinessMessage = [this](const ILink3::FramedMessage& message)
			{
				DispatchExecutionReport(message);
			};
			_gateways[static_cast<size_t>(segment.CoreGroupId)] = std::move(gateway);
		}
	}

	// Stopping on destruction keeps a thrown exception from unwinding into live service threads.
	~CmeServer()
	{
		Stop();
	}

	Provider::Server& Server() { return _server; }

	// The catalog position of an instrument by its exchange id, or -1 if not committed.
	int32_t FindInstrumentHeaderId(int32_t securityId) const
	{
		for (size_t headerId = 0; headerId < _loadedByHeaderId.size(); ++headerId)
			if (_loadedByHeaderId[headerId]->SecurityID == securityId)
				return static_cast<int32_t>(headerId);
		return -1;
	}

	// Connect and log on every segment, then start the service threads.
	void Start()
	{
		// Step 1: Start the message-log drain.
		_loggerManager.Start();

		// Step 2: Open every segment's session. Logon uses the generous receive limit; the
		// serving loop then shortens it so a quiet connection hands control straight back.
		for (const ServedSegment& segment : _segments)
		{
			ILink3::MarketSegmentGateway& gateway = *_gateways[static_cast<size_t>(segment.CoreGroupId)];
			gateway.Connect();
			if (!gateway.Logon())
				throw std::runtime_error("CmeServer: logon failed on segment " + std::to_string(segment.MarketSegmentID));
			gateway.SetReceiveTimeout(1);
		}

		// Step 3: The admin thread services connections and instrument allocations.
		_running = true;
		_threads.emplace_back([this]()
		{
			while (_running.load(std::memory_order_relaxed))
			{
				try
				{
					_server.ReadAdmin();
				}
				catch (const std::exception& error)
				{
					std::cout << "CmeServer admin: " << error.what() << "\n";
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		});

		// Step 4: One thread per segment owns its gateway completely: it drains the strategies'
		// order targets and polls the connection, so all session state stays on one core. (Core
		// pinning per the segment map comes with the production transport.)
		for (const ServedSegment& segment : _segments)
		{
			const int32_t coreGroupId = segment.CoreGroupId;
			_threads.emplace_back([this, coreGroupId]()
			{
				ILink3::MarketSegmentGateway& gateway = *_gateways[static_cast<size_t>(coreGroupId)];
				while (_running.load(std::memory_order_relaxed))
				{
					try
					{
						_server.ReadExecution(coreGroupId);
						gateway.Poll();
					}
					catch (const std::exception& error)
					{
						std::cout << "CmeServer segment " << coreGroupId << ": " << error.what() << "\n";
					}
				}
			});
		}
	}

	// Stop the threads, close the sessions, and shut the seam down.
	void Stop()
	{
		_running = false;
		for (std::thread& thread : _threads)
			if (thread.joinable())
				thread.join();
		_threads.clear();

		for (const ServedSegment& segment : _segments)
		{
			ILink3::MarketSegmentGateway* gateway = _gateways[static_cast<size_t>(segment.CoreGroupId)].get();
			if (gateway != nullptr && gateway->State() == ILink3::SessionState::Established)
				gateway->Disconnect();
		}
		_server.Stop();
		_loggerManager.Stop();
	}

private:
	// The server's shared-memory identity: its directory-path name and which core groups exist
	// (bit 0 is the admin channel; each served segment adds its own).
	static Provider::ServerHeader MakeServerHeader(const std::string& serverName, const std::vector<ServedSegment>& segments)
	{
		Provider::ServerHeader header = Provider::Server::DefaultServerHeader;
		header.ServerName = Tools::String128(Provider::ServerContext::GetDirectoryPath(serverName).string());
		header.CoreGroupIds.Set(Socket::SocketChannel::Admin);
		for (const ServedSegment& segment : segments)
			header.CoreGroupIds.Set(segment.CoreGroupId);
		return header;
	}

	// Commit the catalog: every future of a requested root on a served segment, stamped with its
	// segment's core group. Catalog order defines the instrument header ids.
	void CommitCatalog(const std::vector<std::string>& roots)
	{
		for (const SecDef::Loaded& loaded : _secdef.Instruments())
		{
			// Step 1: Futures only for now (spread headers need the leg-allocation flow).
			if (loaded.Header.Base.InstrumentType != Data::InstrumentType::Future)
				continue;

			// Step 2: Only instruments on a segment this server trades.
			const ServedSegment* segment = FindSegment(loaded.MarketSegmentID);
			if (segment == nullptr)
				continue;

			// Step 3: Only the requested product roots.
			const std::string root = loaded.Header.Base.Root.ToString();
			bool wanted = false;
			for (const std::string& requestedRoot : roots)
				wanted |= root == requestedRoot;
			if (!wanted)
				continue;

			// Step 4: Commit a copy stamped with the segment's core group, and remember which
			// security-definition record this header id came from.
			Data::InstrumentHeader128 header = loaded.Header;
			header.Base.CoreGroupId = static_cast<uint8_t>(segment->CoreGroupId);
			_server.OnInstrumentHeader(header);
			_loadedByHeaderId.push_back(&loaded);
		}
		std::cout << "CmeServer: committed " << _loadedByHeaderId.size() << " instruments to the catalog.\n";
	}

	const ServedSegment* FindSegment(int32_t marketSegmentId) const
	{
		for (const ServedSegment& segment : _segments)
			if (segment.MarketSegmentID == marketSegmentId)
				return &segment;
		return nullptr;
	}

	// Build the router for a freshly allocated instrument and wire its events into the server.
	void BuildRouter(const Provider::AllocateInstrument& allocate)
	{
		// Step 1: Idempotent — a re-allocation of the same instrument keeps its router.
		if (_routers[static_cast<size_t>(allocate.InstrumentId)] != nullptr)
			return;

		// Step 2: The instrument's exchange identity and price scale from its catalog record.
		const SecDef::Loaded& loaded = *_loadedByHeaderId[static_cast<size_t>(allocate.InstrumentHeaderId)];
		const ServedSegment* segment = FindSegment(loaded.MarketSegmentID);
		ILink3::MarketSegmentGateway* gateway = _gateways[static_cast<size_t>(segment->CoreGroupId)].get();
		const int32_t ordersCapacity = _server.Context().ServerHeader().GetReadonlyRef().OrdersCapacity();

		auto router = std::make_unique<ILink3::InstrumentRouter>(allocate.InstrumentId, loaded.SecurityID,
			gateway, loaded.Header.AsInstrumentHeader().TickSize, loaded.DisplayFactor,
			_config.Parties.Operator, _config.Parties.Location, ordersCapacity);

		// Step 3: The router's events flow into the server, which merges them into the shared
		// order state and returns them to the owning strategy.
		router->OnOrderState = [this](const Execution::OrderState& state)
		{
			Execution::OrderState copy = state;
			_server.OnOrderState(copy);
		};
		router->OnFill = [this](const Execution::Fill& fill)
		{
			Execution::Fill copy = fill;
			_server.OnFill(copy);
		};
		router->OnOrderRejected = [this](const Execution::OrderRejected& rejected, const std::string& text)
		{
			Execution::OrderRejected copy = rejected;
			_server.OnOrderRejected(copy, text);
		};

		_routers[static_cast<size_t>(allocate.InstrumentId)] = std::move(router);
		std::cout << "CmeServer: instrument " << allocate.InstrumentId << " -> SecurityID "
		          << loaded.SecurityID << " on segment " << loaded.MarketSegmentID << "\n";
	}

	// Route one execution report to its instrument's router: the client order id it carries has
	// the instrument id packed inside. Reports with no per-order id are only logged.
	void DispatchExecutionReport(const ILink3::FramedMessage& message)
	{
		uint64_t clientOrderId = 0;
		if (ILink3::InstrumentRouter::TryGetClientOrderId(message, clientOrderId))
		{
			const int32_t instrumentId = Execution::OrderIdAllocator::GetInstrumentId(clientOrderId);
			ILink3::InstrumentRouter* router = static_cast<size_t>(instrumentId) < _routers.size()
				? _routers[static_cast<size_t>(instrumentId)].get() : nullptr;
			if (router != nullptr)
			{
				router->OnExecutionReport(message);
				return;
			}
		}
		std::cout << "CmeServer: unrouted " << ILink3::ToObjectType(message.TemplateId) << ": "
		          << ILink3::ToJsonLine(message.TemplateId, message.Body) << "\n";
	}
};

} // namespace Server
