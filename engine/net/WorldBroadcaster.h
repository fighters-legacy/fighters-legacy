// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "AuthTracker.h"
#include "CongestionController.h"
#include "GameProtocol.h"
#include "INetwork.h"
#include "InputTraceWriter.h"
#include "JitterBuffer.h"
#include "RequiredPackPolicy.h"
#include "SnapshotScheduler.h"
#include "TickGovernor.h"
#include "TransformHistory.h"          // lag-compensation rewind ring (#425)
#include "config/DifficultySettings.h" // AiScaling — sensing difficulty scaling (#685)
#include "entity/Collision.h"          // CollisionPair — entity-entity collision (#630)
#include "entity/DamageApplication.h"  // DamageRules — the gameplay damage gates (#626)
#include "entity/EntityEvent.h"        // IEntityEventHandler — kill attribution + scoring (#626)
#include "entity/EntityId.h"
#include "entity/SubsystemDamage.h" // SubsystemStateSet — per-subsystem damage (#675)
#include "flight/AeroForces.h"
#include "flight/IGravityField.h"
#include "loop/ISimUpdate.h"
#include "perf/TickProfiler.h"
#include "sensor/SensorSystem.h"
#include "spatial/SpatialIndex.h"
#include "weapon/FireControl.h"      // per-entity fire state + request emission (#625)
#include "weapon/ProjectileSystem.h" // the projectile pool (#625)
#include "world/FormationRegistry.h" // the formation / command tree (#610)

#include <glm/vec3.hpp> // glm::dvec3 (ground-elevation query — radial floor #477)

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fl {
class ILogger;
class EntityManager;
class FlightIntegrator; // full definition in WorldBroadcaster.cpp
class JobSystem;        // engine/job/JobSystem.h — full definition in WorldBroadcaster.cpp
struct EntityDef;       // engine/entity/EntityDef.h — the PayloadResolver's argument
struct EntityState;
struct EntityTransform;   // engine/entity/EntityState.h — spawnPilotEntity's transform
struct FlightModelData;   // engine/flight/FlightModelData.h
struct IEntityController; // engine/entity/IEntityController.h
class EntityTypeRegistry;
class WeatherController;
class FactionRegistry; // engine/world/FactionRegistry.h — coalition-aware hostility (#632)
} // namespace fl

namespace fl {

// Sentinel initial airspeed (#883): "no explicit speed given — pick a sane cruise default for an
// airborne spawn." A concrete value (incl. 0 for a ground start, #885) is used verbatim. Any
// negative value is treated as auto.
inline constexpr float kAutoSpawnAirspeed = -1.f;

// Parsed, validated client input stored per connected peer.
struct PeerInputState {
    // 8-byte fields first to avoid padding.
    uint64_t lastActivityTick{0}; // tick of last MsgClientInput or MsgHeartbeat; set in onConnect
    uint64_t lastInputTick{0};    // m_currentTick at last accepted MsgClientInput (inter-arrival jitter timing)
    uint64_t ackedTick{0};        // highest WorldSnapshot tick this peer has acknowledged (echoed in
                                  // MsgClientInput/MsgHeartbeat tickIndex; clamped to m_currentTick).
                                  // Drives client-acked delta baselines: an entity is sent full until
                                  // the client confirms it decoded the tick its full streak started on.
    // 4-byte fields next.
    uint32_t ackMask{0}; // selective-ack bitmask paired with ackedTick (#566); adopted together on a
                         // strict high-water advance. Bit b = decoded tick ackedTick-1-b (see AckWindow.h).
    float throttle{0.f}; // last drained value from jitterBuffer (effective input this tick)
    float elevator{0.f}; // last drained value
    float aileron{0.f};  // last drained value
    float rudder{0.f};   // last drained value
    float viewAxis[3]{1.f, 0.f, 0.f};
    float ewmaDelayTicks{0.f};       // EWMA of one-way delay in ticks (alpha = 1/jitterAdaptWindow)
    float ewmaJitterTicks{0.f};      // EWMA of inter-arrival jitter in ticks (RFC 3550 style)
    uint32_t lastSeqNum{0};          // seqNum of last accepted input
    uint32_t lastAppliedSeqNum{0};   // seqNum of the last input DRAINED from jitterBuffer + applied (#427)
    uint32_t estimatedDelayTicks{0}; // one-way delay in sim ticks (derived from tickIndex)
    // 1-byte fields last.
    uint8_t buttons{0};           // last drained value
    uint8_t selectedStation{255}; // last drained absolute station selection (#625); 255 = none
    bool hasSeq{false};           // false until first input received from this peer
    bool hasAppliedSeq{false};    // false until the first input is drained + applied (#427 TLV gate)
    bool ewmaSeeded{false};       // false until EWMA receives its first sample
    // Jitter buffer: initialized to depth 1; sized from estimatedDelayTicks on first input,
    // then continuously adjusted by the adaptive resize loop in WorldBroadcaster::onTick.
    JitterBuffer jitterBuffer{1};
    // Adaptive send-rate / congestion response (#518). Updated each tick from ENet link stats; gates
    // both the per-peer snapshot send cadence and the effective byte budget.
    CongestionController congestion{};
    uint64_t lastSnapshotSentTick{0}; // tick of the last snapshot actually sent (decimation gate)
    bool sentSnapshot{false};         // false until the first snapshot is sent (so tick 0 isn't skipped)

    // Wingman/flight order channel (#610). Lives here because this struct is already per-peer and is
    // already erased on disconnect, so the rate-limit window has no lifetime of its own to manage.
    std::chrono::steady_clock::time_point wingmanCmdWindowStart{}; // 1 s rate-limit window
    uint32_t wingmanCmdCount{0};                                   // orders seen in the current window
    uint32_t lastWingmanSeq{0};                                    // dup/reorder guard
    bool hasWingmanSeq{false};         // false until the first order (a reconnect restarts the counter)
    bool wingmanRateLimitAcked{false}; // one RateLimited ack per window, never one per packet

