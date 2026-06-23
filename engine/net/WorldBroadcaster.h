// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "AuthTracker.h"
#include "GameProtocol.h"
#include "INetwork.h"
#include "entity/EntityId.h"
#include "flight/IGravityField.h"
#include "loop/ISimUpdate.h"
#include "spatial/SpatialIndex.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class ILogger;

namespace fl {
class EntityManager;
class FlightIntegrator; // full definition in WorldBroadcaster.cpp
struct EntityState;
struct ControlInput;      // engine/flight/AeroForces.h
struct FlightModelData;   // engine/flight/FlightModelData.h
struct IEntityController; // engine/entity/IEntityController.h
class EntityTypeRegistry;
class WeatherController;
} // namespace fl

namespace fl {

// Parsed, validated client input stored per connected peer.
struct PeerInputState {
    // 8-byte field first to avoid padding.
    uint64_t lastActivityTick{0}; // tick of last MsgClientInput or MsgHeartbeat; set in onConnect
    // 4-byte fields next.
    float throttle{0.f};
    float elevator{0.f};
    float aileron{0.f};
    float rudder{0.f};
    float viewAxis[3]{1.f, 0.f, 0.f};
    uint32_t lastSeqNum{0};          // seqNum of last accepted input
    uint32_t estimatedDelayTicks{0}; // one-way delay in sim ticks (derived from tickIndex)
    // 1-byte fields last.
    uint8_t buttons{0};
    bool hasSeq{false}; // false until first input received from this peer
};

// One simulated entity together with its control source. The registry is EntityId-keyed (not peer-
// keyed) so peers, AI, and scripted entities are all stepped uniformly in onTick. unique_ptr members
// hold incomplete types here; WorldBroadcaster's destructor is defined in the .cpp where both are
// complete.
struct ControlledEntity {
    EntityId id;
    std::unique_ptr<FlightIntegrator> sim;
    std::unique_ptr<IEntityController> controller;
};

// Pre-start scalar configuration. Bundles the init-time setters so callers configure rate limiting,
// the per-IP cap, admin-auth lockout, MOTD, and the operator password in one applyConfig() call
// instead of remembering six separate "call before gameLoop.start()" setters. The hot-reload setters
// (setMotd, setBannedAddresses, setAllowedAddresses, ...) remain available for runtime changes.
struct WorldBroadcasterConfig {
    int connectRateLimit{5};             // max connects per window per IP
    int connectRateWindowS{10};          // sliding-window length (seconds)
    int floodMultiplier{3};              // MsgClientInput flood threshold multiplier
    int maxConnectionsPerIp{0};          // simultaneous connections per IP; 0 = unlimited
    int adminAuthMaxFailures{5};         // wrong operator passwords before per-IP lockout
    int adminAuthLockoutSeconds{300};    // lockout duration (seconds)
    std::string motd;                    // empty = no MOTD
    uint16_t motdDisplaySeconds{0};      // 0 = client default
    std::string operatorPassword;        // empty = network admin channel disabled
    int idleTimeoutS{0};                 // 0 = disabled; seconds of peer inactivity before disconnect
    float drawDistanceKm{200.f};         // per-peer interest radius; 0 = degenerate (empty snapshots)
    uint32_t baselineIntervalTicks{120}; // force full MsgEntityEntry records every N ticks for loss recovery
};

// Wraps EntityManager to provide a server-side ISimUpdate that:
//   1. Advances each peer's FlightIntegrator from stored client inputs.
//   2. Advances the entity simulation each tick (calls EntityManager::onTick).
//   3. Serializes live entity state into a MsgWorldSnapshot packet.
//   4. Broadcasts the packet to all connected clients via INetwork.
//   5. Calls INetwork::service(0) to flush the outbound ENet queue.
//
// Also implements INetworkEventHandler to:
//   - Spawn a player entity, create its FlightIntegrator, and send MsgConnectAck on connect.
//   - Kill the player entity and tear down its FlightIntegrator on disconnect.
//   - Decode and validate MsgClientInput packets.
//
// Threading: all ISimUpdate and INetworkEventHandler methods are called from
// the GameLoop sim thread. INetwork::setEventHandler(&broadcaster) must be
// called before GameLoop::start().
class WorldBroadcaster : public ISimUpdate, public INetworkEventHandler {
  public:
    // weather may be nullptr; when non-null it is ticked and broadcast each sim tick.
    WorldBroadcaster(EntityManager& entityManager, EntityTypeRegistry& registry, INetwork& net, ILogger& logger,
                     WeatherController* weather = nullptr);
    ~WorldBroadcaster(); // defined in .cpp — FlightIntegrator must be complete at destruction

