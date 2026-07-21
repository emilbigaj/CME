#pragma once

// The CME trading server: the composition that plugs native market data and order entry into
// the HFT library's trading-server seam. It owns one Provider::Server (the shared-memory hub
// the strategies connect to) and, per configured market segment: an order-entry gateway, a
// market-data receiver and book builder, and the two pinned threads that drive them.
// Strategies never see CME — they allocate instruments, read books, and send order targets
// exactly as they do against any other venue server.
//
// Start-up: the whole instrument universe from the security-definition file — every future and
// calendar spread — is committed to the server's catalog, each header stamped with its
// segment's core group when this server trades that segment. When a strategy allocates an
// instrument, the callback builds its order router and subscribes its market data. From then
// on the segment's two threads carry everything:
//
//   market-data thread (pinned): joins the channel's incremental feed, builds books, and
//     publishes book updates and trades into the server, which broadcasts to the strategies;
//   execution thread (pinned): owns the segment's gateway completely — drains the strategies'
//     order targets and polls the connection — so all session state stays on one core.
//
// The threads pin to the cores the segment's settings name, so a segment's market data,
// execution, and the strategy beside them share one core complex and its cache.

#include "CmeServerConfig.hpp"
#include "MarketSegments.hpp"
#include "ILink3Config.hpp"
#include "CmeLogger.hpp"
#include "MarketSegmentGateway.hpp"
#include "SessionStore.hpp"
#include "InstrumentRouter.hpp"
#include "SecDefFile.hpp"
#include "BookBuilder.hpp"
#include "ChannelConfig.hpp"
#include "UdpReceiver.hpp"
#include "Server.hpp"             // Provider::Server (the shared-memory hub)
#include "OrderIdAllocator.hpp"
#include "LowLatency.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace Server
{

template <typename Connection>
class BasicCmeServer
{
	// The gateway and router stamped for this server's transport.
	using GatewayType = ILink3::BasicMarketSegmentGateway<Connection>;
	using RouterType = ILink3::BasicInstrumentRouter<GatewayType>;

	// One running market segment: its settings, its venue connections, and the handoff of
	// fresh subscriptions from the admin thread to the market-data thread (a tiny single
	// producer, single consumer ring — the admin thread writes, the market-data thread reads).
	struct Segment
	{
		MarketSegment Config;
		std::unique_ptr<GatewayType> Gateway;
		Mdp3::UdpReceiver Receiver;
		Mdp3::UdpReceiver SnapshotReceiver;   // read only while any instrument awaits a snapshot
		Mdp3::BookBuilder Books;

		struct Subscription
		{
			int32_t SecurityID = 0;
			int32_t InstrumentId = 0;
			double TickSize = 0.0;
			double DisplayFactor = 0.0;
		};
		static constexpr uint32_t SubscriptionRingLength = 256;
		std::array<Subscription, SubscriptionRingLength> Subscriptions;
		std::atomic<uint32_t> SubscriptionsWritten{0};   // admin thread advances (release)
		uint32_t SubscriptionsRead = 0;                  // market-data thread only

		// Queue-tracking lifecycle commands from the execution thread, same shape of handoff.
		static constexpr uint32_t QueueRingLength = 256;
		std::array<Mdp3::QueueTracker::Command, QueueRingLength> QueueCommands;
		std::atomic<uint32_t> QueueWritten{0};           // execution thread advances (release)
		uint32_t QueueRead = 0;                          // market-data thread only

		int64_t NextReconnectNanos = 0;                  // paces reconnect attempts (execution thread only)
	};

	// Hand one queue-tracking command across a segment's ring.
	static void PushQueueCommand(Segment& segment, const Mdp3::QueueTracker::Command& command)
	{
		const uint32_t written = segment.QueueWritten.load(std::memory_order_relaxed);
		segment.QueueCommands[written % Segment::QueueRingLength] = command;
		segment.QueueWritten.store(written + 1, std::memory_order_release);
	}

	// ---- configuration (set once) ----
	CmeServerConfig _config;
	MarketSegments _marketSegments;
	ILink3::ILink3Config _ilink3Config;
	const SecDef::SecDefFile& _secdef;        // owned by the caller; outlives this server
	const Mdp3::ChannelConfig& _channels;     // owned by the caller; outlives this server

	// ---- the seam and the venue ----
	Provider::Server _server;
	ILink3::CmeLoggerManager _loggerManager;
	std::vector<std::unique_ptr<Segment>> _segments;
	std::vector<const SecDef::Loaded*> _loadedByHeaderId;   // catalog order == header id
	std::array<std::unique_ptr<RouterType>, 64> _routers;   // by instrument id

	// ---- service threads ----
	std::vector<std::thread> _threads;
	std::atomic<bool> _running{false};
	uint64_t _partyDetailsListId = 0;   // the registered party id; reapplied after every reconnect

public:
	// Raised with any exception a service thread catches; the owner wires it to the alerts.
	std::function<void(const std::exception&)> Exception;

	BasicCmeServer(const CmeServerConfig& config, const MarketSegments& marketSegments,
	          const ILink3::ILink3Config& ilink3Config, const SecDef::SecDefFile& secdef,
	          const Mdp3::ChannelConfig& channels)
		: _config(config), _marketSegments(marketSegments), _ilink3Config(ilink3Config),
		  _secdef(secdef), _channels(channels),
		  _server(MakeServerHeader(config, marketSegments, secdef))
	{
		// Step 1: Commit the whole instrument universe to the catalog.
		CommitCatalog();

		// Step 2: When a strategy allocates an instrument, build its order router and subscribe
		// its market data.
		_server.AllocateInstrument = [this](const Provider::AllocateInstrument& allocate)
		{
			OnAllocateInstrument(allocate);
		};

		// Step 3: When a strategy sends an order target (already risk-checked by the server),
		// route it to the instrument's router.
		_server.OrderTarget = [this](const Execution::OrderTarget& target)
		{
			RouterType* router = _routers[static_cast<size_t>(target.OrderHeader.InstrumentId)].get();
			if (router != nullptr)
				router->OnOrderTarget(target);
			else
				std::cout << "CmeServer: no router for instrument " << target.OrderHeader.InstrumentId
				          << " (its segment is not served)\n";
		};

		// Step 4: One segment bundle per configured market segment: the order gateway (and its
		// message log), the market-data receiver, and the book builder wired into the server.
		for (const MarketSegment& segmentConfig : _marketSegments.Segments)
		{
			auto segment = std::make_unique<Segment>();
			segment->Config = segmentConfig;

			ILink3::CmeLogger& logger = _loggerManager.NewLogger(
				ILink3::CmeLoggerManager::LogDirectory("/mnt/S", _ilink3Config.Environment, segmentConfig.MarketSegmentID), segmentConfig.MarketSegmentID);
			segment->Gateway = std::make_unique<GatewayType>(_ilink3Config, segmentConfig.MarketSegmentID, &logger,
				/*useSecondary*/ false, ILink3::SessionStore::Directory("/mnt/S", _ilink3Config.Environment));
			segment->Gateway->OnBusinessMessage = [this](const ILink3::FramedMessage& message)
			{
				DispatchExecutionReport(message);
			};

			segment->Books.OnMarketByPrice = [this](Data::MarketByPrice& marketByPrice, std::span<uint8_t> span)
			{
				_server.OnMarketByPrice(marketByPrice, span);
			};
			segment->Books.OnTrade = [this](const Data::Trade& trade)
			{
				_server.OnTrade(trade);
			};

			// The queue tracker reads the authoritative book (this market-data thread writes it,
			// so the read is consistent) and publishes into the server-owned order state.
			segment->Books.ReadSide = [this](int32_t instrumentId, bool isBid, Data::Level* levels, int32_t capacity)
			{
				const Data::MarketByPrice64& book = _server.Context().GetMarketByPrice64(instrumentId).GetReadonlyRef();
				const Data::SideByPrice64& side = isBid ? book.Bids : book.Asks;
				int32_t count = 0;
				Data::SideByPrice64::Enumerator enumerator = side.GetEnumerator();
				while (count < capacity && enumerator.MoveNext())
					levels[count++] = enumerator.Current();
				return count;
			};
			segment->Books.Queue.ReadLevelQuantity = [this](int32_t instrumentId, bool isBid, int32_t ticks)
			{
				const Data::MarketByPrice64& book = _server.Context().GetMarketByPrice64(instrumentId).GetReadonlyRef();
				return isBid ? book.Bids.GetQuantity(ticks) : book.Asks.GetQuantity(ticks);
			};
			segment->Books.Queue.IsPriceVisible = [this](int32_t instrumentId, bool isBid, int32_t ticks)
			{
				// Visible when the side shows fewer levels than the exchange publishes (ten for
				// our products), or when the price is at or better than the worst shown level.
				const Data::MarketByPrice64& book = _server.Context().GetMarketByPrice64(instrumentId).GetReadonlyRef();
				const Data::SideByPrice64& side = isBid ? book.Bids : book.Asks;
				if (side.Count() < 10)
					return true;
				Data::SideByPrice64::Enumerator enumerator = side.GetEnumerator();
				int32_t worst = 0;
				for (int32_t n = 0; n < 10 && enumerator.MoveNext(); ++n)
					worst = enumerator.Current().Ticks;
				return isBid ? ticks >= worst : ticks <= worst;
			};
			segment->Books.Queue.OnQuantityAhead = [this](uint64_t clientOrderId, int32_t quantityAhead)
			{
				_server.OnQuantityAhead(clientOrderId, quantityAhead);
			};

			_segments.push_back(std::move(segment));
		}
	}

	// Stopping on destruction keeps a thrown exception from unwinding into live service threads.
	~BasicCmeServer()
	{
		Stop();
	}

	Provider::Server& Server() { return _server; }

	// Test hook: kill every segment's connection without a Terminate, as a network failure
	// would, so the reconnect path can be exercised end to end.
	void DropConnections()
	{
		for (std::unique_ptr<Segment>& segment : _segments)
			segment->Gateway->DropConnection();
	}

	// The catalog position of an instrument by its exchange id, or -1 if not committed.
	int32_t FindInstrumentHeaderId(int32_t securityId) const
	{
		for (size_t headerId = 0; headerId < _loadedByHeaderId.size(); ++headerId)
			if (_loadedByHeaderId[headerId]->SecurityID == securityId)
				return static_cast<int32_t>(headerId);
		return -1;
	}

	// Open every segment's connections, then start the pinned service threads.
	void Start()
	{
		// Step 1: Start the message-log drain.
		_loggerManager.Start();

		// Step 2: Register the trading parties once on the Order Entry Service Gateway, so
		// every order references the registered id instead of dragging its own definition.
		// 0 (not configured, or registration failed) means on-demand mode.
		_partyDetailsListId = RegisterParties();

		// Step 3: Open every segment's venue connections: log on to the order gateway (the
		// serving loop then shortens its receive limit so a quiet connection hands control
		// straight back), and join the market-data channel's incremental feed.
		for (std::unique_ptr<Segment>& segment : _segments)
		{
			segment->Gateway->Connect();
			if (!segment->Gateway->Logon())
				throw std::runtime_error("CmeServer: logon failed on segment " + std::to_string(segment->Config.MarketSegmentID));
			segment->Gateway->SetPartyDetailsListId(_partyDetailsListId);
			segment->Gateway->SetReceiveTimeout(1);

			const Mdp3::Channel* channel = _channels.FindChannel(segment->Config.Channel);
			if (channel == nullptr)
				throw std::runtime_error("CmeServer: channel " + std::to_string(segment->Config.Channel) + " not in the channel configuration");
			const Mdp3::Connection* feed = channel->Find("I", 'A');
			if (feed == nullptr)
				throw std::runtime_error("CmeServer: channel " + std::to_string(segment->Config.Channel) + " has no incremental feed");
			segment->Receiver.Join(feed->Ip, feed->Port, _config.MarketDataInterfaceIp);
			const Mdp3::Connection* snapshotFeed = channel->Find("S", 'A');
			if (snapshotFeed == nullptr)
				throw std::runtime_error("CmeServer: channel " + std::to_string(segment->Config.Channel) + " has no snapshot feed");
			segment->SnapshotReceiver.Join(snapshotFeed->Ip, snapshotFeed->Port, _config.MarketDataInterfaceIp, /*recvTimeoutMilliseconds*/ 1);

			std::cout << "CmeServer: " << segment->Config.Name << " (segment " << segment->Config.MarketSegmentID
			          << ") up — gateway session open, market data " << feed->Id
			          << " (" << feed->Ip << ":" << feed->Port << "), core group " << segment->Config.CoreGroupId << "\n";
		}

		// Step 4: The admin thread services connections and allocations on the background cores.
		_running = true;
		_threads.push_back(Tools::LowLatency::StartBackgroundThread("CmeServer.Admin", [this]()
		{
			while (_running.load(std::memory_order_relaxed))
			{
				Guarded([this]() { _server.ReadAdmin(); });
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		}));

		// Step 5: Per segment, the two pinned owners: market data and execution.
		for (std::unique_ptr<Segment>& segmentPtr : _segments)
		{
			Segment* segment = segmentPtr.get();
			_threads.emplace_back([this, segment]()
			{
				Pin(segment->Config.MarketDataCore);
				RunMarketData(*segment);
			});
			_threads.emplace_back([this, segment]()
			{
				Pin(segment->Config.ExecutionCore);
				RunExecution(*segment);
			});
		}
	}

	// Stop the threads, close the connections, and shut the seam down.
	void Stop()
	{
		_running = false;
		for (std::thread& thread : _threads)
			if (thread.joinable())
				thread.join();
		_threads.clear();

		for (std::unique_ptr<Segment>& segment : _segments)
		{
			if (segment->Gateway->State() == ILink3::SessionState::Established)
				segment->Gateway->Disconnect();
			segment->Receiver.Close();
			segment->SnapshotReceiver.Close();
		}
		_server.Stop();
		_loggerManager.Stop();
	}

private:
	// The server's shared-memory identity: its directory-path name, which core groups exist
	// (bit 0 is the admin channel; each segment adds its own), and room for every instrument.
	static Provider::ServerHeader MakeServerHeader(const CmeServerConfig& config, const MarketSegments& marketSegments,
	                                               const SecDef::SecDefFile& secdef)
	{
		Provider::ServerHeader header = Provider::Server::DefaultServerHeader;
		header.ServerName = Tools::String128(Provider::ServerContext::GetDirectoryPath(config.ServerName).string());
		header.InstrumentsCapacity = static_cast<int32_t>(secdef.Instruments().size()) + 1024;
		header.CoreGroupIds.Set(Socket::SocketChannel::Admin);
		for (const MarketSegment& segment : marketSegments.Segments)
			header.CoreGroupIds.Set(segment.CoreGroupId);
		return header;
	}

	// Register the trading parties on the Order Entry Service Gateway — the dedicated CME
	// segment that stores party lists for the firm — over a short-lived session at start-up.
	// Returns the registered id for every trading session to reference, or 0 (= on-demand
	// mode, a definition paired with every order) when no gateway is configured or anything
	// fails: trading is never blocked on this step.
	uint64_t RegisterParties()
	{
		// Step 1: Not configured means on-demand.
		const int32_t serviceSegmentId = _ilink3Config.ServiceGatewayMarketSegmentID;
		if (serviceSegmentId == 0)
			return 0;

		// Step 2: Open a session to the service gateway, register, and close it again. This
		// session is deliberately ephemeral (no session directory): nothing recoverable flows
		// on it, and a fresh id per run keeps each registration collision-free.
		try
		{
			ILink3::CmeLogger& logger = _loggerManager.NewLogger(
				ILink3::CmeLoggerManager::LogDirectory("/mnt/S", _ilink3Config.Environment, serviceSegmentId), serviceSegmentId);
			ILink3::MarketSegmentGateway gateway(_ilink3Config, serviceSegmentId, &logger);
			gateway.Connect();
			if (!gateway.Logon())
			{
				std::cout << "CmeServer: service-gateway logon failed; trading on-demand.\n";
				return 0;
			}
			const bool registered = gateway.RegisterPartyDetails();
			gateway.Disconnect();
			return registered ? gateway.PartyDetailsListId() : 0;
		}
		catch (const std::exception& error)
		{
			std::cout << "CmeServer: party registration failed (" << error.what() << "); trading on-demand.\n";
			return 0;
		}
	}

	// Commit every instrument in the security-definition file — the whole universe, futures and
	// calendar spreads alike. Instruments on a served segment carry its core group; the rest
	// stay browsable in the catalog but route nowhere until their segment is added. Catalog
	// order defines the instrument header ids.
	void CommitCatalog()
	{
		for (const SecDef::Loaded& loaded : _secdef.Instruments())
		{
			const MarketSegment* segment = FindSegment(loaded.MarketSegmentID);
			Data::InstrumentHeader128 header = loaded.Header;
			header.Base.CoreGroupId = segment != nullptr ? static_cast<uint8_t>(segment->CoreGroupId) : uint8_t(0);
			_server.OnInstrumentHeader(header);
			_loadedByHeaderId.push_back(&loaded);
		}
		std::cout << "CmeServer: committed " << _loadedByHeaderId.size() << " instruments to the catalog.\n";
	}

	const MarketSegment* FindSegment(int32_t marketSegmentId) const
	{
		for (const MarketSegment& segment : _marketSegments.Segments)
			if (segment.MarketSegmentID == marketSegmentId)
				return &segment;
		return nullptr;
	}

	Segment* FindSegmentBundle(int32_t marketSegmentId)
	{
		for (std::unique_ptr<Segment>& segment : _segments)
			if (segment->Config.MarketSegmentID == marketSegmentId)
				return segment.get();
		return nullptr;
	}

	// A freshly allocated instrument: build its order router and hand its market-data
	// subscription to the segment's market-data thread.
	void OnAllocateInstrument(const Provider::AllocateInstrument& allocate)
	{
		// Step 1: Idempotent — a re-allocation of the same instrument keeps its router.
		if (_routers[static_cast<size_t>(allocate.InstrumentId)] != nullptr)
			return;

		// Step 2: Only instruments on a served segment become tradeable.
		const SecDef::Loaded& loaded = *_loadedByHeaderId[static_cast<size_t>(allocate.InstrumentHeaderId)];
		Segment* segment = FindSegmentBundle(loaded.MarketSegmentID);
		if (segment == nullptr)
		{
			std::cout << "CmeServer: instrument " << allocate.InstrumentId << " (SecurityID " << loaded.SecurityID
			          << ") is on unserved segment " << loaded.MarketSegmentID << " — catalog only.\n";
			return;
		}

		// Step 3: The order router, its events flowing back into the server.
		const double tickSize = loaded.Header.AsInstrumentHeader().TickSize;
		const int32_t ordersCapacity = _server.Context().ServerHeader().GetReadonlyRef().OrdersCapacity();
		auto router = std::make_unique<RouterType>(allocate.InstrumentId, loaded.SecurityID,
			segment->Gateway.get(), tickSize, loaded.DisplayFactor,
			_ilink3Config.Parties.Operator, _ilink3Config.Parties.Location, ordersCapacity);
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
		// Step 3b: The order lifecycle feeds the queue tracker across the segment's ring: sent
		// starts the pending log at the price, the acknowledgment reconciles it, partial fills
		// shrink our size, and done frees the slot.
		Segment* segmentPointer = segment;
		const int32_t securityId = loaded.SecurityID;
		const int32_t instrumentId = allocate.InstrumentId;
		router->OnOrderSent = [segmentPointer, securityId, instrumentId](uint64_t clientOrderId, int32_t ticks, int32_t quantity, bool isBid)
		{
			Mdp3::QueueTracker::Command command{};
			command.Kind = Mdp3::QueueTracker::CommandKind::PreArm;
			command.IsBid = isBid ? 1 : 0;
			command.SecurityID = securityId;
			command.InstrumentId = instrumentId;
			command.Ticks = ticks;
			command.Quantity = quantity;
			command.ClientOrderId = clientOrderId;
			PushQueueCommand(*segmentPointer, command);
		};
		router->OnOrderLive = [segmentPointer](uint64_t clientOrderId, uint64_t exchangeOrderId)
		{
			Mdp3::QueueTracker::Command command{};
			command.Kind = Mdp3::QueueTracker::CommandKind::Arm;
			command.ClientOrderId = clientOrderId;
			command.ExchangeOrderID = exchangeOrderId;
			PushQueueCommand(*segmentPointer, command);
		};
		router->OnOrderQuantity = [segmentPointer](uint64_t clientOrderId, int32_t remainingQuantity)
		{
			Mdp3::QueueTracker::Command command{};
			command.Kind = Mdp3::QueueTracker::CommandKind::OwnQuantity;
			command.Quantity = remainingQuantity;
			command.ClientOrderId = clientOrderId;
			PushQueueCommand(*segmentPointer, command);
		};
		router->OnOrderDone = [segmentPointer](uint64_t clientOrderId)
		{
			Mdp3::QueueTracker::Command command{};
			command.Kind = Mdp3::QueueTracker::CommandKind::Disarm;
			command.ClientOrderId = clientOrderId;
			PushQueueCommand(*segmentPointer, command);
		};

		_routers[static_cast<size_t>(allocate.InstrumentId)] = std::move(router);

		// Step 4: Hand the market-data subscription to the segment's market-data thread.
		const uint32_t written = segment->SubscriptionsWritten.load(std::memory_order_relaxed);
		segment->Subscriptions[written % Segment::SubscriptionRingLength] =
			typename Segment::Subscription{loaded.SecurityID, allocate.InstrumentId, tickSize, loaded.DisplayFactor};
		segment->SubscriptionsWritten.store(written + 1, std::memory_order_release);

		std::cout << "CmeServer: instrument " << allocate.InstrumentId << " -> SecurityID "
		          << loaded.SecurityID << " on segment " << loaded.MarketSegmentID << "\n";
	}

	// The market-data owner: adopt fresh subscriptions, then pump the feed into the books.
	void RunMarketData(Segment& segment)
	{
		std::array<uint8_t, 65536> buffer{};
		while (_running.load(std::memory_order_relaxed))
		{
			Guarded([&]()
			{
				// Step 1: Adopt any subscriptions the admin thread handed over.
				while (segment.SubscriptionsRead != segment.SubscriptionsWritten.load(std::memory_order_acquire))
				{
					const typename Segment::Subscription& subscription =
						segment.Subscriptions[segment.SubscriptionsRead % Segment::SubscriptionRingLength];
					segment.Books.Subscribe(subscription.SecurityID, subscription.InstrumentId,
						subscription.TickSize, subscription.DisplayFactor);
					++segment.SubscriptionsRead;
				}
				while (segment.QueueRead != segment.QueueWritten.load(std::memory_order_acquire))
				{
					segment.Books.Queue.Apply(segment.QueueCommands[segment.QueueRead % Segment::QueueRingLength]);
					++segment.QueueRead;
				}

				// Step 2: One receive; a whole datagram goes to the book builder.
				const ssize_t n = segment.Receiver.Recv(buffer);
				if (n > 0)
					segment.Books.OnPacket(std::span<const uint8_t>(buffer.data(), static_cast<size_t>(n)), Tools::Timestamp::UtcNow());

				// Step 3: While any instrument awaits its snapshot, drain the snapshot feed too
				// (otherwise it is left unread; the cyclic data ages out in the kernel harmlessly).
				while (segment.Books.StaleCount() > 0)
				{
					const ssize_t snapshotBytes = segment.SnapshotReceiver.Recv(buffer);
					if (snapshotBytes <= 0)
						break;
					segment.Books.OnSnapshotPacket(std::span<const uint8_t>(buffer.data(), static_cast<size_t>(snapshotBytes)), Tools::Timestamp::UtcNow());
				}
			});
		}
	}

	// The execution owner: drain the strategies' order targets, poll the gateway, and bring a
	// dropped session back. Targets keep draining while the session is down — the routers
	// reject them straight back, so strategies always know where they stand.
	void RunExecution(Segment& segment)
	{
		while (_running.load(std::memory_order_relaxed))
		{
			Guarded([&]()
			{
				_server.ReadExecution(segment.Config.CoreGroupId);
				segment.Gateway->Poll();
				if (segment.Gateway->State() == ILink3::SessionState::Disconnected)
					MaybeReconnect(segment);
			});
		}
	}

	// One paced reconnect attempt for a segment whose session has dropped. On success the
	// resumed session has already asked for everything it missed; drain that replay (bounded),
	// re-apply the party registration, then fail any order whose send died with the old
	// connection — the exchange never saw it, its acknowledgment would have replayed by now.
	void MaybeReconnect(Segment& segment)
	{
		// Step 1: Pace the attempts (CME refuses an immediate reconnect while it still tears
		// the old session down); the address alternation covers primary and backup.
		const int64_t now = Tools::Timestamp::UtcNow().NanosSinceEpoch;
		if (now < segment.NextReconnectNanos)
			return;
		segment.NextReconnectNanos = now + 1'000'000'000LL;
		std::cout << "CmeServer: segment " << segment.Config.MarketSegmentID << " session down; reconnecting...\n";
		if (!segment.Gateway->TryReconnect())
			return;

		// Step 2: The session is back: restore its settings and drain the recovery replay so
		// working-order state is current before anything new happens.
		segment.Gateway->SetPartyDetailsListId(_partyDetailsListId);
		segment.Gateway->SetReceiveTimeout(1);
		const int64_t drainDeadline = Tools::Timestamp::UtcNow().NanosSinceEpoch + 3'000'000'000LL;
		while (segment.Gateway->RetransmitPending() > 0
		    && Tools::Timestamp::UtcNow().NanosSinceEpoch < drainDeadline)
			segment.Gateway->Poll();

		// Step 3: Whatever is still unacknowledged was lost with the old connection.
		for (const std::unique_ptr<RouterType>& router : _routers)
			if (router != nullptr && router->GatewayUsed() == segment.Gateway.get())
			{
				const int32_t swept = router->SweepUnacknowledged();
				if (swept > 0)
					std::cout << "CmeServer: instrument " << router->InstrumentId() << ": failed "
					          << swept << " orders the exchange never saw.\n";
			}
	}

	// Pin the calling thread to its segment core at real-time priority; degrade loudly but
	// keep running where the machine refuses (an unprivileged test run).
	void Pin(int32_t core)
	{
		try
		{
			Tools::LowLatency::PinCurrentThreadToCore(core);
			Tools::LowLatency::SetThreadPriorityCritical();
		}
		catch (const std::exception& error)
		{
			std::cout << "CmeServer: pinning to core " << core << " degraded: " << error.what() << "\n";
		}
	}

	// Run one service-loop pass, reporting anything thrown to the alert wiring.
	template <typename Action>
	void Guarded(const Action& action)
	{
		try
		{
			action();
		}
		catch (const std::exception& error)
		{
			std::cout << "CmeServer: " << error.what() << "\n";
			if (Exception)
				Exception(error);
		}
	}

	// Route one execution report to its instrument's router: the client order id it carries has
	// the instrument id packed inside. Reports with no per-order id are only logged.
	void DispatchExecutionReport(const ILink3::FramedMessage& message)
	{
		uint64_t clientOrderId = 0;
		if (RouterType::TryGetClientOrderId(message, clientOrderId))
		{
			const int32_t instrumentId = Execution::OrderIdAllocator::GetInstrumentId(clientOrderId);
			RouterType* router = static_cast<size_t>(instrumentId) < _routers.size()
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

// The kernel-socket form: bring-up, certification, and ordinary network paths. Production
// instantiates BasicCmeServer<ILink3::ZfConnection> for the kernel-bypass transport.
using CmeServer = BasicCmeServer<ILink3::TcpConnection>;

} // namespace Server