    // Connect handshake (#853/#857). Set when MsgConnectRequest is processed. Before that a connected
    // peer has an input slot (so idle-timeout covers it) but no entity, no role, and no snapshot
    // delivery -- it is not admitted until it sends a request.
    PeerRole role{PeerRole::Pilot};
    bool handshakeComplete{false}; // false until MsgConnectRequest processed; guards duplicate requests
    // Interest center for an ENTITY-LESS observer (#857): a pilot centers interest on its aircraft, an
    // observer (or a dead peer) on this point. Seeded at admit time from the spawn/last-aircraft
    // position, then driven by the client's camera eye each frame (#858, set in onReceive from
    // MsgClientInput::cameraEye). Unused for a pilot, whose aircraft transform wins in the gather.
    glm::dvec3 interestCenter{0.0, 0.0, 0.0};
};

// Snapshot of a connected peer's state, delivered by forEachPeer. The struct form makes future
// additions (e.g. per-peer spectate target, client-version string) trivially backward-compatible
// compared to a positional function-pointer callback.
struct PeerInfo {
    uint32_t peerId{};
    EntityId eid{};
    std::string addr;
    uint32_t delayTicks{};      // last estimatedDelayTicks (raw one-way delay measurement)
    uint32_t queueDepth{};      // current jitter buffer fill (inputs waiting to be drained)
    uint32_t bufferMaxDepth{};  // current jitter buffer max depth (set by adaptive resize)
    float ewmaDelayTicks{};     // EWMA of one-way delay; drives adaptive depth targeting
    float ewmaJitterTicks{};    // EWMA of inter-arrival jitter; scales depth via jitterMultiplier
    float sendRateHz{};         // current adaptive snapshot send rate (60 / congestion send interval)
    uint32_t effectiveBudget{}; // current congestion-scaled per-snapshot byte budget (0 = unlimited)
    float packetLoss{};         // last sampled ENet mean loss fraction (0..1)
};

// One simulated entity together with its control source. The registry is EntityId-keyed (not peer-
// keyed) so peers, AI, and scripted entities are all stepped uniformly in onTick. unique_ptr members
// hold incomplete types here; WorldBroadcaster's destructor is defined in the .cpp where both are
// complete.
struct ControlledEntity {
    EntityId id;
    std::unique_ptr<FlightIntegrator> sim;
    std::unique_ptr<IEntityController> controller;
    bool decimatable{false};        // AI/scripted entity whose sample() may be skipped under overrun; players never
    ControlInput lastInput{};       // last sampled control input, reused on a decimated (skipped) AI tick
    bool lastInputValid{false};     // false until the first sample() — forces a sample on the entity's first tick
    PayloadEffect payload{};        // what the CURRENT loadout costs this airframe; starts at the #812
                                    // default and shrinks as stores release (#625)
    FireState fire{};               // stations, ammo, edge/rate state (#625); empty when no registry/hardpoints
    SubsystemStateSet subsystems{}; // per-subsystem damage pools (#675); hasSubsystems gates its use
    bool hasSubsystems{false};      // true when the entity def declares [damage.subsystems]
    float fuelLeakKgS{0.f};         // accumulated fuel-leak rate from failed fuel subsystem(s) (#675)
};

// Pre-start scalar configuration. Bundles the init-time setters so callers configure rate limiting,
// the per-IP cap, admin-auth lockout, MOTD, and the operator password in one applyConfig() call
// instead of remembering six separate "call before gameLoop.start()" setters. The hot-reload setters
// (setMotd, setBannedAddresses, setAllowedAddresses, ...) remain available for runtime changes.
struct WorldBroadcasterConfig {
    int connectRateLimit{5};                              // max connects per window per IP
    int connectRateWindowS{10};                           // sliding-window length (seconds)
    int floodMultiplier{3};                               // MsgClientInput flood threshold multiplier
    int maxConnectionsPerIp{0};                           // simultaneous connections per IP; 0 = unlimited
    int adminAuthMaxFailures{5};                          // wrong operator passwords before per-IP lockout
    int adminAuthLockoutSeconds{300};                     // lockout duration (seconds)
    std::string motd;                                     // empty = no MOTD
    uint16_t motdDisplaySeconds{0};                       // 0 = client default
    std::string operatorPassword;                         // empty = network admin channel disabled
    std::string playerEntityType{"builtin:debug-entity"}; // pilot spawn default when client requests none (#834)
    bool allowObservers{true};                            // #857: false = refuse observer connect requests
    std::vector<RequiredPack> requiredPacks;              // #872: packs a client must have (id + optional version)
    RequiredPackPolicy requiredPackPolicy{RequiredPackPolicy::Warn}; // #872: warn / refuse / allow-placeholder
    int idleTimeoutS{0};              // 0 = disabled; seconds of peer inactivity before disconnect
    float drawDistanceKm{200.f};      // per-peer interest radius; 0 = degenerate (empty snapshots)
    double spatialCellSizeM{10000.0}; // SpatialIndex cell size (m); 0 = auto from draw distance; restart-only
    uint32_t snapshotBudgetBytes{0};  // per-client snapshot byte budget; 0 = unlimited (#516)
    bool compressSnapshots{false};    // zstd snapshot payload compression (#775); internal default
                                      // OFF (byte-stable tests), fl-server config default ON
    uint32_t jitterBufferMaxDepth{4}; // per-peer input queue depth; [1, JitterBuffer::kHardMaxDepth]
    uint32_t jitterAdaptWindow{60};   // EWMA smoothing window in ticks; alpha = 1/window; [10, 3600]
    uint32_t jitterHysteresis{2};     // dead-band in ticks before resize fires; [0, 8]
    float jitterMultiplier{2.0f};     // k factor: depth = ceil(ewma_delay + k*jitter); [0.0, 8.0]
    CongestionParams congestion{};    // per-client adaptive send-rate / congestion response (#518)
    TickGovernorParams governor{};    // graceful tick-overrun governor (#514)
    DamageRules gameplay{};           // friendly-fire / crash-damage gates (#626); hot-reloadable
};

// Snapshot of the overrun governor's current degradation state — read cross-thread (fl-server main
// thread) via getOverrunStatus(); backed by relaxed atomics published each tick by the sim thread.
struct OverrunStatus {
    float loadFactor{1.f};             // [floor, 1]; 1 = no degradation
    uint32_t snapshotIntervalTicks{1}; // server-wide snapshot send spacing
    uint32_t aiStride{1};              // AI sample() decimation stride
    float interestScale{1.f};          // interest-radius scale [min_interest_fraction, 1] (#726)
    bool degraded{false};              // loadFactor < 1
};

// Run-long watermarks of the per-peer adaptive send-rate controller (#518), read cross-thread via
// getCongestionTelemetry() (relaxed atomics published each tick by the sim thread). Designed for the
// synthetic congestion gate (#714), which reads a single --metrics-json snapshot at run end:
//   minSendHz         — all-time minimum across peers of the adaptive snapshot send rate. 60 = the
//                       controller never engaged over the run.
//   recoveredSendHz   — maximum send rate observed SINCE minSendHz was last lowered (reset to the
//                       current rate whenever a new minimum is set). Climbs back toward 60 when the
//                       controller recovers after the link clears.
//   maxPacketLoss     — all-time maximum sampled ENet mean loss fraction across peers (diagnostic).
// All three freeze while no peers are connected, so the trailing metrics writes after the load
// clients disconnect don't wipe the evidence the gate asserts on.
struct CongestionTelemetry {
    float minSendHz{60.f};
    float recoveredSendHz{60.f};
    float maxPacketLoss{0.f};
};

// Host-wide WIRE traffic sampled from the transport (#772), read cross-thread via getWireTelemetry()
// (relaxed atomics published by the sim thread). These are bytes actually on the socket — including
// transport framing and, on GNS, AES-GCM overhead — as distinct from the application snapshot
// payload the scale gate baselines (`downstream_kbs_per_client`), which is transport-independent by
// construction and therefore blind to what a transport costs to run. Zero on backends that don't
// report wire traffic.
//
// Sampled at FULL LOAD and held: the published sample is the one taken at the highest peer count
// seen so far (refreshed on ties, so it settles on steady state). fl-server keeps rewriting
// --metrics-json while the swarm ramps up and drains away, and the gate reads only the final
// snapshot — so a plain "latest sample" reports the connect ramp or the disconnect drain (measured:
// a 16-client run reported its wire rate at 2 peers), and an unguarded rate reports an idle server.
// `peersAtSample` travels with the rates so the per-client figure divides by the peers that actually
// produced the traffic. Same class of trap the #714 congestion watermarks freeze to avoid.
struct WireTelemetry {
    double outKbs{0.0}; // host egress, KB/s
    double inKbs{0.0};  // host ingress, KB/s
    double outPacketsPerSec{0.0};
    int peersAtSample{0}; // peers connected when the sample was taken (see the freeze note above)
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
class WorldBroadcaster : public ISimUpdate, public INetworkEventHandler, public IEntityEventHandler {
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

    // IEntityEventHandler (#626) — the first production consumer of entity events. fl-server
    // registers the broadcaster with EntityManager::addEventHandler() before gameLoop.start().
    // Died/ScoreAwarded feed the per-peer scoreboard and queue the reliable kill-feed broadcast;
    // DamageLevelChanged applies the entity's DamageDef penalties to its flight integrator (this is
    // where thrustFactor/controlFactor/avionicsFailure finally act). Fires on the sim thread, from
    // inside EntityManager calls that already run serially in onTick.
    void onEntityEvent(const EntityEvent& event) override;

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
                            std::shared_ptr<const FlightModelData> model = nullptr,
                            float initialAirspeed = kAutoSpawnAirspeed);