    // ISimUpdate
    void onTick(double simDt, uint64_t tickIndex) override;

    // INetworkEventHandler
    void onConnect(uint32_t peerId) override;
    void onDisconnect(uint32_t peerId) override;
    void onReceive(uint32_t peerId, const void* data, std::size_t size) override;

    // Safe to call from any thread (main thread reads this for the LAN discovery beacon).
    int getPeerCount() const noexcept {
        return m_activePeerCount.load(std::memory_order_relaxed);
    }

    // Register a server-side controller (AI, scripted, ...) for an already-spawned entity. The entity
    // is then stepped every onTick exactly like a connected peer and serialized into MsgWorldSnapshot
    // for free — no peer required. The flight integrator is built from `model` (null = builtin UFO
    // model) and reset to the entity's current transform. Replaces any existing controller for the
    // entity. Sim-thread only. This is the seam future AI/scripted controllers plug into.
    void registerController(EntityId id, std::unique_ptr<IEntityController> controller,
                            std::shared_ptr<const FlightModelData> model = nullptr);

    // Peer management — all must be called from the sim thread (via GameLoop::enqueueSimCallback).

    // Gracefully disconnect one peer by ID.
    void kickPeer(uint32_t peerId);

    // Add a normalized IP to the in-memory ban set and kick any currently connected peers
    // with that IP. ip may be plain IPv4 ("1.2.3.4"), bare IPv6 ("::1"), bracketed IPv6
    // ("[::1]"), or IPv4-mapped IPv6 ("::ffff:1.2.3.4" or "[::ffff:1.2.3.4]").
    void banAddress(std::string ip);

    // Remove an IP from the ban set (same normalization rules as banAddress).
    void unbanAddress(const std::string& ip);

    // Clear the admin auth lockout for an IP immediately. Normalizes the IP.
    // Call from the sim thread (via GameLoop::enqueueSimCallback).
    // Returns true if a lockout was active and was cleared; false if the IP was not locked.
    bool unlockAdminAuth(const std::string& ip);

    // Iterate all connected peers. fn receives (peerId, full "ip:port" address string, EntityId,
    // one-way delay in sim ticks). The address string is copied per entry — safe despite
    // INetwork::getPeerAddress() returning a pointer backed by a single overwrite buffer.
    void forEachPeer(
        std::function<void(uint32_t peerId, const std::string& addr, EntityId eid, uint32_t delayTicks)> fn) const;

    // Replace the entire in-memory ban set. Safe to call before gameLoop.start().
    void setBannedAddresses(std::unordered_set<std::string> addrs);

    // Set the allowlist. Empty set = allowlist disabled (all IPs permitted).
    void setAllowedAddresses(std::unordered_set<std::string> addrs);

    // Return a copy of the current ban set (called from sim thread to save to file).
    std::unordered_set<std::string> getBannedAddresses() const;

    // Snapshot of admin auth lockout state — sim-thread-only read (acceptable monitoring
    // race, same pattern as getBannedAddresses() / liveCount()).
    AuthLockoutSummary getAuthLockoutSummary() const;