    // Override a controlled entity's loadout from a mission's per-object `loadout:` (#855). Rebuilds the
    // live stations from `stores` (each replaces one station's default, respecting the station's allowed
    // list; see buildLoadoutOverride) and re-costs the airframe's payload mass/drag. Must be called AFTER
    // the entity has a controller (registerController / a mission slot), so its ControlledEntity exists.
    // Returns false (with no change) when the entity has no controller or no weapon registry is set;
    // per-store problems append to `warnings`. Sim-thread / pre-start.
    bool setEntityLoadout(EntityId id, const std::vector<std::string>& stores, std::vector<std::string>& warnings);

    // Install a per-tick hook run at the END of onTick, after the world has stepped (#633). fl-server
    // wires it to the mission objective/trigger evaluator (MissionRuntime::step), keeping engine-net
    // free of an engine-mission dependency — GameLoop drives exactly one ISimUpdate, so this is the
    // clean seam for a second sim-side consumer. Called with the current tick index; sim-thread only.
    // Call before gameLoop.start().
    void setMissionTickHook(std::function<void(uint64_t)> hook) {
        m_missionTickHook = std::move(hook);
    }

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

    // Iterate all connected peers. fn receives a PeerInfo snapshot for each peer; the address
    // string is copied per entry — safe despite INetwork::getPeerAddress() returning a single
    // overwrite buffer. Sim-thread only (called from enqueueSimCallback or onTick context).
    void forEachPeer(std::function<void(const PeerInfo&)> fn) const;

    // Replace the entire in-memory ban set. Safe to call before gameLoop.start().
    void setBannedAddresses(std::unordered_set<std::string> addrs);

    // Set the allowlist. Empty set = allowlist disabled (all IPs permitted).
    void setAllowedAddresses(std::unordered_set<std::string> addrs);

    // Return a copy of the current ban set (called from sim thread to save to file).
    std::unordered_set<std::string> getBannedAddresses() const;

    // Snapshot of admin auth lockout state — sim-thread-only read (acceptable monitoring
    // race, same pattern as getBannedAddresses() / liveCount()).
    AuthLockoutSummary getAuthLockoutSummary() const;

    // Snapshot of the rolling per-phase tick budget (integrate / ai / collision / serialize /
    // total). Thread-safe (mutex-guarded inside TickProfiler) — safe to call from any thread;
    // read by the fl-server admin `status`/`tickstats` commands and the --metrics-json writer.
    TickBudget getTickBudget() const {
        return m_tickProfiler.snapshot();
    }

    // Current overrun-governor degradation state. Thread-safe (relaxed-atomic reads of values the sim
    // thread publishes each tick) — read by the fl-server `status`/`tickstats` commands and the
    // --metrics-json writer from the main thread.
    OverrunStatus getOverrunStatus() const noexcept {
        OverrunStatus s;
        s.loadFactor = m_overrunLoadFactor.load(std::memory_order_relaxed);
        s.snapshotIntervalTicks = m_overrunSnapInterval.load(std::memory_order_relaxed);
        s.aiStride = m_overrunAiStride.load(std::memory_order_relaxed);
        s.interestScale = m_overrunInterestScale.load(std::memory_order_relaxed);
        s.degraded = s.loadFactor < 1.f;
        return s;
    }

    // Run-long congestion-controller watermarks (#714). Thread-safe (relaxed-atomic reads of values
    // the sim thread publishes each tick) — read by the --metrics-json writer from the main thread.
    CongestionTelemetry getCongestionTelemetry() const noexcept {
        CongestionTelemetry t;
        t.minSendHz = m_congMinSendHz.load(std::memory_order_relaxed);
        t.recoveredSendHz = m_congRecoveredSendHz.load(std::memory_order_relaxed);
        t.maxPacketLoss = m_congMaxLoss.load(std::memory_order_relaxed);
        return t;
    }

    // Host-wide wire traffic (#772). Thread-safe (relaxed-atomic reads of values the sim thread
    // publishes) — read by the --metrics-json writer from the main thread.
    WireTelemetry getWireTelemetry() const noexcept {
        WireTelemetry t;
        t.outKbs = m_wireOutKbs.load(std::memory_order_relaxed);
        t.inKbs = m_wireInKbs.load(std::memory_order_relaxed);
        t.outPacketsPerSec = m_wireOutPps.load(std::memory_order_relaxed);
        t.peersAtSample = m_wirePeersAtSample.load(std::memory_order_relaxed);
        return t;
    }

    // Set the terrain floor elevation (m) used for ground collision in each peer's
    // FlightIntegrator. Thread-safe; may be called from any thread. Serves as a global
    // fallback when no per-entity query function is set via setGroundElevationQuery().
    void setGroundElevation(float elev) noexcept {
        m_groundElevation.store(elev, std::memory_order_relaxed);
    }

    // Inject a per-entity terrain height query called on the sim thread each tick.
    // fn(worldPos) → terrain elevation (m) ABOVE THE DATUM along the radial through worldPos
    // (i.e. TerrainStreamer::heightAt(dvec3)); FlightIntegrator compares it against the entity's
    // geodetic altitude for radial ground contact (#477). When set, this overrides the global
    // m_groundElevation scalar for each entity's FlightIntegrator::step() call.
    // Requires TerrainStreamer::heightAt() to be thread-safe (shared_mutex). Call before
    // gameLoop.start().
    void setGroundElevationQuery(std::function<float(glm::dvec3)> fn);

    // Set pre-cached peer spawn positions [x, y, z] in world space.
    // y must already include the terrain height + AGL offset, computed on the main thread
    // before gameLoop.start(). Positions are assigned round-robin to connecting peers.
    // Empty list = legacy behaviour: spawn at origin with y = m_groundElevation + 500 m.
    // Call before gameLoop.start(); never mutated after that.
    void setSpawnPoints(std::vector<std::array<double, 3>> points) noexcept;

    // A joinable mission player slot (#854): a mission object marked `player: true`. A POD so engine-net
    // stays free of an engine-mission dependency — fl-server translates engine-mission's PlayerSlot into
    // this. `factionIndex` indexes the FactionRegistry handed to setFactionRegistry; `quat` is the
    // resolved spawn orientation (heading already placed on the local tangent frame at spawn time).
    struct MissionSpawnSlot {
        std::string missionObjectId; // the mission object id this slot came from (#884), reported to
                                     // the mission-slot binder so destroy(<id>) tracks the pilot
        std::string entityType;
        uint16_t factionIndex{0};
        double pos[3]{};
        float quat[4]{0.f, 0.f, 0.f, 1.f};
        float airspeed{kAutoSpawnAirspeed}; // initial airspeed for the joining pilot (#883); auto = cruise
    };

    // Notified when a pilot claims/leaves a mission player slot (#884): (missionObjectId, entity). A
    // VALID entity = the pilot's aircraft now occupies the slot; an INVALID one = the slot was freed
    // (pilot disconnected). fl-server wires this to MissionRuntime::registerObjectEntity so the objective
    // evaluator's destroy(<slot-id>) tracks the live aircraft instead of firing from t=0. Sim-thread.
    using MissionSlotBinder = std::function<void(const std::string& missionObjectId, EntityId entity)>;
    void setMissionSlotBinder(MissionSlotBinder fn) {
        m_missionSlotBinder = std::move(fn);
    }

    // Install the mission's player slots. When non-empty, a connecting pilot is assigned the next open
    // slot (its type/faction/spawn) instead of the round-robin setSpawnPoints() + [world] player_faction
    // path; the slot frees on disconnect. All slots occupied ⇒ the pilot falls back to the default path,
    // so extra players still get an aircraft. Empty (the default) = pre-mission behavior. Resets slot
    // occupancy; call before gameLoop.start().
    void setMissionPlayerSlots(std::vector<MissionSpawnSlot> slots);

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