    // Set the terrain floor elevation (m) used for ground collision in each peer's
    // FlightIntegrator. Thread-safe; may be called from any thread. Serves as a global
    // fallback when no per-entity query function is set via setGroundElevationQuery().
    void setGroundElevation(float elev) noexcept {
        m_groundElevation.store(elev, std::memory_order_relaxed);
    }

    // Inject a per-entity terrain height query called on the sim thread each tick.
    // fn(worldX, worldZ) → terrain elevation (m). When set, this overrides the global
    // m_groundElevation scalar for each entity's FlightIntegrator::step() call.
    // Requires TerrainStreamer::heightAt() to be thread-safe (shared_mutex). Call before
    // gameLoop.start().
    void setGroundElevationQuery(std::function<float(double, double)> fn);

    // Set pre-cached peer spawn positions [x, y, z] in world space.
    // y must already include the terrain height + AGL offset, computed on the main thread
    // before gameLoop.start(). Positions are assigned round-robin to connecting peers.
    // Empty list = legacy behaviour: spawn at origin with y = m_groundElevation + 500 m.
    // Call before gameLoop.start(); never mutated after that.
    void setSpawnPoints(std::vector<std::array<double, 3>> points) noexcept;

    // World-XZ position of the most recently stepped peer entity (sim thread writes;
    // main thread may read to steer terrain loading).
    double cachedEntityX() const noexcept {
        return m_entityX.load(std::memory_order_relaxed);
    }
    double cachedEntityZ() const noexcept {
        return m_entityZ.load(std::memory_order_relaxed);
    }

    // Configure rate limiting; call before gameLoop.start().
    void setRateLimitParams(int maxConnects, int windowSeconds, int floodMultiplier);

    // Maximum simultaneous connections from one IP; 0 = unlimited. Call before gameLoop.start().
    void setMaxConnectionsPerIp(int max) noexcept;

    // Override the clock used for rate limiting and shutdown timing (for testing only).
    void setClock(const IClock& clock);

    // Shutdown countdown — all must be called from the sim thread (via enqueueSimCallback),
    // except setShutdownCallback which must be called before gameLoop.start().

    // Schedule a graceful shutdown. Broadcasts a MsgServerNotice at initiateShutdown time and
    // every warningIntervalS seconds thereafter; at T=0 sends a final notice and invokes the
    // shutdown callback. warningIntervalS == 0 skips intermediate notices (fires only at T=0).
    // Optional reason is prepended to each broadcast: "{reason} -- shutting down in X minutes."
    // Long reasons are safely truncated to fit MsgServerNotice::text[60].
    void initiateShutdown(uint32_t secondsDelay, uint32_t warningIntervalS, std::string reason = "");

    // Cancel a pending shutdown (no-op if none active).
    void cancelShutdown();

    // Push the scheduled shutdown back by additionalSeconds. Returns false if no shutdown is
    // active (no-op). On success resets the notice timer so clients see an immediate update.
    bool extendShutdown(uint32_t additionalSeconds);

    // Returns true if a shutdown is currently counting down (sim-thread-only read).
    bool isShuttingDown() const noexcept {
        return m_shuttingDown;
    }

    // Sim-thread only. Returns the spatial index rebuilt at the start of the most recent
    // onTick(). Consumers: interest management (#346), AoE warhead commands (#356); AI
    // controllers receive it via the si parameter of IEntityController::sample().
    [[nodiscard]] const SpatialIndex& spatialIndex() const noexcept {
        return m_spatialIndex;
    }

    // Seconds until the scheduled shutdown; 0 if none active (sim-thread-only read).
    uint32_t secondsUntilShutdown() const noexcept;

    // Register a callback invoked on the sim thread at T=0. Call before gameLoop.start().
    void setShutdownCallback(std::function<void()> fn);

    // Set the MOTD unicast to each connecting client after MsgConnectAck.
    // Empty string disables MOTD. May be called before gameLoop.start() or via
    // enqueueSimCallback for hot-reload (reload_config).
    void setMotd(std::string motd);