    // Resolves an EntityDef::flightModelAsset to a parsed flight model for the spawn path. Injected as a
    // std::function so engine-net stays free of engine-content/engine-flight asset deps (the parse
    // lives in fl-server, which links both). Returns nullptr when the id is unknown; an empty
    // flightModelAsset or an unset resolver falls back to the builtin UFO model. Call before
    // gameLoop.start().
    using FlightModelResolver = std::function<std::shared_ptr<const FlightModelData>(const std::string& id)>;
    void setFlightModelResolver(FlightModelResolver fn);

    // Resolves an entity type's DEFAULT loadout to the mass and drag it costs the airframe (#812).
    // Same std::function injection as the flight-model resolver, and for the same reason: the
    // summation lives in engine-weapon (fl::defaultPayload), and engine-net must not link it.
    //
    // Resolved ONCE per controlled entity at spawn and cached on the ControlledEntity -- a loadout
    // does not change mid-flight (rearm/jettison is #583). Unset => every entity flies clean, which
    // is exactly the pre-#812 behaviour.
    using PayloadResolver = std::function<PayloadEffect(const EntityDef& def)>;
    void setPayloadResolver(PayloadResolver fn);

    // ---------------------------------------------------------------------------------------------
    // Sensing (#685)
    // ---------------------------------------------------------------------------------------------
    // Resolves a sensor-def id to a parsed def, the setFlightModelResolver pattern (engine-net must
    // not link engine-content). Unset ⇒ every observer falls back to the builtin eyeball, which is
    // the honest default and is what the zero-content sandbox runs on. Call before gameLoop.start().
    void setSensorDefResolver(sensor::SensorSystem::SensorDefResolver fn);

    // Geometry checks per second (default 10 = the reference cadence every authored `pod` is tuned
    // against). Converted to a tick stride; checks are staggered across it. Changing this changes
    // effective acquisition time — that is the honest consequence, and it is documented rather than
    // silently renormalized. [1, 60]; atomic, hot-reloadable.
    void setSensorCheckHz(float hz) noexcept;

    // The EMCON / RWR seam: a non-emitting observer cannot hold a radar or laser TRACK lobe. Nothing
    // flips it yet (#526/#529 do); sim-thread only.
    void setEmitting(uint32_t entityIdx, bool emitting);

    // What this entity has honestly detected. Null = it has no sensors (or sensing has not run for
    // it yet) — which a consumer must read as "not evaluated", never as "sees nothing".
    [[nodiscard]] const sensor::ContactTable* contactsFor(uint32_t entityIdx) const;

    // Difficulty scaling for sensing (radar range fraction, reaction time). Unset = NO scaling:
    // radar reaches its authored range and the AI reacts the moment it detects. Deliberately not
    // defaulted to AiScaling{} — those defaults are the Cadet preset, and silently halving every
    // radar range on a server that never configured a difficulty would be a lie. #682 wires it.
    void setAiScaling(const AiScaling& scaling) noexcept;

    // ---------------------------------------------------------------------------------------------
    // Formations and the wingman command channel (#610)
    // ---------------------------------------------------------------------------------------------
    // engine-net must never link engine-ai (cmake/layering.cmake enforces the module boundary), so
    // the two places where an order needs to *build a controller* are std::function hooks filled in
    // by fl-server — the same injection pattern as setFlightModelResolver above. It also keeps
    // test_world_broadcaster free of engine-ai, which matters because that test is in the TSan set.

    // Faction stamped onto every player entity at spawn. MUST be non-zero for any threat logic to
    // work at all: fl::areFactionsHostile treats faction 0 as neutral, i.e. an entity with no
    // enemies — so with the legacy default of 0, nothing in the world is hostile to a player, the
    // wingman's engage/cover conditions can never fire, and boresight designation can never
    // designate. Default 1. Call before gameLoop.start().
    void setPlayerFaction(uint16_t faction) noexcept;
    [[nodiscard]] uint16_t playerFaction() const noexcept {
        return m_playerFaction;
    }

    // Called on the sim thread at the end of onConnect, after the peer's entity and controller exist.
    // The implementation spawns the peer's flight (N AI members), registers their controllers, and
    // returns the formation it created. kNoFormation (the default with no hook installed) = the peer
    // flies alone, which is exactly today's behavior.
    using FlightSpawner = std::function<fl::FormationId(uint32_t peerId, EntityId leadEntity)>;
    void setFlightSpawner(FlightSpawner fn);

    // Called on the sim thread once per addressed, live, AI member of a formation. Builds the new
    // controller for `cmd` and registers it (registerController REPLACES the existing one).
    // `designatedTarget` is resolved by the server (boresight, ai/Threat.h) before this is called;
    // an invalid target on an attack order means the order was already refused.
    // Returns false if the controller could not be built, which the caller reports as Rejected.
    using FlightOrderHandler = std::function<bool(const fl::Formation& formation, const fl::FormationMember& member,
                                                  uint8_t command, EntityId designatedTarget)>;
    void setFlightOrderHandler(FlightOrderHandler fn);

    // Resolves the target for `attack_my_target` from the commander's own state. Injected because it
    // lives in engine-ai (ai::designateBoresightTarget). Unset = attack orders always refuse with
    // NoTarget, which is the correct degradation: never invent a target.
    // Args: the commanding entity, and its last-known look axis (PeerInputState::viewAxis).
    using TargetDesignator = std::function<EntityId(const EntityState& commander, const float viewAxis[3])>;
    void setTargetDesignator(TargetDesignator fn);

    // Max wingman orders a peer may issue per second before the excess is refused with RateLimited.
    // Acked once per window, never per packet — an ack per rejected packet would be an amplifier.
    void setFlightCommandRateLimit(int perSecond) noexcept;

    // Apply an order to a formation WITHOUT a peer-authority check — the game-master path, used by
    // the `flight order` admin command. The console is authorized by the operator password, so it
    // does not have (or need) a commander role, and it must not pretend to be peer 0 to get one:
    // authority bypasses here are explicit and visible rather than forged.
    //
    // The GM has no boresight, so `attack_my_target` cannot designate through this path — the caller
    // should point an AI at a specific entity with `spawn --ai pursuit` instead. Relayed orders to
    // human members still go out (a GM CAN radio a player).
    // Returns the number of AI members actually retasked. Sim-thread only.
    struct FlightOrderReport {
        int aiRetasked{0};
        int humansRelayed{0};
        int deadSkipped{0};
    };
    FlightOrderReport applyFlightOrder(fl::FormationId fid, uint8_t command, uint32_t memberIdx, bool cascade,
                                       EntityId designatedTarget = {});

    // The formation tree. Sim-thread only. fl-server reaches it via enqueueSimCallback to build
    // AI-only formations and to serve the `flight` admin command family (the game-master and AWACS
    // surface); WorldBroadcaster itself uses it to authorize and dispatch MsgWingmanCommand.
    [[nodiscard]] fl::FormationRegistry& formations() noexcept {
        return m_formations;
    }
    [[nodiscard]] const fl::FormationRegistry& formations() const noexcept {
        return m_formations;
    }

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

    // Enable/disable server-side input tracing (#560). When `dir` is non-empty, each connected
    // peer's accepted MsgClientInput is appended to a per-peer FLIT trace in `dir` (created if
    // needed); an empty string disables tracing and closes all open trace files. Sim-thread only:
    // call before gameLoop.start() or via enqueueSimCallback (the trace_start/trace_stop admin
    // commands). Switching directories reopens fresh per-peer files on the next accepted input.
    void setInputTraceDir(std::string dir);

    // Set the per-peer draw distance for snapshot interest management. Only entities within this
    // radius of a peer's own entity position are included in that peer's MsgWorldSnapshot.
    // 0 km = degenerate (queryRadius finds nothing; peers see empty snapshots). Default = 200 km.
    // Call before gameLoop.start() or via enqueueSimCallback for hot-reload (reload_config).
    void setDrawDistance(float km) noexcept;

    // Set the SpatialIndex cell size (metres) used for per-peer interest queries + AI range queries.
    // cellSizeM <= 0 selects an auto heuristic derived from the current draw distance (so a query
    // spans a bounded number of cells rather than degenerating toward O(N) at high density). This
    // reassigns the index, so it is RESTART-ONLY: call before gameLoop.start() (applyConfig does),
    // never via reload_config. Depends on m_drawDistanceM, so call after setDrawDistance().
    void setSpatialCellSize(double cellSizeM);

    // Set the per-client snapshot byte budget (#516). 0 = unlimited (legacy: send every visible
    // entity). When non-zero, each peer's snapshot is capped at roughly this many bytes; the scheduler
    // ranks visible entities by relevance (distance/threat/recency) and sends the highest-priority set
    // that fits, deferring the rest to later ticks. Atomic / hot-reloadable; call before
    // gameLoop.start() or via enqueueSimCallback (reload_config).
    void setSnapshotBudget(uint32_t bytes) noexcept;

    // Enable zstd compression of snapshot payloads (#775). The per-peer payload after the raw
    // 24-byte header is compressed in the parallel build pass when it wins (strictly smaller;
    // payloads under kMinSnapshotCompressBytes are sent raw), signalled via
    // MsgWorldSnapshotHeader::flags + uncompressedBytes. Internal default OFF so the broadcast
    // byte-shape tests stay stable; the fl-server config default is ON ([network]
    // compress_snapshots). Atomic / hot-reloadable like setSnapshotBudget.
    void setSnapshotCompression(bool enabled) noexcept;

    // Set the global maximum jitter buffer depth (ticks). The actual per-peer initial depth is
    // min(estimatedDelayTicks, maxDepth), floored at 1. The adaptive resize loop in onTick
    // continuously adjusts per-peer depths within this bound. Thread-safe; may be called before
    // gameLoop.start() or via enqueueSimCallback.
    void setJitterBufferDepth(uint32_t maxDepth) noexcept;

    // Set the EWMA smoothing window (ticks) for adaptive jitter buffer resizing.
    // alpha = 1/adaptWindow; larger values = slower adaptation. Range [1, 3600].
    // Call before gameLoop.start() or via enqueueSimCallback.
    void setJitterAdaptWindow(uint32_t ticks) noexcept;

    // Set the dead-band (ticks) for adaptive resize: resize fires only when
    // |target_depth - current_depth| > hysteresis. Range [0, 8].
    // Call before gameLoop.start() or via enqueueSimCallback.
    void setJitterHysteresis(uint32_t ticks) noexcept;

    // Set the jitter confidence multiplier k in: depth = ceil(ewma_delay + k * jitter_ewma).
    // 0.0 = delay-only sizing (pure EWMA, no jitter term). Range [0.0, 8.0].
    // Call before gameLoop.start() or via enqueueSimCallback.
    void setJitterMultiplier(float k) noexcept;

    // Set the per-client adaptive send-rate / congestion-response parameters (#518). Applied to every
    // peer's controller each tick, so this is hot-reloadable (reload_config). Disabled params pin all
    // peers to the full 60 Hz / full-budget behaviour. Call before gameLoop.start() or via
    // enqueueSimCallback.
    // Gameplay damage gates (#626): friendly fire + crash damage. Sim-thread-only state — call
    // before gameLoop.start(), or via enqueueSimCallback for reload_config hot-reload.
    void setDamageRules(const DamageRules& rules) noexcept {
        m_damageRules = rules;
    }

    // The flight integrator driving a controlled entity, or nullptr. Sim-thread only. Consumers:
    // tests asserting damage penalties actually landed (#626) and the fire path's launch-state
    // reads (#625) — never a back door for controllers, which see the world through AiTickContext.
    [[nodiscard]] const FlightIntegrator* integratorFor(uint32_t entityIdx) const noexcept;

    // The weapon vocabulary (#625). Main-thread, before gameLoop.start(); the registry outlives
    // the broadcaster (fl-server main-scope, like the type registry). Null = the fire path is off:
    // trigger intent is read and discarded, exactly the pre-#583 behavior. Also arms the
    // projectile pool with the registry + the current gravity field.
    void setWeaponRegistry(const WeaponRegistry* weapons) noexcept;

    // Detonate a warhead at a world position (#356): blast damage with linear falloff through
    // applyPointDamage (so the friendly-fire gate holds inside a blast), and — when nuclear — the
    // EMP ring wired to SensorSystem::setAvionicsFailed. Every warhead consumer (proximity fuzes,
    // bomb impacts, the `detonate` admin command) goes through here. Sim-thread only; call via
    // enqueueSimCallback from anywhere else. Note the spatial index is rebuilt at the top of each
    // onTick, so a detonation between ticks sees the previous tick's positions — one tick of
    // staleness, the same view every other consumer of the index gets.
    WarheadResult applyWarheadAt(const double pos[3], const BlastSpec& blast, EntityId instigator);

    void setCongestionParams(const CongestionParams& params) noexcept;

    // Set the graceful tick-overrun governor parameters (#514). Applied to the governor each tick, so
    // this is hot-reloadable (reload_config). Disabled params pin loadFactor to 1 (no degradation —
    // exact pre-#514 behaviour). Call before gameLoop.start() or via enqueueSimCallback.
    void setGovernorParams(const TickGovernorParams& params) noexcept;

    // Set the gravity field applied to all FlightIntegrators spawned on this broadcaster (current
    // and future). Also records the planet radius sent to clients in MsgConnectAck so their terrain
    // rendering matches server physics. Defaults to CentralGravityField::earthInstance() /
    // 6371 km; only call this for non-Earth planets. Call before gameLoop.start().
    void setGravityField(const IGravityField& field, float planetRadiusKm = 6371.f) noexcept;

    // Earth-rotation rate Ω (rad/s) applied to every FlightIntegrator spawned here (current and
    // future) — the Coriolis + centrifugal terms of an Earth-fixed rotating world frame (#482).
    // Default 0 = an inertial frame (all existing WB tests stay bit-identical); fl-server sets
    // kEarthRotationRate from `[world] earth_rotation`. Call before gameLoop.start(); the value is
    // read at each addControlledEntity, so it must be set before peers spawn.
    void setEarthRotationRate(double omega_rad_s) noexcept {
        m_earthRotationRate = (omega_rad_s > 0.0) ? omega_rad_s : 0.0;
    }

    // Inject the coalition registry that resolves hostility for AI controllers (#632). Passed into the
    // AiTickContext each tick, so a scripted mission bot honors mission-declared alliances instead of
    // the crude "distinct non-zero faction = hostile" affiliation rule. nullptr (the default) keeps
    // that pre-mission affiliation behavior — the hostile() fallback (FactionRegistry.h). The registry
    // is owned by the caller (fl-server, for the server lifetime; wired at mission load in #854) and
    // must outlive this broadcaster. Call before gameLoop.start().
    void setFactionRegistry(const FactionRegistry* registry) noexcept {
        m_factionRegistry = registry;
    }

    // Inject the data-parallel job system used to parallelise the per-entity AI + integrate passes
    // in onTick. nullptr (the default) runs both passes inline on the sim thread — keeps unit tests
    // thread-free and gives a serial-equivalent result. The JobSystem must outlive this broadcaster
    // (and the GameLoop sim thread). Call before gameLoop.start().
    void setJobSystem(JobSystem& jobs) noexcept {
        m_jobs = &jobs;
    }