    // Set the display duration (seconds) embedded in MsgMotd (displaySeconds field).
    // 0 = client uses its own motd_display_s setting (the default).
    // Call alongside setMotd() before gameLoop.start() or via enqueueSimCallback.
    void setMotdDisplaySeconds(uint16_t seconds) noexcept;

    // Resolves an EntityDef::flightModelId to a parsed flight model for the spawn path. Injected as a
    // std::function so engine-net stays free of engine-content/engine-flight asset deps (the parse
    // lives in fl-server, which links both). Returns nullptr when the id is unknown; an empty
    // flightModelId or an unset resolver falls back to the builtin UFO model. Call before
    // gameLoop.start().
    using FlightModelResolver = std::function<std::shared_ptr<const FlightModelData>(const std::string& id)>;
    void setFlightModelResolver(FlightModelResolver fn);

    // Configure the operator password for MsgAdminCommand authentication.
    // Empty string disables the network admin channel. Call before gameLoop.start().
    void setOperatorPassword(std::string password);

    // Attach the admin command dispatcher for MsgAdminCommand handling.
    // Typically: [&adminRegistry](std::string_view cmd){ return adminRegistry.dispatch(cmd); }
    // Call before gameLoop.start(). Does not take ownership.
    void setAdminDispatch(std::function<std::string(std::string_view)> fn);

    // Wire CommandShell mark/drainSince callbacks so that output written inside
    // enqueueSimCallback lambdas is forwarded to the requesting peer as follow-on
    // MsgAdminResponseChunk packets on the next sim tick. Null functions (the default)
    // disable deferred output forwarding. Call before gameLoop.start().
    // Pattern mirrors setAdminDispatch to avoid coupling engine-net to engine-console.
    void setAdminShell(std::function<int()> markFn, std::function<std::vector<std::string>(int)> drainFn);

    // Configure per-IP failed-auth lockout for the operator network admin channel.
    // After maxFailures consecutive wrong passwords from the same IP the peer is kicked
    // and reconnections from that IP are refused for lockoutSeconds seconds.
    // Call before gameLoop.start().
    void setAdminAuthParams(int maxFailures, int lockoutSeconds);

    // Apply all pre-start scalar configuration in one call (rate limiting, per-IP cap, admin-auth
    // lockout, MOTD, operator password). Equivalent to the corresponding individual setters.
    // Call before gameLoop.start(). The admin dispatcher (setAdminDispatch) is wired separately.
    void applyConfig(const WorldBroadcasterConfig& cfg);

    // Disconnect peers that send no MsgClientInput and no MsgHeartbeat for this many seconds.
    // 0 = disabled (default). Converted to ticks at 60 Hz. Call before gameLoop.start() or
    // via enqueueSimCallback.
    void setIdleTimeout(int timeoutSeconds) noexcept;

    // Set the per-peer draw distance for snapshot interest management. Only entities within this
    // radius of a peer's own entity position are included in that peer's MsgWorldSnapshot.
    // 0 km = degenerate (queryRadius finds nothing; peers see empty snapshots). Default = 200 km.
    // Call before gameLoop.start() or via enqueueSimCallback for hot-reload (reload_config).
    void setDrawDistance(float km) noexcept;

    // Set the interval between full-snapshot baseline ticks (in sim ticks). On baseline ticks the
    // per-peer known-gen set is cleared, forcing full MsgEntityEntry records for all visible
    // entities — provides UDP packet-loss recovery. Default = 120 (2 s at 60 Hz). Range [1, +∞).
    // Call before gameLoop.start() or via enqueueSimCallback.
    void setBaselineInterval(uint32_t ticks) noexcept;

    // Set the gravity field applied to all FlightIntegrators spawned on this broadcaster (current
    // and future). Also records the planet radius sent to clients in MsgConnectAck so their terrain
    // rendering matches server physics. Defaults to CentralGravityField::earthInstance() /
    // 6371 km; only call this for non-Earth planets. Call before gameLoop.start().
    void setGravityField(const IGravityField& field, float planetRadiusKm = 6371.f) noexcept;

  private:
    void sendConnectAck(uint32_t peerId, EntityId assigned);
    void sendConnectRefusal(uint32_t peerId, ConnectRefusalCode code, const char* reason);
    // Send a complete admin command result over ENet. Short results (<=kAdminResponseFastPathMax
    // chars) go as a single MsgAdminResponse; longer results are streamed as MsgAdminResponseChunk
    // packets terminated by kChunkFlagEnd. reqId is echoed from the triggering MsgAdminCommand.
    static void sendAdminResponse(INetwork& net, uint32_t peerId, uint16_t reqId, const std::string& result);
    // Log, send a MsgConnectRefusal with the reason text for `code`, and disconnect the peer.
    // Centralizes the five onConnect rejection paths.
    void rejectConnection(uint32_t peerId, const std::string& ip, ConnectRefusalCode code);
    // Build a FlightIntegrator (from model, or builtin when null) reset to the entity's current
    // transform, and register it with the controller under the entity's index. Shared by onConnect
    // (PeerController) and registerController (AI/scripted). Sim-thread only.
    void addControlledEntity(EntityId id, std::unique_ptr<IEntityController> controller,
                             std::shared_ptr<const FlightModelData> model, float initialThrottle);

    // Resolve an entity type's EntityDef::flightModelId via the injected resolver. Returns null when
    // the id is empty, no resolver is set, or the id is unknown (logs Warn) — callers fall back to
    // the builtin model.
    std::shared_ptr<const FlightModelData> resolveFlightModel(EntityId id);

    void stepFlightSim(FlightIntegrator& fi, EntityState& state, const ControlInput& ctrl, double simDt);
    void broadcastShutdownNotice(uint16_t secsLeft, const char* text);
    static std::string makeShutdownMessage(uint32_t secsLeft, const std::string& reason = "");

    EntityManager& m_entityManager;
    EntityTypeRegistry& m_registry;
    INetwork& m_net;
    ILogger& m_logger;
    WeatherController* m_weather{nullptr};

    std::unordered_map<uint32_t, EntityId> m_peerEntities;
    std::unordered_map<uint32_t, PeerInputState> m_peerInputs;
    // EntityId.index -> {sim, controller}. Replaces the old peerId-keyed flight-sim map: any control
    // source (peer, AI, script) registers here and is stepped uniformly in onTick.
    std::unordered_map<uint32_t, ControlledEntity> m_controlledEntities;

    std::atomic<int> m_activePeerCount{0};
    uint64_t m_weatherBroadcastTick{0};        // throttle weather broadcasts to ~6 Hz
    uint64_t m_idleTimeoutTicks{0};            // 0 = disabled; pre-computed from idleTimeoutS × 60
    uint32_t m_turbRng{0xCAFEBABEu};           // per-broadcaster RNG for turbulence perturbation
    std::atomic<float> m_groundElevation{0.f}; // floor elevation passed to each FlightIntegrator::step
    std::atomic<double> m_entityX{0.0};        // last stepped entity world-X (sim writes; main reads)
    std::atomic<double> m_entityZ{0.0};        // last stepped entity world-Z

    std::vector<std::array<double, 3>> m_spawnPoints; // pre-cached [x,y,z]; sim-thread read-only after start
    uint32_t m_nextSpawnIdx{0};                       // round-robin counter; sim-thread only

    std::unordered_set<std::string> m_bannedAddresses; // in-memory ban list; sim-thread only

    // Per-IP sliding-window connection rate limiter (sim-thread only).
    struct ConnectRecord {
        std::deque<std::chrono::steady_clock::time_point> timestamps;
    };
    std::unordered_map<std::string, ConnectRecord> m_connectRecords;
    int m_connectRateLimit{5};
    int m_connectRateWindowS{10};
    int m_maxConnectionsPerIp{0}; // 0 = unlimited
    uint64_t m_ratePruneTick{0};  // coarse prune cadence counter (every 600 ticks)
    uint64_t m_currentTick{0};    // set at start of each onTick; used in onReceive for delay estimation