  public:
    // Change a connected peer's role mid-session without a reconnect (#857). pilot->observer despawns
    // the peer's entity; observer->pilot spawns one and re-sends MsgConnectAck so the client learns its
    // new assigned entity + role. No-op if the peer is unknown, not yet admitted, or already in the
    // target role. Sim-thread only (call via GameLoop::enqueueSimCallback from admin/gameplay code);
    // shares the spawn/teardown mechanism with connect/disconnect — the same seam #648 (death ->
    // spectator -> respawn) reuses.
    void setPeerRole(uint32_t peerId, PeerRole role);

  private:
    // Handle MsgConnectRequest (#853): grant a role, admit the peer (pilot = spawn its entity + form its
    // flight; observer = no entity), reply MsgConnectAck, and send the MOTD. Replaces the old "server
    // spawns on connect" flow. Sim-thread only (called from onReceive).
    void handleConnectRequest(uint32_t peerId, const void* data, std::size_t size);
    // Spawn a pilot peer's entity of `entityType`, register its PeerController, stamp faction, and form
    // its flight. Returns the assigned EntityId (invalid on spawn failure). Extracted from the old
    // onConnect. Sim-thread.
    EntityId admitPilot(uint32_t peerId, const std::string& entityType);
    // Shared spawn core for a pilot peer: spawn `entityType` at `t`, record m_peerEntities, stamp
    // `faction` (0 = leave neutral), resolve the flight model, and register the PeerController. Used by
    // both admitPilot (round-robin path) and the mission-slot path. Sim-thread.
    EntityId spawnPilotEntity(uint32_t peerId, const std::string& entityType, const EntityTransform& t,
                              uint16_t faction, float initialAirspeed = kAutoSpawnAirspeed);
    // Claim the next open mission player slot for `peerId` (#854). Returns its index, or -1 when there
    // are no slots or all are occupied. releaseMissionSlot frees it on despawn. Sim-thread.
    int claimMissionSlot(uint32_t peerId);
    void releaseMissionSlot(uint32_t peerId);
    // Resolve the entity type to spawn for a pilot (#834): a client-requested type wins iff it is a
    // REGISTERED type (server-clamped allowlist); otherwise the [world] player_entity_type default;
    // otherwise builtin:debug-entity. An unregistered request falls back with an Info log.
    std::string resolvePlayerEntityType(const char* requested) const;
    // Tear down a peer's entity: its owned formation + AI members, its controller, sensor observer, and
    // the entity itself; erase from m_peerEntities. Does NOT touch m_peerInputs (the peer keeps its
    // slot). Shared by onDisconnect (and setPeerRole once #857 lands). Sim-thread.
    void despawnPeerEntity(uint32_t peerId);
    void sendConnectAck(uint32_t peerId, EntityId assigned, PeerRole grantedRole);
    void sendConnectRefusal(uint32_t peerId, ConnectRefusalCode code, const char* reason);
    // Send a complete admin command result over ENet. Short results (<=kAdminResponseFastPathMax
    // chars) go as a single MsgAdminResponse; longer results are streamed as MsgAdminResponseChunk
    // packets terminated by kChunkFlagEnd. reqId is echoed from the triggering MsgAdminCommand.
    static void sendAdminResponse(INetwork& net, uint32_t peerId, uint16_t reqId, const std::string& result);

    // Wingman/flight order path (#610). Sim-thread only (called from onReceive).
    void handleWingmanCommand(uint32_t peerId, const void* data, std::size_t size);
    // The shared dispatch core behind BOTH the network order path and the `flight order` admin
    // command, so a radio order and a console order cannot behave differently. callerPeerId kNoPeer
    // = the game master (no relay attribution to a player entity).
    FlightOrderReport dispatchOrder(fl::FormationId fid, uint8_t command, uint32_t memberIdx, bool cascade,
                                    EntityId designatedTarget, uint32_t callerPeerId, uint32_t callerEntityIdx);
    void sendWingmanAck(uint32_t peerId, uint8_t command, WingmanResult result, uint16_t flightId, uint8_t flightSize,
                        uint32_t memberIdx, uint32_t targetIdx);
    // Log, send a MsgConnectRefusal with the reason text for `code`, and disconnect the peer.
    // Centralizes the five onConnect rejection paths.
    void rejectConnection(uint32_t peerId, const std::string& ip, ConnectRefusalCode code);
    // Build a FlightIntegrator (from model, or builtin when null) reset to the entity's current
    // transform, and register it with the controller under the entity's index. Shared by onConnect
    // (PeerController) and registerController (AI/scripted). Sim-thread only.
    void addControlledEntity(EntityId id, std::unique_ptr<IEntityController> controller,
                             std::shared_ptr<const FlightModelData> model, float initialThrottle, bool decimatable,
                             float initialAirspeed = kAutoSpawnAirspeed);

    // Resolve an entity type's EntityDef::flightModelAsset via the injected resolver. Returns null when
    // the id is empty, no resolver is set, or the id is unknown (logs Warn) — callers fall back to
    // the builtin model.
    std::shared_ptr<const FlightModelData> resolveFlightModel(EntityId id);

    // Run a per-entity pass over [0, count): via the injected JobSystem (data-parallel) when set,
    // else inline on the sim thread. fn(begin, end) processes a contiguous index sub-range.
    void runEntityPass(std::size_t count, const std::function<void(std::size_t, std::size_t)>& fn);

    // Run a per-peer pass over [0, count): same dispatch as runEntityPass but with a finer grain
    // (each peer is a heavy, heterogeneous-cost unit — interest query + scheduler + bitstream encode).
    // Used to build per-peer snapshot buffers in parallel; the sim thread flushes them serially.
    void runPeerPass(std::size_t count, const std::function<void(std::size_t, std::size_t)>& fn);

    // Turbulence is seeded per (entityIdx, tickIndex) so the integrate step is deterministic and
    // parallel-safe — no shared RNG state mutated across entities.
    void stepFlightSim(FlightIntegrator& fi, EntityState& state, const ControlInput& ctrl, const PayloadEffect& payload,
                       double simDt, uint32_t entityIdx, uint64_t tickIndex);
    // After the integrate pass: cache the lowest-index live controlled entity's XZ for main-thread
    // terrain streaming + floor updates (only meaningful in single-player).
    void updateTerrainSteerCache();
    void broadcastShutdownNotice(uint16_t secsLeft, const char* text);
    static std::string makeShutdownMessage(uint32_t secsLeft, const std::string& reason = "");

    EntityManager& m_entityManager;
    EntityTypeRegistry& m_registry;
    INetwork& m_net;
    ILogger& m_logger;
    WeatherController* m_weather{nullptr};
    const FactionRegistry* m_factionRegistry{nullptr}; // coalition-aware hostility for AI (#632)
    std::function<void(uint64_t)> m_missionTickHook;   // mission objective evaluator, end of onTick (#633)

    std::unordered_map<uint32_t, EntityId> m_peerEntities;
    std::unordered_map<uint32_t, PeerInputState> m_peerInputs;

    // Formations and the wingman order path (#610). Sim-thread only, like every other roster here.
    fl::FormationRegistry m_formations;
    FlightSpawner m_flightSpawner;           // null = peers fly alone (today's behavior)
    FlightOrderHandler m_flightOrderHandler; // null = the order channel is off; orders are discarded
    TargetDesignator m_targetDesignator;     // null = attack orders always refuse (never invent a target)
    uint16_t m_playerFaction{1};             // 0 restores the legacy neutral-player behavior
    std::string m_playerEntityType{"builtin:debug-entity"}; // pilot spawn default when client requests none (#834)
    bool m_allowObservers{true};                            // #857: false = refuse observer connect requests
    std::vector<RequiredPack> m_requiredPacks;              // #872: packs a client must have (id + optional version)
    RequiredPackPolicy m_requiredPackPolicy{RequiredPackPolicy::Warn}; // #872: what to do when one is missing
    int m_flightCmdRateLimit{4};                                       // orders per second per peer
    // EntityId.index -> {sim, controller}. Replaces the old peerId-keyed flight-sim map: any control
    // source (peer, AI, script) registers here and is stepped uniformly in onTick.
    std::unordered_map<uint32_t, ControlledEntity> m_controlledEntities;

    // ── the fire path (#625) — sim-thread only ──────────────────────────────
    const WeaponRegistry* m_weaponRegistry{nullptr};
    ProjectileSystem m_projectileSystem;
    // The conditions the sensing pass ran under this tick; seeker checks (#627) read the same ones.
    sensor::SensingEnvironment m_sensingEnv{};
    // Rolling post-integrate position history (#425): player hitscan rewinds targets to the tick
    // the shooter actually saw (currentTick − estimatedDelayTicks, clamped to the ring depth).
    TransformHistory m_transformHistory;
    std::vector<FireRequest> m_fireRequests;     // scratch, cleared each tick
    std::vector<ProjectileImpact> m_tickImpacts; // scratch, cleared each tick
    uint8_t m_currentWeaponClass{0xFF};          // WeaponType ordinal for kill attribution while a
                                                 // weapon damage call is on the stack
    // Cosmetic effect events for this tick's snapshots (unreliable, interest-filtered per peer,
    // capped). Filled by the weapons pass, read-only in the parallel peer pass, cleared next tick.
    struct EffectRecord {
        uint8_t type{0}; // EffectType (GameProtocol.h)
        uint8_t weaponClass{0xFF};
        uint32_t srcIdx{0xFFFFFFFFu};
        uint32_t tgtIdx{0xFFFFFFFFu};
        float pos[3]{};
    };
    std::vector<EffectRecord> m_tickEffects;
    void runWeaponsPass(double simDt, uint64_t tickIndex);
    void executeFireRequest(const FireRequest& req, uint64_t tickIndex);
    void resolveHitscan(const FireRequest& req, const WeaponDef& def, uint64_t tickIndex);
    // The shooter's designated target through the #610 seam: peer viewAxis or AI nose (#627/#628).
    EntityId designateFor(const EntityState& shooter, uint32_t ownerPeer) const;
    void queueEffect(uint8_t type, uint8_t weaponClass, uint32_t srcIdx, uint32_t tgtIdx, const double pos[3]);

    // ── combat scoring + kill feed (#626) — sim-thread only ─────────────────
    struct PeerScore {
        uint32_t kills{0};
        uint32_t losses{0};
        int32_t score{0};
        bool dirty{false}; // a Stats record is owed to this peer in the next serialize
    };
    std::unordered_map<uint32_t, PeerScore> m_scores; // keyed by peerId; erased on disconnect
    std::vector<CombatEventRecord> m_pendingKillEvents;
    DamageRules m_damageRules{};

    // The owning peer of an entity, or kNoOwningPeer. Resolved against the LIVE peer map, never
    // against EntityState::ownerId — whose "0 = server/AI" convention collides with real peer 0
    // (the #610 kNoPeer lesson).
    [[nodiscard]] uint32_t peerIdForEntity(EntityId id) const noexcept;
    void flushCombatEvents();

    std::atomic<int> m_activePeerCount{0};
    uint64_t m_weatherBroadcastTick{0};        // throttle weather broadcasts to ~6 Hz
    uint64_t m_idleTimeoutTicks{0};            // 0 = disabled; pre-computed from idleTimeoutS × 60
    std::atomic<float> m_groundElevation{0.f}; // floor elevation passed to each FlightIntegrator::step

    // Data-parallel job system for the per-entity AI + integrate passes (nullptr = inline/serial).
    JobSystem* m_jobs{nullptr};
    // Per-tick scratch reused across ticks to avoid reallocation. m_stepItems gathers the live
    // controlled entities into a contiguous indexable range; m_stepInputs holds each one's sampled
    // control so the AI pass and integrate pass can be split (and parallelised).
    struct StepItem {
        uint32_t idx;
        ControlledEntity* ce;
        EntityState* state;
    };
    std::vector<StepItem> m_stepItems;
    std::vector<ControlInput> m_stepInputs;
    // Entity indices already warned about a flight-envelope departure (#891 speed_guard_clamped), so
    // the diagnostic logs once per entity instead of every tick a diverged entity stays pinned.
    std::unordered_set<uint32_t> m_envelopeWarned;
    std::atomic<double> m_entityX{0.0}; // last stepped entity world-X (sim writes; main reads)
    std::atomic<double> m_entityZ{0.0}; // last stepped entity world-Z

    // Entity-entity collision detection (#630). Gathered serially post-integrate; the detect pass is
    // data-parallel (each candidate writes only its own m_collisionScratch slot, reads the frozen
    // spatial index + candidate list), and the damage apply is serial (applyPointDamage fires event
    // handlers). Reused across ticks to avoid reallocation.
    struct CollisionCand {
        EntityId id;
        double pos[3];
        float vel[3];
        float radius;
    };
    std::vector<CollisionCand> m_collisionCands;
    std::unordered_map<uint32_t, uint32_t> m_collisionIdxToSlot; // entity index -> slot in m_collisionCands
    std::vector<std::vector<CollisionPair>> m_collisionScratch;  // per-candidate detected pairs
    std::vector<CollisionPair> m_collisionPairs;                 // flattened + sorted for serial apply
    void runCollisionPass(uint64_t tickIndex);

    // Route damage that already landed on `target` (via applyPointDamage) into a subsystem (#675).
    // `hitDirWorld` is the direction the damage travelled in WORLD space (rotated to body frame
    // here); null = undirected (a weight-only pick, e.g. a crash). No-op unless the target is a
    // controlled entity that declared [damage.subsystems]. Applies the failed subsystem's effect.
    void routeSubsystemDamage(EntityId target, float amount, const float* hitDirWorld, uint64_t tickIndex);
    void applySubsystemEffects(ControlledEntity& ce); // recompute integrator/sensor state from the mask

    std::vector<std::array<double, 3>> m_spawnPoints; // pre-cached [x,y,z]; sim-thread read-only after start
    uint32_t m_nextSpawnIdx{0};                       // round-robin counter; sim-thread only

    // Mission player slots (#854). m_slotOccupant[i] = the peer holding slot i, or kSlotFree. m_peerSlot
    // maps a peer to its held slot for O(1) release on despawn. Sim-thread only.
    static constexpr uint32_t kSlotFree = 0xFFFFFFFFu;
    std::vector<MissionSpawnSlot> m_missionSlots;
    std::vector<uint32_t> m_slotOccupant;
    std::unordered_map<uint32_t, int> m_peerSlot;
    MissionSlotBinder m_missionSlotBinder; // notified on slot claim/free for destroy(<id>) tracking (#884)

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

    // Resolves EntityDef::flightModelAsset -> FlightModelData at spawn (null = always builtin model).
    FlightModelResolver m_flightModelResolver;
    PayloadResolver m_payloadResolver;

    // Sensing (#685). The system owns the observer side-storage; EntityState stays a flat POD that
    // knows nothing about being observed.
    sensor::SensorSystem m_sensorSystem;
    std::atomic<float> m_sensorCheckHz{10.f};
    std::optional<AiScaling> m_aiScaling; // unset = no difficulty scaling (see setAiScaling)

    // Server-side input tracing (#560): while m_inputTraceDir is non-empty, each peer's accepted
    // (post-validation) MsgClientInput is appended to a per-peer FLIT trace. Sim-thread only.
    std::string m_inputTraceDir; // empty = tracing disabled
    std::unordered_map<uint32_t, std::unique_ptr<InputTraceWriter>> m_peerTraceWriters;
    uint32_t m_traceFileSeq{0}; // disambiguates trace filenames across reconnects/restarts