    // Per-peer packet flood detector (sim-thread only).
    struct PeerFloodState {
        uint32_t packetCount{0};
        std::chrono::steady_clock::time_point windowStart{};
    };
    std::unordered_map<uint32_t, PeerFloodState> m_peerFloodState;
    int m_floodMultiplier{3};

    std::unordered_set<std::string> m_allowedAddresses; // empty = allowlist disabled

    // Injectable clock for testing; defaults to steady_clock::now.
    const IClock* m_clock{&SystemClock::instance()};

    // MOTD state (set before gameLoop.start() or via enqueueSimCallback; read on sim thread only).
    std::string m_motd;               // empty = no MOTD sent
    uint16_t m_motdDisplaySeconds{0}; // 0 = client default

    // Resolves EntityDef::flightModelId -> FlightModelData at spawn (null = always builtin model).
    FlightModelResolver m_flightModelResolver;

    // Gravity field applied to all spawned integrators. Initialized to CentralGravityField::earthInstance()
    // in the constructor; override with setGravityField() for non-Earth servers.
    const IGravityField* m_gravity{nullptr};
    float m_planetRadiusKm{0.f}; // sent in MsgConnectAck (km); initialized to 6371 in constructor

    // Per-entity terrain height query (sim-thread only). When set, called each tick per entity instead
    // of the global m_groundElevation scalar.
    std::function<float(double, double)> m_groundQuery;

    // Network admin channel state (set before gameLoop.start(); read on sim thread only).
    std::string m_operatorPassword;                               // empty = admin channel disabled
    std::function<std::string(std::string_view)> m_adminDispatch; // null = admin channel disabled
    AuthTracker m_adminAuthTracker{5, 300}; // per-IP failed-auth lockout (defaults: 5 attempts, 5 min)

    // Deferred admin shell drain: one entry per in-flight MsgAdminCommand; fires after a 20 ms
    // wall-clock deadline (matching the RCON drain) so enqueueSimCallback lambdas have run and
    // shell output is available. Wall-clock is immune to GameLoop tick-batch catch-up.
    static constexpr int kENetAdminDrainDelayMs = 20;
    struct PendingAdminDrain {
        uint32_t peerId;
        uint16_t reqId;
        int shellMark;
        std::chrono::steady_clock::time_point drainDeadline;
    };
    std::vector<PendingAdminDrain> m_pendingAdminDrains;
    std::function<int()> m_adminShellMark;                          // null = drain disabled
    std::function<std::vector<std::string>(int)> m_adminShellDrain; // null = drain disabled

    SpatialIndex m_spatialIndex; // rebuilt at the start of each onTick; default 10 km cell size

    // Interest management + delta compression state (sim-thread only).
    double m_drawDistanceM{200'000.0};      // precomputed from drawDistanceKm × 1000; 200 km default
    uint64_t m_baselineIntervalTicks{120u}; // ticks between full-snapshot baselines for loss recovery

    // Per-peer entity known-gen set: peerId → (entityIdx → last-sent generation, truncated uint16).
    // Cleared at each baseline tick (forces full re-sync for all visible entities).
    // Erased in full on peer disconnect.
    std::unordered_map<uint32_t, std::unordered_map<uint32_t, uint16_t>> m_peerKnownGens;

    // Shutdown countdown state (sim-thread only).
    bool m_shuttingDown{false};
    std::chrono::steady_clock::time_point m_shutdownAt{};
    std::chrono::steady_clock::time_point m_nextNoticeAt{};
    uint32_t m_warningIntervalS{300};
    std::string m_shutdownReason;
    std::function<void()> m_shutdownCallback;
};

} // namespace fl