    // Gravity field applied to all spawned integrators. Initialized to CentralGravityField::earthInstance()
    // in the constructor; override with setGravityField() for non-Earth servers.
    const IGravityField* m_gravity{nullptr};
    float m_planetRadiusKm{0.f};     // sent in MsgConnectAck (km); initialized to 6371 in constructor
    double m_earthRotationRate{0.0}; // #482: Ω for integrator Coriolis/centrifugal; 0 = inertial frame

    // Per-entity terrain height query (sim-thread only). When set, called each tick per entity instead
    // of the global m_groundElevation scalar.
    std::function<float(glm::dvec3)> m_groundQuery;

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

    // Per-phase tick-budget instrumentation. Written on the sim thread in onTick (begin/end +
    // TickPhaseScope); snapshot()ed (mutex-guarded) by getTickBudget() from any thread.
    TickProfiler m_tickProfiler;

    // Graceful tick-overrun governor (#514/#726). Stepped once per onTick on the sim thread from the
    // prior tick's measured wall-time; its four lever values are frozen into sim-thread locals for the
    // parallel regions and mirrored into the atomics below so getOverrunStatus() is a safe cross-thread
    // read (main thread). m_governorParams is sim-thread-only (configure()d into the governor each tick
    // so reload_config is automatic, like m_congestionParams).
    TickGovernor m_tickGovernor;
    TickGovernorParams m_governorParams{};
    std::atomic<float> m_overrunLoadFactor{1.f};
    std::atomic<uint32_t> m_overrunSnapInterval{1};
    std::atomic<uint32_t> m_overrunAiStride{1};
    std::atomic<float> m_overrunInterestScale{1.f};

    // Congestion-controller run-long watermarks (#714). Updated on the sim thread in the congestion
    // pass (only while >= 1 peer is connected, so they freeze rather than reset when the load clients
    // disconnect); mirrored into atomics so getCongestionTelemetry() is a safe cross-thread read.
    float m_congMinSendHzSim{60.f};       // sim-thread working copy of the min watermark
    float m_congRecoveredSendHzSim{60.f}; // sim-thread working copy of the max-since-min watermark
    // Wire-traffic telemetry (#772): sampled on the sim thread every kWireSampleTicks and mirrored
    // into atomics so getWireTelemetry() is a safe cross-thread read. Sampled periodically rather
    // than every tick because GNS's getWireStats() walks every live connection.
    static constexpr uint64_t kWireSampleTicks = 30; // ~0.5 s at 60 Hz
    std::atomic<double> m_wireOutKbs{0.0};
    std::atomic<double> m_wireInKbs{0.0};
    std::atomic<double> m_wireOutPps{0.0};
    std::atomic<int> m_wirePeersAtSample{0};

    std::atomic<float> m_congMinSendHz{60.f};
    std::atomic<float> m_congRecoveredSendHz{60.f};
    std::atomic<float> m_congMaxLoss{0.f};

    // Interest management + delta compression state (sim-thread only).
    double m_drawDistanceM{200'000.0}; // precomputed from drawDistanceKm × 1000; 200 km default
    // Per-client snapshot byte budget (#516): 0 = unlimited (send every visible entity, legacy
    // behaviour used by unit tests). fl-server sets a real budget; atomic so reload_config can mutate
    // it (the read happens on the sim thread). The scheduler ranks visible entities by relevance and
    // sends only the highest-priority set that fits.
    std::atomic<uint32_t> m_snapshotBudgetBytes{0};
    // Snapshot payload compression (#775): internal default OFF (unit tests assert raw byte shapes);
    // fl-server config default ON. Atomic for reload_config; frozen into a local before the parallel
    // per-peer pass.
    std::atomic<bool> m_compressSnapshots{false};
    SchedulerWeights m_schedulerWeights{};     // relevance weights (tuned defaults; sim-thread only)
    std::atomic<uint32_t> m_jitterMaxDepth{4}; // global cap for per-peer jitter buffer initialization
    // Adaptive resize parameters — sim-thread only; hot-reloadable via enqueueSimCallback.
    uint32_t m_jitterAdaptWindow{60}; // EWMA smoothing window; alpha = 1/window
    uint32_t m_jitterHysteresis{2};   // dead-band ticks before resize fires
    float m_jitterMultiplier{2.0f};   // k factor in depth = ceil(ewma + k*jitter)

    // Per-client adaptive send-rate / congestion-response params (#518): sim-thread only, copied into
    // each peer's CongestionController every tick (so reload_config hot-reload is automatic). Default
    // is enabled with full-rate-when-healthy behaviour — zero link stats (mocks, loopback) leave every
    // peer at the full 60 Hz / full budget, so existing per-tick-send tests are unaffected.
    CongestionParams m_congestionParams{};

    // Per-peer entity tracking: peerId → (entityIdx → record). Drives client-acked delta baselines:
    //   * gen           — full vs delta on respawn (generation change forces a full).
    //   * lastSentTick  — scheduler recency term + the kSnapshotRetentionTicks force-full (the
    //                     interest-out / client-evicted re-entry case) + the knownGens GC prune.
    //   * fullStreakTick— tick the CURRENT contiguous run of full records started on (0 = never sent
    //                     a full). The entity is sent full every tick until the peer confirms it decoded
    //                     fullStreakTick (selective-ack, #566); freezing the streak start (rather than
    //                     advancing it each tick) lets it converge to deltas in one RTT rather than
    //                     re-fulling forever (the confirm target must be a fixed tick the ack can catch).
    //   * lastWasFull   — whether the last record sent for this entity was a full (detects a
    //                     contiguous full run together with lastSentTick).
    // Erased in full on peer disconnect; pruned per-tick once stale past kSnapshotRetentionTicks.
    struct PeerEntityRec {
        uint16_t gen{0};
        uint64_t lastSentTick{0};
        uint64_t fullStreakTick{0};
        bool lastWasFull{false};
    };
    std::unordered_map<uint32_t, std::unordered_map<uint32_t, PeerEntityRec>> m_peerKnownGens;

    // Per-peer pending explicit despawns (#516): peerId → (entityIdx → remaining repeat ticks). An
    // entity the peer knew that left the sim entirely (kill/despawn) is queued here and emitted in the
    // SnapshotDespawn TLV for kDespawnRepeatTicks ticks (drop tolerance on the unreliable channel).
    // Erased in full on peer disconnect.
    std::unordered_map<uint32_t, std::unordered_map<uint32_t, uint8_t>> m_peerPendingDespawn;

    // Per-tick scratch for the data-parallel per-peer snapshot build. Each entry resolves a peer's
    // stable per-peer state pointers once (serially, in the gather), so the parallel build performs
    // no map operator[] / rehash; the worker writes only into its own buf and the peer-private maps
    // it points at. The sim thread then flushes buf via m_net.send. buf retains capacity across ticks.
    struct PeerSnapWork {
        uint32_t peerId{};
        EntityId peerEid;                      // invalid for an observer (no entity) (#857)
        const EntityState* peerState{nullptr}; // null for an observer
        double center[3]{};                    // interest center: the pilot's entity, or the observer's point

        PeerInputState* pin{nullptr};
        std::unordered_map<uint32_t, PeerEntityRec>* knownGens{nullptr};
        std::unordered_map<uint32_t, uint8_t>* pending{nullptr};
        std::vector<uint8_t> buf;
        std::vector<uint8_t> compressScratch; // zstd output scratch (#775); reused across ticks
    };
    std::vector<PeerSnapWork> m_peerWork;

    // Shutdown countdown state (sim-thread only).
    bool m_shuttingDown{false};
    std::chrono::steady_clock::time_point m_shutdownAt{};
    std::chrono::steady_clock::time_point m_nextNoticeAt{};
    uint32_t m_warningIntervalS{300};
    std::string m_shutdownReason;
    std::function<void()> m_shutdownCallback;
};

} // namespace fl
