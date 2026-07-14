// SPDX-License-Identifier: GPL-3.0-or-later
#include "net/WorldBroadcaster.h"
#include "render/RenderSnapshot.h"

#include "ILogger.h"
#include "INetwork.h"
// The wingman grammar (#610). Header-only and stdlib-only, so this adds NO link dependency —
// engine-net still does not link engine-ai (cmake/layering.cmake), and must not: building a
// controller is engine-ai's job and reaches this file only through the FlightOrderHandler hook.
// Including it here rather than hardcoding ordinals keeps one source of truth for the vocabulary.
#include "ai/WingmanCommand.h"
#include "entity/EntityManager.h"
#include "entity/EntityState.h"
#include "entity/EntityTypeRegistry.h"
#include "entity/IEntityController.h"
#include "flight/BuiltinFlightModel.h"
#include "flight/CentralGravityField.h"
#include "flight/FlightIntegrator.h"
#include "job/JobSystem.h"
#include "net/AckWindow.h"
#include "net/BitStream.h"
#include "net/GameProtocol.h"
#include "net/NetworkUtils.h"
#include "net/SnapshotCodec.h"
#include "net/SnapshotCompression.h"
#include "net/WireCodec.h"
#include "weather/WeatherController.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

static_assert(std::atomic<double>::is_always_lock_free,
              "WorldBroadcaster requires lock-free double atomics for entity XZ cache");

using namespace fl;

// ---------------------------------------------------------------------------
// Control sources
// ---------------------------------------------------------------------------

namespace {
// Drives an entity from the latest MsgClientInput stored for its connected peer. Holds a pointer to
// the peer's stable PeerInputState slot in WorldBroadcaster::m_peerInputs (unordered_map element
// pointers stay valid across rehash); the slot outlives the controller (torn down first on disconnect).
class PeerController final : public fl::IEntityController {
  public:
    explicit PeerController(const fl::PeerInputState* input) : m_input(input) {}

    fl::ControlInput sample(const fl::EntityState& /*state*/, uint64_t /*tick*/, double /*dt*/,
                            const fl::AiTickContext& /*ctx*/ = {}) override {
        fl::ControlInput ctrl{};
        ctrl.throttle = m_input->throttle;
        ctrl.elevator = m_input->elevator;
        ctrl.aileron = m_input->aileron;
        ctrl.rudder = m_input->rudder;
        ctrl.afterburner = (m_input->buttons & 0x02u) != 0; // bit 1 per MsgClientInput::buttons
        return ctrl;
    }

  private:
    const fl::PeerInputState* m_input;
};
} // namespace

// ---------------------------------------------------------------------------
// IP address helpers
// ---------------------------------------------------------------------------

// Extract the normalized IP from an "ip:port" or "[ip]:port" string returned by getPeerAddress().
static std::string extractIp(const char* addrPort) {
    if (!addrPort)
        return {};
    std::string_view av(addrPort);
    std::string_view ipv;
    if (!av.empty() && av.front() == '[') {
        av.remove_prefix(1);
        auto end = av.find(']');
        ipv = (end != std::string_view::npos) ? av.substr(0, end) : av;
    } else {
        auto colon = av.rfind(':');
        ipv = (colon != std::string_view::npos) ? av.substr(0, colon) : av;
    }
    return fl::normalizeIp(ipv);
}

// ---------------------------------------------------------------------------
// Quaternion helpers — pure float array math, no GLM dependency.
// Convention: q = [x, y, z, w] matching EntityTransform::quat.
// ---------------------------------------------------------------------------

// Rotate vector v by quaternion q using the Rodrigues formula.
static void quatRotate(const float q[4], const float v[3], float out[3]) {
    float tx = q[1] * v[2] - q[2] * v[1];
    float ty = q[2] * v[0] - q[0] * v[2];
    float tz = q[0] * v[1] - q[1] * v[0];
    out[0] = v[0] + 2.f * q[3] * tx + 2.f * (q[1] * tz - q[2] * ty);
    out[1] = v[1] + 2.f * q[3] * ty + 2.f * (q[2] * tx - q[0] * tz);
    out[2] = v[2] + 2.f * q[3] * tz + 2.f * (q[0] * ty - q[1] * tx);
}

// Returns true if `incoming` is strictly newer than `last` under uint32 wrap-around.
// Uses the half-window comparison: a difference in [1, 2^31-1] (mod 2^32) is "newer".
static bool isNewerSeq(uint32_t incoming, uint32_t last) noexcept {
    return incoming != last && ((incoming - last) & 0x80000000u) == 0u;
}

// ---------------------------------------------------------------------------
// Connection-rejection reason table — one place mapping each ConnectRefusalCode
// to the client-facing reason text and the server-side log phrase/level.
// ---------------------------------------------------------------------------
namespace {
struct RejectInfo {
    const char* reason;    // sent to the client in MsgConnectRefusal
    const char* logPhrase; // context logged server-side
    LogLevel level;
};
RejectInfo rejectInfoFor(fl::ConnectRefusalCode code) {
    using C = fl::ConnectRefusalCode;
    switch (code) {
    case C::Banned:
        return {"You are banned from this server.", "banned", LogLevel::Info};
    case C::AccessDenied:
        return {"Access denied.", "not on allowlist", LogLevel::Info};
    case C::RateLimited:
        return {"Connection rate limit exceeded. Try again later.", "rate-limited", LogLevel::Info};
    case C::TooManyConnections:
        return {"Too many connections from your address.", "too many connections from this address", LogLevel::Info};
    case C::AdminLockout:
        return {"Access denied.", "admin auth lockout active", LogLevel::Warn};
    case C::Generic:
        break;
    }
    return {"Access denied.", "access denied", LogLevel::Info};
}
} // namespace

namespace fl {

// GameProtocol.h must stay stdlib-only (engine-protocol is zero-dep, enforced by fl_assert_zero_dep),
// so it MIRRORS these two values rather than including the headers that define them. This is the one
// TU that sees both sides, so this is where the mirrors are checked. If either fires, the wire and
// the engine disagree about the vocabulary, which is exactly the drift the #678 record and the #624
// validator were both cleaning up.
static_assert(kWingmanCommandCount == static_cast<uint8_t>(fl::ai::WingmanCommand::Count),
              "GameProtocol kWingmanCommandCount is out of sync with fl::ai::WingmanCommand");
static_assert(kNoFlightId == fl::kNoFormation, "GameProtocol kNoFlightId is out of sync with fl::kNoFormation");

WorldBroadcaster::WorldBroadcaster(EntityManager& entityManager, EntityTypeRegistry& registry, INetwork& net,
                                   ILogger& logger, WeatherController* weather)
    : m_entityManager(entityManager), m_registry(registry), m_net(net), m_logger(logger), m_weather(weather),
      m_sensorSystem(entityManager, registry), m_gravity(&fl::CentralGravityField::earthInstance()),
      m_planetRadiusKm(6371.f) {}

WorldBroadcaster::~WorldBroadcaster() = default;

// ---------------------------------------------------------------------------
// Peer management (sim-thread only)
// ---------------------------------------------------------------------------

void WorldBroadcaster::kickPeer(uint32_t peerId) {
    m_net.disconnectPeer(peerId);
}

void WorldBroadcaster::banAddress(std::string ip) {
    ip = fl::normalizeIp(ip);
    m_bannedAddresses.insert(ip);
    for (const auto& [peerId, eid] : m_peerEntities) {
        if (extractIp(m_net.getPeerAddress(peerId)) == ip)
            m_net.disconnectPeer(peerId);
    }
}

void WorldBroadcaster::unbanAddress(const std::string& ip) {
    m_bannedAddresses.erase(fl::normalizeIp(ip));
}

bool WorldBroadcaster::unlockAdminAuth(const std::string& ip) {
    std::string norm = fl::normalizeIp(ip);
    bool wasLocked = m_adminAuthTracker.isLockedOut(norm);
    m_adminAuthTracker.clearLockout(norm);
    return wasLocked;
}

AuthLockoutSummary WorldBroadcaster::getAuthLockoutSummary() const {
    AuthLockoutSummary s;
    s.threshold = m_adminAuthTracker.maxFailures();
    s.entries = m_adminAuthTracker.failureSummary();
    for (const auto& e : s.entries)
        if (e.lockedOut)
            ++s.activeCount;
    return s;
}

void WorldBroadcaster::setBannedAddresses(std::unordered_set<std::string> addrs) {
    m_bannedAddresses = std::move(addrs);
}

void WorldBroadcaster::setAllowedAddresses(std::unordered_set<std::string> addrs) {
    m_allowedAddresses = std::move(addrs);
}

std::unordered_set<std::string> WorldBroadcaster::getBannedAddresses() const {
    return m_bannedAddresses;
}

void WorldBroadcaster::setRateLimitParams(int maxConnects, int windowSeconds, int floodMultiplier) {
    m_connectRateLimit = maxConnects;
    m_connectRateWindowS = windowSeconds;
    m_floodMultiplier = floodMultiplier;
}

void WorldBroadcaster::setMaxConnectionsPerIp(int max) noexcept {
    m_maxConnectionsPerIp = max;
}

void WorldBroadcaster::setSpawnPoints(std::vector<std::array<double, 3>> points) noexcept {
    m_spawnPoints = std::move(points);
}

void WorldBroadcaster::setClock(const IClock& clock) {
    m_clock = &clock;
    m_adminAuthTracker.setClock(clock);
    m_tickProfiler.setClock(clock);
}

void WorldBroadcaster::setMotd(std::string motd) {
    m_motd = std::move(motd);
}

void WorldBroadcaster::setMotdDisplaySeconds(uint16_t seconds) noexcept {
    m_motdDisplaySeconds = seconds;
}

void WorldBroadcaster::setPlayerFaction(uint16_t faction) noexcept {
    m_playerFaction = faction;
}

void WorldBroadcaster::setFlightSpawner(FlightSpawner fn) {
    m_flightSpawner = std::move(fn);
}

void WorldBroadcaster::setFlightOrderHandler(FlightOrderHandler fn) {
    m_flightOrderHandler = std::move(fn);
}

void WorldBroadcaster::setTargetDesignator(TargetDesignator fn) {
    m_targetDesignator = std::move(fn);
}

void WorldBroadcaster::setFlightCommandRateLimit(int perSecond) noexcept {
    m_flightCmdRateLimit = perSecond > 0 ? perSecond : 1;
}

void WorldBroadcaster::setFlightModelResolver(FlightModelResolver fn) {
    m_flightModelResolver = std::move(fn);
}

void WorldBroadcaster::setSensorDefResolver(sensor::SensorSystem::SensorDefResolver fn) {
    m_sensorSystem.setResolver(std::move(fn));
}

void WorldBroadcaster::setSensorCheckHz(float hz) noexcept {
    m_sensorCheckHz.store(std::clamp(hz, 1.f, 60.f), std::memory_order_relaxed);
}

void WorldBroadcaster::setEmitting(uint32_t entityIdx, bool emitting) {
    m_sensorSystem.setEmitting(entityIdx, emitting);
}

const sensor::ContactTable* WorldBroadcaster::contactsFor(uint32_t entityIdx) const {
    return m_sensorSystem.contactsFor(entityIdx);
}

void WorldBroadcaster::setAiScaling(const AiScaling& scaling) noexcept {
    m_aiScaling = scaling;
}

void WorldBroadcaster::setOperatorPassword(std::string password) {
    m_operatorPassword = std::move(password);
}

void WorldBroadcaster::setAdminDispatch(std::function<std::string(std::string_view)> fn) {
    m_adminDispatch = std::move(fn);
}

void WorldBroadcaster::setAdminShell(std::function<int()> markFn,
                                     std::function<std::vector<std::string>(int)> drainFn) {
    m_adminShellMark = std::move(markFn);
    m_adminShellDrain = std::move(drainFn);
}

void WorldBroadcaster::setAdminAuthParams(int maxFailures, int lockoutSeconds) {
    m_adminAuthTracker = AuthTracker(maxFailures, lockoutSeconds);
    m_adminAuthTracker.setClock(*m_clock);
}

void WorldBroadcaster::setGravityField(const IGravityField& field, float planetRadiusKm) noexcept {
    m_gravity = &field;
    m_planetRadiusKm = planetRadiusKm;
}

void WorldBroadcaster::setGroundElevationQuery(std::function<float(glm::dvec3)> fn) {
    m_groundQuery = std::move(fn);
}

void WorldBroadcaster::applyConfig(const WorldBroadcasterConfig& cfg) {
    setRateLimitParams(cfg.connectRateLimit, cfg.connectRateWindowS, cfg.floodMultiplier);
    setMaxConnectionsPerIp(cfg.maxConnectionsPerIp);
    setAdminAuthParams(cfg.adminAuthMaxFailures, cfg.adminAuthLockoutSeconds);
    setMotd(cfg.motd);
    setMotdDisplaySeconds(cfg.motdDisplaySeconds);
    setOperatorPassword(cfg.operatorPassword);
    setIdleTimeout(cfg.idleTimeoutS);
    setDrawDistance(cfg.drawDistanceKm);
    setSpatialCellSize(cfg.spatialCellSizeM); // after setDrawDistance: auto mode reads m_drawDistanceM
    setSnapshotBudget(cfg.snapshotBudgetBytes);
    setSnapshotCompression(cfg.compressSnapshots);
    setJitterBufferDepth(cfg.jitterBufferMaxDepth);
    setJitterAdaptWindow(cfg.jitterAdaptWindow);
    setJitterHysteresis(cfg.jitterHysteresis);
    setJitterMultiplier(cfg.jitterMultiplier);
    setCongestionParams(cfg.congestion);
    setGovernorParams(cfg.governor);
}

void WorldBroadcaster::setIdleTimeout(int timeoutSeconds) noexcept {
    m_idleTimeoutTicks = timeoutSeconds > 0 ? static_cast<uint64_t>(timeoutSeconds) * 60u : 0u;
}

void WorldBroadcaster::setInputTraceDir(std::string dir) {
    // Any change closes the currently open per-peer trace files so records never straddle two
    // directories; disabling clears the dir, enabling/switching creates the target and lets each
    // peer's writer reopen lazily on its next accepted input.
    m_peerTraceWriters.clear();
    m_inputTraceDir = std::move(dir);
    if (m_inputTraceDir.empty())
        return;
    std::error_code ec;
    std::filesystem::create_directories(m_inputTraceDir, ec);
    if (ec) {
        char msg[256];
        std::snprintf(msg, sizeof(msg), "input trace dir '%s' could not be created (%s) — tracing disabled",
                      m_inputTraceDir.c_str(), ec.message().c_str());
        m_logger.log(LogLevel::Warn, __FILE__, __LINE__, msg);
        m_inputTraceDir.clear();
    }
}

void WorldBroadcaster::setDrawDistance(float km) noexcept {
    m_drawDistanceM = static_cast<double>(km) * 1000.0;
}

void WorldBroadcaster::setSpatialCellSize(double cellSizeM) {
    // Auto (<= 0): pick a cell so a full draw-distance query spans a bounded number of cells rather
    // than degenerating toward O(N) at high density. drawDistance/32 with a 500 m floor keeps a
    // 200 km radius near ~6 km cells (~13x13 cells/query) while never producing a zero/UB cell size.
    double resolved = cellSizeM;
    if (resolved <= 0.0) {
        const double dd = m_drawDistanceM > 0.0 ? m_drawDistanceM : 200'000.0;
        resolved = std::clamp(dd / 32.0, 500.0, 10'000.0);
    }
    m_spatialIndex.setCellSize(resolved);
}

void WorldBroadcaster::setSnapshotBudget(uint32_t bytes) noexcept {
    m_snapshotBudgetBytes.store(bytes, std::memory_order_relaxed);
}

void WorldBroadcaster::setSnapshotCompression(bool enabled) noexcept {
    m_compressSnapshots.store(enabled, std::memory_order_relaxed);
}

void WorldBroadcaster::setJitterBufferDepth(uint32_t maxDepth) noexcept {
    m_jitterMaxDepth.store(maxDepth == 0u ? 1u : maxDepth, std::memory_order_relaxed);
}

void WorldBroadcaster::setJitterAdaptWindow(uint32_t ticks) noexcept {
    m_jitterAdaptWindow = (ticks == 0u ? 1u : ticks);
}

void WorldBroadcaster::setJitterHysteresis(uint32_t ticks) noexcept {
    m_jitterHysteresis = ticks;
}

void WorldBroadcaster::setJitterMultiplier(float k) noexcept {
    m_jitterMultiplier = (k < 0.f ? 0.f : k);
}

void WorldBroadcaster::setCongestionParams(const CongestionParams& params) noexcept {
    m_congestionParams = params;
}

void WorldBroadcaster::setGovernorParams(const TickGovernorParams& params) noexcept {
    m_governorParams = params;
}

void WorldBroadcaster::forEachPeer(std::function<void(const PeerInfo&)> fn) const {
    for (const auto& [peerId, eid] : m_peerEntities) {
        PeerInfo pi;
        pi.peerId = peerId;
        pi.eid = eid;
        const char* raw = m_net.getPeerAddress(peerId);
        pi.addr = raw ? raw : "";
        if (auto it = m_peerInputs.find(peerId); it != m_peerInputs.end()) {
            const PeerInputState& ps = it->second;
            pi.delayTicks = ps.estimatedDelayTicks;
            pi.queueDepth = ps.jitterBuffer.size();
            pi.bufferMaxDepth = ps.jitterBuffer.maxDepth();
            pi.ewmaDelayTicks = ps.ewmaDelayTicks;
            pi.ewmaJitterTicks = ps.ewmaJitterTicks;
            const uint32_t interval = ps.congestion.sendIntervalTicks();
            pi.sendRateHz = interval > 0u ? 60.f / static_cast<float>(interval) : 60.f;
            pi.effectiveBudget = ps.congestion.effectiveBudget(m_snapshotBudgetBytes.load(std::memory_order_relaxed));
            pi.packetLoss = m_net.getPeerLinkStats(peerId).packetLoss; // live ENet mean loss fraction
        }
        fn(pi);
    }
}

void WorldBroadcaster::onTick(double simDt, uint64_t tickIndex) {
    m_currentTick = tickIndex;

    // Per-phase tick-budget instrumentation. beginTick() resets the per-tick accumulators and
    // records the wall start; each phase boundary records its elapsed wall-time; endTick() rolls
    // the samples into the rolling window. See TickProfiler.h.
    m_tickProfiler.beginTick();
    const auto tMaintenanceStart = m_clock->now();

    // Coarse prune of stale rate-limit records every 600 ticks (~10 s at 60 Hz).
    if (++m_ratePruneTick % 600 == 0) {
        auto cutoff = m_clock->now() - std::chrono::seconds(m_connectRateWindowS);
        for (auto it = m_connectRecords.begin(); it != m_connectRecords.end();) {
            auto& ts = it->second.timestamps;
            while (!ts.empty() && ts.front() < cutoff)
                ts.pop_front();
            if (ts.empty())
                it = m_connectRecords.erase(it);
            else
                ++it;
        }
        m_adminAuthTracker.pruneExpired();
    }

    // Idle timeout: disconnect peers that have sent no activity for m_idleTimeoutTicks ticks.
    if (m_idleTimeoutTicks > 0) {
        std::vector<uint32_t> toKick;
        for (const auto& [peerId, ps] : m_peerInputs) {
            if (tickIndex > ps.lastActivityTick && tickIndex - ps.lastActivityTick >= m_idleTimeoutTicks)
                toKick.push_back(peerId);
        }
        for (uint32_t pid : toKick) {
            char msg[80];
            std::snprintf(msg, sizeof(msg), "peer %u idle timeout — disconnecting", pid);
            m_logger.log(LogLevel::Info, __FILE__, __LINE__, msg);
            m_net.disconnectPeer(pid);
        }
    }

    // Fire deferred admin drains: deliver CommandShell output written by enqueueSimCallback
    // lambdas as follow-on MsgAdminResponseChunk packets. Uses a wall-clock deadline (20 ms,
    // matching the RCON drain) rather than a tick index, so drain timing is immune to
    // GameLoop tick-batch catch-up (up to kMaxTicksPerIteration ticks per iteration).
    if (!m_pendingAdminDrains.empty() && m_adminShellDrain) {
        auto it = m_pendingAdminDrains.begin();
        while (it != m_pendingAdminDrains.end()) {
            if (m_clock->now() < it->drainDeadline) {
                ++it;
                continue;
            }
            if (m_peerEntities.count(it->peerId)) {
                auto lines = m_adminShellDrain(it->shellMark);
                if (!lines.empty()) {
                    std::string payload;
                    for (const auto& ln : lines) {
                        if (!ln.empty()) {
                            payload += ln;
                            payload += '\n';
                        }
                    }
                    if (!payload.empty())
                        payload.pop_back(); // trim trailing newline
                    if (!payload.empty())
                        sendAdminResponse(m_net, it->peerId, it->reqId, payload);
                }
            }
            it = m_pendingAdminDrains.erase(it);
        }
    }

    // Rebuild spatial index from entity positions at tick start (previous-tick state).
    // Dead entities were reaped in the previous tick's m_entityManager.onTick(); forEach skips them.
    m_spatialIndex.clear();
    m_entityManager.forEach([this](const EntityState& s) { m_spatialIndex.insert(s.id.index, s.transform.pos); });

    // Drain one buffered input per peer before stepping. When the buffer is empty the existing
    // control fields are retained (stale repeat) — the entity continues on its last known inputs
    // rather than coasting to zero. viewAxis is not buffered (camera only, not flight control).
    for (auto& [peerId, ps] : m_peerInputs) {
        BufferedInput bi;
        if (ps.jitterBuffer.pop(bi)) {
            ps.throttle = bi.throttle;
            ps.elevator = bi.elevator;
            ps.aileron = bi.aileron;
            ps.rudder = bi.rudder;
            ps.buttons = bi.buttons;
        }
    }

    // Adaptive jitter buffer resize: for each peer with a seeded EWMA, compute the target depth
    // from the delay EWMA and inter-arrival jitter EWMA, then resize if outside the hysteresis band.
    // Runs O(P) float comparisons per tick — negligible at max 32 peers × 60 Hz.
    {
        const uint32_t globalMax = m_jitterMaxDepth.load(std::memory_order_relaxed);
        const float k = m_jitterMultiplier;
        const uint32_t hysteresis = m_jitterHysteresis;
        for (auto& [peerId, ps] : m_peerInputs) {
            if (!ps.ewmaSeeded)
                continue;
            const float targetF =
                std::clamp(ps.ewmaDelayTicks + k * ps.ewmaJitterTicks, 1.0f, static_cast<float>(globalMax));
            const uint32_t target = static_cast<uint32_t>(std::ceil(targetF));
            const uint32_t current = ps.jitterBuffer.maxDepth();
            const bool shouldGrow = (target > current && target - current > hysteresis);
            const bool shouldShrink = (current > target && current - target > hysteresis);
            if (shouldGrow || shouldShrink)
                ps.jitterBuffer.setMaxDepth(target);
        }
    }

    // Adaptive send-rate / congestion response (#518): sample each connected peer's ENet link quality
    // and step its AIMD controller. The controller holds a per-peer throttle that gates both the
    // snapshot send cadence (decimation gate in the per-peer loop below) and the effective byte budget.
    // configure() each tick so reload_config param changes (and the enabled flag) take effect live; the
    // getPeerLinkStats call is a cheap field read (zeros from the mock/loopback => throttle stays 1).
    float congMinHzThisTick = 60.f; // min adaptive send rate across peers this tick
    float congMaxLossThisTick = 0.f;
    for (auto& [peerId, eid] : m_peerEntities) {
        (void)eid;
        PeerInputState& ps = m_peerInputs[peerId];
        const PeerLinkStats link = m_net.getPeerLinkStats(peerId);
        CongestionSample sample;
        sample.packetLoss = link.packetLoss;
        sample.rttMs = link.rttMs;
        sample.reliableBytesInFlight = link.reliableBytesInFlight;
        ps.congestion.configure(m_congestionParams);
        ps.congestion.update(tickIndex, sample);
        congMinHzThisTick = std::min(congMinHzThisTick, 60.f / static_cast<float>(ps.congestion.sendIntervalTicks()));
        congMaxLossThisTick = std::max(congMaxLossThisTick, link.packetLoss);
    }
    // Congestion telemetry watermarks (#714) — only advanced while peers are connected, so the values
    // freeze (rather than reset toward 60/0) once the load clients disconnect and the final metrics
    // writes still carry the run's evidence. A new all-time minimum resets the recovered watermark to
    // the current rate; recovery is then the max the (slowest) peer climbs back to afterwards.
    if (!m_peerEntities.empty()) {
        if (congMinHzThisTick < m_congMinSendHzSim) {
            m_congMinSendHzSim = congMinHzThisTick;
            m_congRecoveredSendHzSim = congMinHzThisTick;
        } else {
            m_congRecoveredSendHzSim = std::max(m_congRecoveredSendHzSim, congMinHzThisTick);
        }
        m_congMinSendHz.store(m_congMinSendHzSim, std::memory_order_relaxed);
        m_congRecoveredSendHz.store(m_congRecoveredSendHzSim, std::memory_order_relaxed);
        if (congMaxLossThisTick > m_congMaxLoss.load(std::memory_order_relaxed))
            m_congMaxLoss.store(congMaxLossThisTick, std::memory_order_relaxed);
    }

    // Wire-traffic sample (#772). Sim-thread only — the transport host is sim-thread-owned — and
    // only every kWireSampleTicks, because the GNS backend walks every live connection to build it.
    // Published to relaxed atomics for the --metrics-json writer.
    //
    // We keep the sample taken at the HIGHEST peer count seen so far (refreshing on ties, so it
    // tracks steady state rather than the first tick of the ramp), and never publish an idle one.
    // The gate reads a single end-of-run snapshot, and fl-server keeps writing that file while the
    // swarm ramps up and drains away — so "latest sample" would report the connect ramp or, worse,
    // the disconnect drain (measured: a 16-client run reported its wire rate at 2 peers), and
    // "latest sample with any peers" is no better. Full-load is the number worth gating on, and the
    // peer count travels WITH the sample so the per-client figure divides by the peers that actually
    // produced the traffic, not by whoever happens to be connected when the file is written. (Same
    // class of trap as the #714 congestion watermarks, which freeze for the same reason.)
    if (m_currentTick % kWireSampleTicks == 0 && !m_peerEntities.empty()) {
        const int peersNow = static_cast<int>(m_peerEntities.size());
        if (peersNow >= m_wirePeersAtSample.load(std::memory_order_relaxed)) {
            const WireStats w = m_net.getWireStats();
            m_wireOutKbs.store(w.outBytesPerSec / 1000.0, std::memory_order_relaxed);
            m_wireInKbs.store(w.inBytesPerSec / 1000.0, std::memory_order_relaxed);
            m_wireOutPps.store(w.outPacketsPerSec, std::memory_order_relaxed);
            m_wirePeersAtSample.store(peersNow, std::memory_order_relaxed);
        }
    }

    // Graceful tick-overrun governor (#514/#726): step from the PREVIOUS tick's measured wall-time vs
    // the fixed-step budget, then freeze its four lever values for this tick's parallel regions.
    // configure() each tick so reload_config (m_governorParams) takes effect live, like the per-peer
    // congestion controllers. Publish the levers into the atomics that getOverrunStatus() reads from
    // the main thread.
    m_tickGovernor.configure(m_governorParams);
    m_tickGovernor.update(tickIndex, m_tickProfiler.lastTotalMs(), simDt * 1000.0);
    const uint32_t govSnapInterval = m_tickGovernor.snapshotIntervalTicks();
    const uint32_t govAiStride = m_tickGovernor.aiSampleStride();
    const float govInterestScale = m_tickGovernor.interestScale();
    m_overrunLoadFactor.store(m_tickGovernor.loadFactor(), std::memory_order_relaxed);
    m_overrunSnapInterval.store(govSnapInterval, std::memory_order_relaxed);
    m_overrunAiStride.store(govAiStride, std::memory_order_relaxed);
    m_overrunInterestScale.store(govInterestScale, std::memory_order_relaxed);

    m_tickProfiler.addPhaseSample(
        TickPhase::Maintenance, std::chrono::duration<double, std::milli>(m_clock->now() - tMaintenanceStart).count());

    // ---- Per-entity simulation: gather, AI sample pass, integrate pass ----
    // Two passes (rather than one interleaved loop) so AI sampling reads a consistent pre-step
    // world snapshot, and the integrate pass writes only each entity's own state — no cross-entity
    // writes. Both passes are therefore safe to run data-parallel (see runEntityPass). Each pass is
    // timed as one wall-clock phase.

    // Gather the live controlled entities into a contiguous, indexable range.
    m_stepItems.clear();
    for (auto& [entityIdx, ce] : m_controlledEntities) {
        EntityState* state = m_entityManager.get(ce.id);
        if (!state || state->dead)
            continue;
        m_stepItems.push_back({entityIdx, &ce, state});
    }
    m_stepInputs.resize(m_stepItems.size());

    // ---- Sensing pass (#685) ----
    // Runs BEFORE the AI pass and AFTER the spatial rebuild, so a controller samples against contacts
    // detected from the same pre-step world it is about to steer in. Each observer writes only its own
    // ObserverState and reads only const world state, so the pass is data-parallel and
    // serial-equivalent: the dice are seeded from (observer, target, tick, slot, lobe) rather than
    // drawn from shared RNG state, so the contact tables are byte-identical on 1 worker and on 16.
    sensor::SensingEnvironment sensingEnv{};
    if (m_weather) {
        const EnvironmentState envState = m_weather->computeEnvironment();
        sensingEnv.cloudCoverage = envState.cloudCoverage;
        sensingEnv.fogDensity = envState.fogDensity;
        sensingEnv.fogStartDist = envState.fogStartDist;
        sensingEnv.timeOfDayH = envState.timeOfDay;
        sensingEnv.isNight = (envState.timeOfDay < 6.f || envState.timeOfDay >= 20.f);
    }
    // Unset difficulty = NO scaling: radar reaches its authored range and the AI reacts the moment it
    // detects. See setAiScaling — defaulting to AiScaling{} would silently apply the Cadet preset.
    const float radarRangeFraction = m_aiScaling ? m_aiScaling->radarSensorRange : 1.f;
    const float reactionTimeS = m_aiScaling ? m_aiScaling->reactionTimeS : 0.f;
    {
        const auto tSensingStart = m_clock->now();

        const float checkHz = m_sensorCheckHz.load(std::memory_order_relaxed);
        const auto stride =
            static_cast<uint32_t>(std::max(1L, std::lround(1.0 / (simDt * static_cast<double>(checkHz)))));

        auto& work = m_sensorSystem.gatherDue(tickIndex, stride, simDt);
        runEntityPass(work.size(),
                      [this, &work, tickIndex, &sensingEnv, radarRangeFraction, reactionTimeS](size_t b, size_t e) {
                          for (size_t i = b; i < e; ++i)
                              m_sensorSystem.evaluateObserver(work[i], m_spatialIndex, tickIndex, sensingEnv,
                                                              radarRangeFraction, reactionTimeS);
                      });

        // Reaction bookkeeping is NOT staggered: a contact's `reacted` flag must flip on the exact
        // tick its delay elapses, not up to a stride later. It touches no geometry, so it is cheap
        // enough to run every tick for every observer on the sim thread.
        m_sensorSystem.updateReactions(tickIndex, simDt, reactionTimeS);

        m_tickProfiler.addPhaseSample(
            TickPhase::Sensing, std::chrono::duration<double, std::milli>(m_clock->now() - tSensingStart).count());
    }

    // AI pass: sample each controller. Read-only on shared world state (EntityState, SpatialIndex,
    // EntityManager); each controller's own mutable state is per-entity / disjoint.
    //
    // The world-wide parts of the context are shared const across workers; `contacts` is per-observer
    // and is looked up inside the worker, so a controller can only ever see ITS OWN detections.
    const AiScaling* difficulty = m_aiScaling ? &*m_aiScaling : nullptr;
    {
        const auto tAiStart = m_clock->now();
        runEntityPass(m_stepItems.size(),
                      [this, tickIndex, simDt, govAiStride, &sensingEnv, difficulty](size_t b, size_t e) {
                          for (size_t i = b; i < e; ++i) {
                              const StepItem& it = m_stepItems[i];
                              // AI-sample decimation (#514): a decimatable (non-player) entity reuses its last sampled
                              // input on ticks where (tickIndex + idx) % stride != 0 — a pure function of (idx, tick,
                              // stride), so the skip pattern is identical across worker counts (serial-equivalent), and
                              // each worker writes only its own entity's lastInput cache (disjoint). Players (stride
                              // applies to all, but decimatable=false) and any entity not yet sampled always sample.
                              if (it.ce->decimatable && it.ce->lastInputValid && govAiStride > 1u &&
                                  ((tickIndex + it.idx) % govAiStride) != 0u) {
                                  m_stepInputs[i] = it.ce->lastInput;
                              } else {
                                  const AiTickContext aiCtx{&m_spatialIndex, m_sensorSystem.contactsFor(it.idx),
                                                            &sensingEnv, difficulty};
                                  m_stepInputs[i] = it.ce->controller->sample(*it.state, tickIndex, simDt, aiCtx);
                                  it.ce->lastInput = m_stepInputs[i];
                                  it.ce->lastInputValid = true;
                              }
                          }
                      });
        m_tickProfiler.addPhaseSample(TickPhase::Ai,
                                      std::chrono::duration<double, std::milli>(m_clock->now() - tAiStart).count());
    }

    // Integrate pass: step each FlightIntegrator. Each worker writes only its own entity's state.
    {
        const auto tIntStart = m_clock->now();
        runEntityPass(m_stepItems.size(), [this, tickIndex, simDt](size_t b, size_t e) {
            for (size_t i = b; i < e; ++i) {
                const StepItem& it = m_stepItems[i];
                stepFlightSim(*it.ce->sim, *it.state, m_stepInputs[i], simDt, it.idx, tickIndex);
            }
        });
        m_tickProfiler.addPhaseSample(TickPhase::Integrate,
                                      std::chrono::duration<double, std::milli>(m_clock->now() - tIntStart).count());
    }

    // Cache the representative entity XZ for main-thread terrain streaming (single-player).
    updateTerrainSteerCache();

    // Diagnostics on the sim thread (after both passes, never from a worker): NaN/Inf detection and
    // the periodic trajectory trace.
    for (const StepItem& it : m_stepItems) {
        const FlightState& fs = it.ce->sim->state();
        const bool badPos =
            !std::isfinite(fs.pos_world[0]) || !std::isfinite(fs.pos_world[1]) || !std::isfinite(fs.pos_world[2]);
        const bool badVel =
            !std::isfinite(fs.vel_body[0]) || !std::isfinite(fs.vel_body[1]) || !std::isfinite(fs.vel_body[2]);
        if (badPos || badVel) {
            char msg[256];
            std::snprintf(
                msg, sizeof(msg), "[flight entity=%u] NaN/Inf — pos=(%.3g,%.3g,%.3g) vel_body=(%.3g,%.3g,%.3g)", it.idx,
                fs.pos_world[0], fs.pos_world[1], fs.pos_world[2], fs.vel_body[0], fs.vel_body[1], fs.vel_body[2]);
            m_logger.log(LogLevel::Error, __FILE__, __LINE__, msg);
        }
        // Periodic state trace: once per second (60 Hz sim) for trajectory diagnostics.
        if (tickIndex % 60 == 0) {
            char msg[256];
            std::snprintf(msg, sizeof(msg),
                          "[flight entity=%u] tick=%llu pos=(%.1f,%.1f,%.1f) vel_body=(%.1f,%.1f,%.1f) thr=%.0f%%",
                          it.idx, static_cast<unsigned long long>(tickIndex), fs.pos_world[0], fs.pos_world[1],
                          fs.pos_world[2], fs.vel_body[0], fs.vel_body[1], fs.vel_body[2], fs.throttle_actual * 100.f);
            m_logger.log(LogLevel::Trace, __FILE__, __LINE__, msg);
        }
    }

    const auto tCollisionStart = m_clock->now();
    m_entityManager.onTick(simDt, tickIndex);
    m_tickProfiler.addPhaseSample(TickPhase::Collision,
                                  std::chrono::duration<double, std::milli>(m_clock->now() - tCollisionStart).count());

    // Serialize phase: telemetry, snapshot assembly + send, weather, and shutdown notices.
    const auto tSerializeStart = m_clock->now();

    // Build per-peer world snapshots with interest management and delta compression.
    //
    // Step 1: build telemetry from flight integrators (same as before).
    struct TelemetryEntry {
        uint8_t throttle;
        uint8_t fuelPct;
        uint8_t abEngaged;
        uint8_t engineFailFlags;
        float omega[3]; // body-frame angular rates p,q,r (rad/s)
    };
    std::unordered_map<uint32_t, TelemetryEntry> entityTelemetry;
    for (auto& [entityIdx, ce] : m_controlledEntities) {
        const auto& s = ce.sim->state();
        entityTelemetry[entityIdx] = {static_cast<uint8_t>(s.throttle_actual * 100.f),
                                      static_cast<uint8_t>(std::clamp(s.fuel_kg / 4000.f * 100.f, 0.f, 100.f)),
                                      static_cast<uint8_t>(s.ab_engaged ? 1u : 0u),
                                      s.engineFailFlags,
                                      {s.omega[0], s.omega[1], s.omega[2]}};
    }

    // Step 2: build entity snapshot map — one pass shared across all per-peer loops.
    struct EntitySnap {
        const EntityState* state;
        uint8_t throttle;
        uint8_t fuelPct;
        uint8_t abEngaged;
        uint8_t engineFailFlags;
        float omega[3]; // body-frame angular rates p,q,r (rad/s)
    };
    std::unordered_map<uint32_t, EntitySnap> snapMap;
    snapMap.reserve(m_spatialIndex.entityCount());
    m_entityManager.forEach([&](const EntityState& state) {
        auto tit = entityTelemetry.find(state.id.index);
        uint8_t efFlags = (tit != entityTelemetry.end()) ? tit->second.engineFailFlags : 0u;
        if (static_cast<uint8_t>(state.damageLevel) >= 2u)
            efFlags |= fl::kEngineFailGeneric;
        const float* omegaPtr = (tit != entityTelemetry.end()) ? tit->second.omega : nullptr;
        snapMap[state.id.index] = {
            &state,
            (tit != entityTelemetry.end()) ? tit->second.throttle : uint8_t{0},
            (tit != entityTelemetry.end()) ? tit->second.fuelPct : uint8_t{0},
            (tit != entityTelemetry.end()) ? tit->second.abEngaged : uint8_t{0},
            efFlags,
            {omegaPtr ? omegaPtr[0] : 0.f, omegaPtr ? omegaPtr[1] : 0.f, omegaPtr ? omegaPtr[2] : 0.f}};
    });

    // Encode-once (#725): quantize + bit-pack each live entity ONCE this tick, relative to its shared
    // grid origin (SnapshotCodec::originForPos), as a full and a delta blob. Per-peer assembly stitches
    // these blobs by memcpy instead of re-quantizing, so encode is O(entities) not O(peers x visible).
    // Blobs carry no per-peer state (absolute idx, byte-aligned, position relative to the shared
    // origin), so they drop into any peer's stream in any order. The receiving peer's OWN record is the
    // sole exception — it alone carries omega — so it is re-encoded per peer below (one extra encode per
    // peer, negligible). Order-free: each blob is independent, so the map's iteration order is irrelevant
    // (serial-equivalence preserved).
    struct EncodedRecord {
        double origin[3];
        std::vector<uint8_t> fullBlob;
        std::vector<uint8_t> deltaBlob;
    };
    std::unordered_map<uint32_t, EncodedRecord> encoded;
    encoded.reserve(snapMap.size());
    for (const auto& [encIdx, snap] : snapMap) {
        const EntityState& st = *snap.state;
        QuantEntity qe;
        qe.idx = st.id.index;
        qe.gen = st.id.generation;
        qe.typeIndex = st.typeIndex;
        qe.hasOmega = false; // the once-encoded blob never carries omega (own record re-encoded per peer)
        qe.pos[0] = st.transform.pos[0];
        qe.pos[1] = st.transform.pos[1];
        qe.pos[2] = st.transform.pos[2];
        qe.vel[0] = st.transform.vel[0];
        qe.vel[1] = st.transform.vel[1];
        qe.vel[2] = st.transform.vel[2];
        qe.quat[0] = st.transform.quat[0];
        qe.quat[1] = st.transform.quat[1];
        qe.quat[2] = st.transform.quat[2];
        qe.quat[3] = st.transform.quat[3];
        qe.damageLevel = static_cast<uint8_t>(st.damageLevel);
        qe.engineFailFlags = snap.engineFailFlags;
        qe.throttle = snap.throttle;
        qe.fuelPct = snap.fuelPct;
        qe.abEngaged = snap.abEngaged != 0u;
        qe.playerOwned = st.playerOwned;

        EncodedRecord rec;
        originForPos(qe.pos, rec.origin);
        qe.isFull = true;
        encodeStandaloneRecord(rec.fullBlob, qe, rec.origin, /*sendGen=*/true);
        qe.isFull = false;
        encodeStandaloneRecord(rec.deltaBlob, qe, rec.origin, /*sendGen=*/false);
        encoded.emplace(encIdx, std::move(rec));
    }

    const auto activePeers =
        static_cast<uint16_t>(std::max(0, std::min(m_activePeerCount.load(std::memory_order_relaxed), 65535)));

    // Client-acked delta baselines with selective-ack precision (#566): a record is `full` (carries
    // typeIndex + gen) when the peer has not seen this entity/gen before, the generation changed, the
    // peer has not confirmed it DECODED the tick this full streak started on (fullStreakTick), or the
    // peer was not sent it within kSnapshotRetentionTicks (it may have time-evicted it, so a delta
    // would be undecodable); otherwise a delta. Pure — captures nothing.
    //
    // ackReceived() consults the peer's selective-ack window (high-water ackedTick + ackMask bitmask):
    // it confirms delivery of the SPECIFIC fullStreakTick rather than a high-water mark, closing the
    // #517 residual where acking a later tick could falsely confirm a full the client never decoded.
    // (An existing rec always has a real fullStreakTick: the first send for any entity is a full,
    // which seeds it; rec == nullptr is the "never" case.)
    auto decideFull = [](const PeerEntityRec* rec, uint16_t gen, uint64_t ackedTick, uint32_t ackMask,
                         uint64_t T) -> bool {
        return rec == nullptr || rec->gen != gen || !ackReceived(ackedTick, ackMask, rec->fullStreakTick) ||
               (T - rec->lastSentTick) >= kSnapshotRetentionTicks;
    };

    // Step 3: per-peer snapshot — interest filter (queryRadius) + client-acked delta compression.
    //
    // Serial gather: resolve each sending peer's stable per-peer state pointers once. The operator[]
    // insertions (and any rehash) happen here on the sim thread, never inside the parallel build, so
    // the workers see a frozen map structure (unordered_map keeps element pointers valid across later
    // rehashes). Decimated peers (#518) are excluded from the work set — a decimated tick mutates no
    // per-peer state, exactly matching the previous in-loop `continue`.
    // Compose the per-peer congestion send-interval with the server-wide overrun-governor interval
    // (#514): a peer is decimated by whichever lever spaces it out more. The governor-scaled static
    // budget below is likewise the per-client budget after server-wide overrun shedding, fed into each
    // peer's congestion budget lever. Both governor values are frozen sim-thread locals — the parallel
    // build region never touches the governor object.
    const uint32_t govStaticBudget =
        m_tickGovernor.effectiveBudget(m_snapshotBudgetBytes.load(std::memory_order_relaxed));
    // Overrun interest-radius lever (#726): scale the per-peer interest radius by the governor's
    // frozen interestScale — the only lever that shrinks the visible set itself (the input to the
    // interest query, scheduler ranking, and encode) rather than trimming the encoded output after
    // ranking. A pure function of loadFactor, uniform across peers and frozen here BEFORE the
    // parallel build region, so the peer pass stays serial-equivalent by construction. Entities
    // leaving the shrunk radius are ordinary interest-out (client retention + the
    // kSnapshotRetentionTicks force-full backstop handle re-entry) — never despawned, no wire
    // change. Healthy / disabled governor => interestScale == 1 => the exact configured radius.
    const double govInterestRadiusM = m_drawDistanceM * static_cast<double>(govInterestScale);
    // Snapshot payload compression (#775): frozen before the parallel region like the governor
    // levers, so every peer in this tick sees the same setting.
    const bool compressSnap = m_compressSnapshots.load(std::memory_order_relaxed);
    m_peerWork.clear();
    for (auto& [peerId, peerEid] : m_peerEntities) {
        PeerInputState& pin = m_peerInputs[peerId];
        const uint32_t sendInterval = std::max(pin.congestion.sendIntervalTicks(), govSnapInterval);
        if (pin.sentSnapshot && tickIndex - pin.lastSnapshotSentTick < sendInterval)
            continue; // adaptive send-rate decimation: too few ticks since the last send
        PeerSnapWork w;
        w.peerId = peerId;
        w.peerEid = peerEid;
        w.pin = &pin;
        w.peerState = m_entityManager.get(peerEid);
        w.knownGens = &m_peerKnownGens[peerId];
        w.pending = &m_peerPendingDespawn[peerId];
        m_peerWork.push_back(std::move(w));
    }

    // Parallel build: each worker assembles one peer's snapshot into its own w.buf and mutates only
    // that peer's private state (knownGens GC/records, pending despawns, pin EWMA-free fields). Shared
    // reads (snapMap, m_spatialIndex, m_entityManager.get, m_drawDistanceM, the frozen
    // govInterestRadiusM local, m_snapshotBudgetBytes, m_schedulerWeights, m_congestionParams) are
    // read-only for the whole region. No m_net.send here —
    // the ENetHost is sim-thread-owned, so the actual send + send-cadence bookkeeping is the serial
    // flush below.
    runPeerPass(m_peerWork.size(), [&](std::size_t wbegin, std::size_t wend) {
        for (std::size_t wi = wbegin; wi < wend; ++wi) {
            PeerSnapWork& w = m_peerWork[wi];
            const EntityId peerEid = w.peerEid;
            PeerInputState& pin = *w.pin;
            const EntityState* peerState = w.peerState;
            auto& knownGens = *w.knownGens;
            const uint64_t peerAckedTick = pin.ackedTick;
            const uint32_t peerAckMask = pin.ackMask;

            // Confirmed-despawn detection (#516) + GC prune, in one pass over the known set:
            //   * Absent from the live snapMap → removed from the sim entirely (kill/despawn). Queue an
            //     explicit despawn so the client drops it promptly rather than waiting out the retention
            //     timeout, and erase it from the known set.
            //   * Present but not sent within kSnapshotRetentionTicks → the client has already
            //     time-evicted it (interest-out), so prune the record to bound the map (replacing the GC
            //     the removed periodic baseline clear used to provide). No despawn TLV — it's a timeout,
            //     not a kill; a re-entry is force-fulled by the retention clause in decideFull().
            {
                auto& pending = *w.pending;
                for (auto it = knownGens.begin(); it != knownGens.end();) {
                    if (snapMap.find(it->first) == snapMap.end()) {
                        pending[it->first] = kDespawnRepeatTicks;
                        it = knownGens.erase(it);
                    } else if (tickIndex - it->second.lastSentTick >= kSnapshotRetentionTicks) {
                        it = knownGens.erase(it);
                    } else {
                        ++it;
                    }
                }
            }

            std::vector<uint8_t>& buf = w.buf; // reused scratch, retains capacity across ticks
            buf.clear();
            buf.reserve(sizeof(MsgWorldSnapshotHeader) + 256);

            MsgWorldSnapshotHeader hdr;
            hdr.msgId = static_cast<uint8_t>(MsgId::WorldSnapshot);
            hdr.protocolVersion = static_cast<uint8_t>(kProtocolVersion);
            hdr.recordCount = 0;
            hdr.bitstreamBytes = 0;
            hdr.tickIndex = tickIndex;
            hdr.originCount = 0; // shared-origin table (#725); filled after the stitch loop below
            const std::size_t hdrOffset = buf.size();
            appendMsg(buf, hdr); // placeholder; recordCount/originCount/bitstreamBytes patched below

            // Collect visible entity indices via the spatial index (conservative XZ cells), then apply
            // an exact 3D (XYZ) distance gate (#402) and sort ascending so the bitstream's idx deltas
            // stay small. Both bounds use the governor-scaled interest radius (#726 — frozen before
            // this parallel region). peerState null/dead → empty list → header-only empty snapshot.
            std::vector<uint32_t> visible;
            if (peerState && !peerState->dead && govInterestRadiusM > 0.0) {
                const double r2 = govInterestRadiusM * govInterestRadiusM;
                const double px = peerState->transform.pos[0];
                const double py = peerState->transform.pos[1];
                const double pz = peerState->transform.pos[2];
                m_spatialIndex.queryRadius(peerState->transform.pos, govInterestRadiusM,
                                           [&](uint32_t entityIdx, const double* pos) {
                                               if (snapMap.find(entityIdx) == snapMap.end())
                                                   return; // died this tick after the index was built
                                               const double dx = pos[0] - px, dy = pos[1] - py, dz = pos[2] - pz;
                                               if (dx * dx + dy * dy + dz * dz > r2)
                                                   return; // 3D interest cull (#402)
                                               visible.push_back(entityIdx);
                                           });
                std::sort(visible.begin(), visible.end());
            }

            // Priority/budget scheduling (#516). When a per-client byte budget is set, rank the visible
            // entities by relevance (distance / closing-speed / recency / player-owned) and keep only the
            // highest-priority set that fits; the rest are deferred to a later tick. budget == 0 keeps the
            // legacy behaviour (every visible entity, ascending idx). The own entity is always admitted.
            std::vector<uint32_t> selected;
            // Congestion response (#518): scale the static byte budget by this peer's congestion throttle.
            // A static budget of 0 (unlimited) stays 0 here — under congestion only the send-rate lever
            // applies for unlimited-budget servers.
            // govStaticBudget is the static per-client budget after server-wide overrun shedding (#514);
            // the per-peer congestion lever (#518) then scales it further for this peer.
            const uint32_t budget = pin.congestion.effectiveBudget(govStaticBudget);
            if (budget == 0u || visible.size() <= 1u) {
                selected = visible;
            } else {
                // Reserve fixed overhead (header + TLV block) out of the budget for the record bitstream.
                constexpr uint32_t kFixedOverhead = sizeof(MsgWorldSnapshotHeader) + 32u;
                const uint32_t recordBudget = budget > kFixedOverhead ? budget - kFixedOverhead : 1u;
                const double px = peerState->transform.pos[0];
                const double py = peerState->transform.pos[1];
                const double pz = peerState->transform.pos[2];
                std::vector<SnapshotCandidate> cands;
                cands.reserve(visible.size());
                for (uint32_t idx : visible) {
                    const EntitySnap& snap = snapMap.at(idx);
                    const EntityState& st = *snap.state;
                    SnapshotCandidate c;
                    c.idx = idx;
                    const double dx = st.transform.pos[0] - px, dy = st.transform.pos[1] - py,
                                 dz = st.transform.pos[2] - pz;
                    c.distSq = dx * dx + dy * dy + dz * dz;
                    // Closing speed: range rate toward the peer (positive = approaching). r_hat points
                    // peer→entity; closing = dot(peerVel - entityVel, r_hat).
                    const double dist = std::sqrt(c.distSq);
                    if (dist > 1e-3) {
                        const double rx = dx / dist, ry = dy / dist, rz = dz / dist;
                        const double rvx = static_cast<double>(peerState->transform.vel[0]) - st.transform.vel[0];
                        const double rvy = static_cast<double>(peerState->transform.vel[1]) - st.transform.vel[1];
                        const double rvz = static_cast<double>(peerState->transform.vel[2]) - st.transform.vel[2];
                        c.closingSpeed = static_cast<float>(rvx * rx + rvy * ry + rvz * rz);
                    }
                    c.isOwn = (st.id.index == peerEid.index && st.id.generation == peerEid.generation);
                    c.playerOwned = st.playerOwned;
                    const uint16_t gen = static_cast<uint16_t>(st.id.generation);
                    auto kit = knownGens.find(idx);
                    const PeerEntityRec* rec = (kit == knownGens.end()) ? nullptr : &kit->second;
                    c.ticksSinceSent = (rec == nullptr) ? UINT64_MAX : (tickIndex - rec->lastSentTick);
                    const bool isFull = decideFull(rec, gen, peerAckedTick, peerAckMask, tickIndex);
                    // Absolute idx (#725) + a conservative 1-byte origin index; the real origin index
                    // isn't known until the per-peer origin table is built after selection, but the budget
                    // is a soft cap so a per-record ±1 byte is acceptable.
                    c.estBytes = estimateRecordBytes(isFull, isFull, c.isOwn, st.typeIndex, /*entityIndex=*/idx,
                                                     /*originIndex=*/1u);
                    cands.push_back(c);
                }
                selected = selectSnapshotRecords(cands, recordBudget, m_schedulerWeights, m_drawDistanceM);
                std::sort(selected.begin(), selected.end()); // ascending for the codec's idx-delta varints

                // No deferral guard is needed under selective-ack (#566): a scheduler-withheld entity is
                // not SENT this tick, so its fullStreakTick keeps its earlier value; the peer's ack of
                // this tick sets only this tick's bit and cannot confirm that earlier fullStreakTick.
                // decideFull() confirms the specific full-sent tick, so the #517 streak-bump workaround
                // (which the high-water mark required) is now redundant.
            }

            // Assemble the stitched record stream (#725): for each selected entity, pick its pre-encoded
            // blob (full or delta per decideFull), record its shared origin into this peer's origin table
            // (deduped), and stitch [origin index][blob]. The peer's OWN entity is the one per-peer record
            // — re-encoded here with omega. Records are byte-aligned, so the stitch is a memcpy, not a
            // re-quantization. Deterministic (selected is sorted; origin table is first-seen order) so the
            // per-peer buffer is byte-identical across worker counts (serial-equivalence, #512).
            std::vector<std::array<double, 3>> originTable;
            std::vector<uint8_t> recordStream;
            auto originIndexOf = [&originTable](const double o[3]) -> uint32_t {
                for (uint32_t i = 0; i < originTable.size(); ++i)
                    if (originTable[i][0] == o[0] && originTable[i][1] == o[1] && originTable[i][2] == o[2])
                        return i;
                originTable.push_back({o[0], o[1], o[2]});
                return static_cast<uint32_t>(originTable.size() - 1u);
            };

            std::vector<uint8_t> ownBlob; // scratch for the (single) own-entity re-encode
            for (uint32_t idx : selected) {
                const EntitySnap& snap = snapMap.at(idx);
                const EntityState& state = *snap.state;
                const uint16_t gen = static_cast<uint16_t>(state.id.generation);
                auto kit = knownGens.find(idx);
                const PeerEntityRec* rec = (kit == knownGens.end()) ? nullptr : &kit->second;
                const bool isFull = decideFull(rec, gen, peerAckedTick, peerAckMask, tickIndex);
                const bool isOwn = (state.id.index == peerEid.index && state.id.generation == peerEid.generation);

                double recOrigin[3];
                const std::vector<uint8_t>* blob = nullptr;
                if (isOwn) {
                    QuantEntity qe;
                    qe.idx = state.id.index;
                    qe.gen = state.id.generation;
                    qe.typeIndex = state.typeIndex;
                    qe.isFull = isFull;
                    qe.hasOmega = true; // the own record alone carries omega
                    qe.pos[0] = state.transform.pos[0];
                    qe.pos[1] = state.transform.pos[1];
                    qe.pos[2] = state.transform.pos[2];
                    qe.vel[0] = state.transform.vel[0];
                    qe.vel[1] = state.transform.vel[1];
                    qe.vel[2] = state.transform.vel[2];
                    qe.quat[0] = state.transform.quat[0];
                    qe.quat[1] = state.transform.quat[1];
                    qe.quat[2] = state.transform.quat[2];
                    qe.quat[3] = state.transform.quat[3];
                    qe.omega[0] = snap.omega[0];
                    qe.omega[1] = snap.omega[1];
                    qe.omega[2] = snap.omega[2];
                    qe.damageLevel = static_cast<uint8_t>(state.damageLevel);
                    qe.engineFailFlags = snap.engineFailFlags;
                    qe.throttle = snap.throttle;
                    qe.fuelPct = snap.fuelPct;
                    qe.abEngaged = snap.abEngaged != 0u;
                    qe.playerOwned = state.playerOwned;
                    originForPos(qe.pos, recOrigin);
                    ownBlob.clear();
                    encodeStandaloneRecord(ownBlob, qe, recOrigin, /*sendGen=*/isFull);
                    blob = &ownBlob;
                } else {
                    const EncodedRecord& er = encoded.at(idx);
                    recOrigin[0] = er.origin[0];
                    recOrigin[1] = er.origin[1];
                    recOrigin[2] = er.origin[2];
                    blob = isFull ? &er.fullBlob : &er.deltaBlob;
                }
                appendStitchedRecord(recordStream, originIndexOf(recOrigin), *blob);

                // Record what we sent. Freeze fullStreakTick at the start of a contiguous run of fulls
                // (same entity, full last tick, sent consecutively) so the client only has to ack the
                // streak start to converge to deltas; a send gap (deferral) or a prior delta restarts it.
                PeerEntityRec& r = knownGens[idx];
                if (isFull) {
                    const bool contiguous = r.lastWasFull && r.lastSentTick + 1 == tickIndex;
                    r.fullStreakTick = contiguous ? r.fullStreakTick : tickIndex;
                }
                r.gen = gen;
                r.lastSentTick = tickIndex;
                r.lastWasFull = isFull;
                ++hdr.recordCount;
            }

            // Body layout: [origin table: originCount x double[3]][stitched record stream]. Append both
            // after the header placeholder, then patch the header counts.
            hdr.originCount = static_cast<uint16_t>(originTable.size());
            for (const auto& o : originTable) {
                const auto* p = reinterpret_cast<const uint8_t*>(o.data());
                buf.insert(buf.end(), p, p + 3u * sizeof(double));
            }
            buf.insert(buf.end(), recordStream.begin(), recordStream.end());
            hdr.bitstreamBytes = static_cast<uint32_t>(recordStream.size());
            writeMsgAt(buf, hdrOffset, hdr);

            // TLV extension block.
            appendExt(buf, static_cast<uint16_t>(ExtTag::SnapshotPeerCount), activePeers);
            // Per-peer latency TLVs. Omitted when estimatedDelayTicks == 0 (e.g. single-player localhost)
            // so the client's m_hasSnapshotLatency stays false and the HUD indicator remains hidden.
            if (pin.estimatedDelayTicks > 0) {
                const auto latMs = static_cast<uint16_t>(
                    std::min(static_cast<uint64_t>(pin.estimatedDelayTicks) * 1000u / 60u, uint64_t{65535u}));
                appendExt(buf, static_cast<uint16_t>(ExtTag::SnapshotPeerLatency), latMs);
                const auto delayTicks = static_cast<uint16_t>(std::min(pin.estimatedDelayTicks, uint32_t{65535u}));
                appendExt(buf, static_cast<uint16_t>(ExtTag::SnapshotPeerDelayTicks), delayTicks);
            }
            // Explicit despawn TLV (#516): indices the peer knew that left the sim. Repeated for a few
            // ticks (drop tolerance on the unreliable channel), decrementing each entry's remaining count.
            if (auto& pendingDespawn = *w.pending; !pendingDespawn.empty()) {
                std::vector<uint32_t> ids;
                ids.reserve(pendingDespawn.size());
                for (auto it = pendingDespawn.begin(); it != pendingDespawn.end();) {
                    ids.push_back(it->first);
                    if (--(it->second) == 0u)
                        it = pendingDespawn.erase(it);
                    else
                        ++it;
                }
                appendExtRaw(buf, static_cast<uint16_t>(ExtTag::SnapshotDespawn), ids.data(),
                             static_cast<uint16_t>(ids.size() * sizeof(uint32_t)));
            }
            // Snapshot payload compression (#775): zstd the fully-assembled payload (origin table +
            // record stream + TLV block) in place, still inside the parallel per-peer region — the
            // codec is deterministic and this worker owns buf, so the #512 byte-identical-across-
            // worker-counts guarantee holds. Raw fallback when compression does not strictly win
            // (tiny/incompressible payloads keep flags == 0 and are wire-identical to compression
            // off). The 24-byte header always stays raw: dispatch, the client's tick dedup, and
            // bot_swarm's metrics read it without decompressing.
            if (const std::size_t payloadSize = buf.size() - sizeof(MsgWorldSnapshotHeader);
                compressSnap && payloadSize <= kMaxSnapshotPayloadBytes) {
                const std::size_t csz = compressSnapshotPayload(buf.data() + sizeof(MsgWorldSnapshotHeader),
                                                                payloadSize, w.compressScratch);
                if (csz > 0u) {
                    hdr.flags |= kSnapshotFlagCompressed;
                    hdr.uncompressedBytes = static_cast<uint32_t>(payloadSize);
                    writeMsgAt(buf, hdrOffset, hdr);
                    buf.resize(sizeof(MsgWorldSnapshotHeader) + csz);
                    std::memcpy(buf.data() + sizeof(MsgWorldSnapshotHeader), w.compressScratch.data(), csz);
                }
            }
            // No m_net.send here — buf is flushed by the sim thread below.
        }
    });

    // Serial flush (sim thread): send each built buffer over the sim-thread-owned ENetHost and record
    // the send-cadence bookkeeping the decimation gate reads next tick (#518). Empty work entries are
    // peers that were decimated this tick (excluded in the gather) and are simply not present.
    for (PeerSnapWork& w : m_peerWork) {
        m_net.send(w.peerId, w.buf.data(), w.buf.size(), /*reliable=*/false);
        w.pin->lastSnapshotSentTick = tickIndex;
        w.pin->sentSnapshot = true;
    }

    // Tick weather and broadcast MsgWeatherState every 10 ticks (~6 Hz at 60 Hz sim).
    if (m_weather) {
        m_weather->advance(simDt);
        ++m_weatherBroadcastTick;
        if (m_weatherBroadcastTick % 10 == 0) {
            const EnvironmentState env = m_weather->computeEnvironment();
            MsgWeatherState ws;
            ws.msgId = static_cast<uint8_t>(MsgId::WeatherState);
            ws.preset = static_cast<uint8_t>(m_weather->preset());
            auto tod = m_weather->timeOfDay();
            ws.timeOfDayTenths = static_cast<uint16_t>(tod * 10.f);
            ws.fogDensity = env.fogDensity;
            ws.fogStartDist = env.fogStartDist;
            ws.windX = env.windX;
            ws.windZ = env.windZ;
            m_net.broadcast(&ws, sizeof(ws), /*reliable=*/false);
        }
    }

    // Shutdown countdown: fire at each interval and at T=0.
    if (m_shuttingDown) {
        using namespace std::chrono;
        auto now = m_clock->now();
        if (now >= m_shutdownAt) {
            broadcastShutdownNotice(0, makeShutdownMessage(0, m_shutdownReason).c_str());
            m_shuttingDown = false;
            if (m_shutdownCallback)
                m_shutdownCallback();
        } else if (now >= m_nextNoticeAt) {
            auto secsLeft = static_cast<uint32_t>(duration_cast<seconds>(m_shutdownAt - now).count());
            broadcastShutdownNotice(static_cast<uint16_t>(secsLeft),
                                    makeShutdownMessage(secsLeft, m_shutdownReason).c_str());
            // Always squeeze in a T-60s notice: if the next interval would skip past it, clamp.
            auto nextInterval = now + seconds(m_warningIntervalS);
            auto oneMinBefore = m_shutdownAt - seconds(60);
            m_nextNoticeAt = (nextInterval > oneMinBefore && oneMinBefore > now) ? oneMinBefore : nextInterval;
        }
    }

    m_net.service(0);

    m_tickProfiler.addPhaseSample(TickPhase::Serialize,
                                  std::chrono::duration<double, std::milli>(m_clock->now() - tSerializeStart).count());
    m_tickProfiler.endTick();
}

void WorldBroadcaster::onConnect(uint32_t peerId) {
    // Rejection gauntlet — each check logs, sends a MsgConnectRefusal with the matching reason,
    // and disconnects via rejectConnection(). Order matters: cheapest/most-decisive checks first.
    std::string ip = extractIp(m_net.getPeerAddress(peerId));

    // Ban check — reject banned IPs before any state is created.
    if (!ip.empty() && m_bannedAddresses.count(ip)) {
        rejectConnection(peerId, ip, ConnectRefusalCode::Banned);
        return;
    }

    // Allowlist check — if non-empty, only listed IPs may connect.
    if (!ip.empty() && !m_allowedAddresses.empty() && !m_allowedAddresses.count(ip)) {
        rejectConnection(peerId, ip, ConnectRefusalCode::AccessDenied);
        return;
    }

    // Connection rate limit — sliding window per IP.
    if (!ip.empty()) {
        auto now = m_clock->now();
        auto& rec = m_connectRecords[ip];
        auto cutoff = now - std::chrono::seconds(m_connectRateWindowS);
        while (!rec.timestamps.empty() && rec.timestamps.front() < cutoff)
            rec.timestamps.pop_front();
        rec.timestamps.push_back(now);
        if (static_cast<int>(rec.timestamps.size()) > m_connectRateLimit) {
            rejectConnection(peerId, ip, ConnectRefusalCode::RateLimited);
            return;
        }
    }

    // Per-IP concurrent connection limit.
    if (m_maxConnectionsPerIp > 0 && !ip.empty()) {
        int count = 0;
        for (const auto& [pid, eid] : m_peerEntities)
            if (extractIp(m_net.getPeerAddress(pid)) == ip)
                ++count;
        if (count >= m_maxConnectionsPerIp) {
            rejectConnection(peerId, ip, ConnectRefusalCode::TooManyConnections);
            return;
        }
    }

    // Admin auth lockout — refuse reconnections from IPs with an active lockout.
    if (!ip.empty() && m_adminAuthTracker.isLockedOut(ip)) {
        rejectConnection(peerId, ip, ConnectRefusalCode::AdminLockout);
        return;
    }

    char msg[64];
    std::snprintf(msg, sizeof(msg), "peer %u connected", peerId);
    m_logger.log(LogLevel::Info, __FILE__, __LINE__, msg);

    MsgHello hello;
    m_net.send(peerId, &hello, sizeof(hello), /*reliable=*/true);

    EntityTransform t{};
    t.quat[3] = 1.0f; // identity quaternion (w component; XYZW layout)
    if (!m_spawnPoints.empty()) {
        // Explicit cast avoids uint32_t/size_t width mismatch warning on MSVC (/W4 → error).
        const std::size_t idx = static_cast<std::size_t>(m_nextSpawnIdx++) % m_spawnPoints.size();
        t.pos[0] = m_spawnPoints[idx][0];
        t.pos[1] = m_spawnPoints[idx][1];
        t.pos[2] = m_spawnPoints[idx][2];
    } else {
        constexpr double kSpawnAGL = 500.0;
        t.pos[0] = 0.0;
        t.pos[2] = 60.0; // 60 m ahead of origin so peer doesn't overlap sandbox entity 0
        t.pos[1] = static_cast<double>(m_groundElevation.load(std::memory_order_relaxed)) + kSpawnAGL;
    }
    EntityId id = m_entityManager.spawn("builtin:debug-entity", t, peerId);
    if (id.valid()) {
        m_peerEntities[peerId] = id;
        m_peerInputs[peerId] = {};
        m_peerInputs[peerId].lastActivityTick = m_currentTick;

        // Stamp the player's faction. Without a non-zero faction the player is NEUTRAL, and
        // fl::areFactionsHostile gives a neutral entity no enemies at all — so nothing would be
        // hostile to them, their wingman's engage/cover conditions could never fire, and boresight
        // designation could never designate. Setting this to 0 restores the pre-#610 behavior.
        if (EntityState* s = m_entityManager.get(id); s && m_playerFaction != 0) {
            s->factionIndex = m_playerFaction;
        }

        // Resolve the entity type's flight model (server-authoritative; never sent on the wire).
        // Empty id, no resolver, or an unknown id falls back to the builtin UFO model.
        std::shared_ptr<const FlightModelData> model = resolveFlightModel(id);

        // PeerController reads the peer's stable input slot (pointer valid across rehash, slot torn
        // down after the controller on disconnect). Start at throttle 0 so the entity is stationary.
        // decimatable=false: a player's input must be sampled every tick for responsiveness (#514).
        addControlledEntity(id, std::make_unique<PeerController>(&m_peerInputs[peerId]), std::move(model), 0.0f,
                            /*decimatable=*/false);
    }
    sendConnectAck(peerId, id);
    if (!m_motd.empty()) {
        const std::size_t textLen = std::min(m_motd.size(), kMaxMotdBytes);
        MsgMotdHeader mhdr{};
        mhdr.displaySeconds = m_motdDisplaySeconds;
        std::vector<uint8_t> pkt;
        pkt.reserve(sizeof(MsgMotdHeader) + textLen + 1);
        appendMsg(pkt, mhdr);
        pkt.insert(pkt.end(), m_motd.c_str(), m_motd.c_str() + textLen);
        pkt.push_back(0u); // NUL terminator
        m_net.send(peerId, pkt.data(), pkt.size(), /*reliable=*/true);
    }

    // Form this peer's flight (#610). The spawner lives in fl-server (it needs engine-ai to build
    // controllers); with no spawner installed the peer simply flies alone, which is exactly the
    // pre-#610 behavior.
    if (id.valid() && m_flightSpawner) {
        const fl::FormationId fid = m_flightSpawner(peerId, id);
        if (const fl::Formation* f = m_formations.get(fid)) {
            // Unsolicited check-in: this is how the client learns it HAS a flight, how big it is, and
            // what id to address it by. MsgConnectAck cannot carry it — it is immediately followed by
            // MsgEntityTypeDef records, so appending a field there would shift them.
            MsgWingmanAck ack{};
            ack.command = static_cast<uint8_t>(fl::ai::WingmanCommand::Rejoin);
            ack.result = static_cast<uint8_t>(WingmanResult::CheckIn);
            ack.flightSize = static_cast<uint8_t>(std::min<std::size_t>(f->members.size(), 255));
            ack.memberIdx = f->members.empty() ? kFlightAll : f->members.front().id.index;
            ack.flightId = fid;
            m_net.send(peerId, &ack, sizeof(ack), /*reliable=*/true);
        }
    }

    m_activePeerCount.fetch_add(1, std::memory_order_relaxed);
}

void WorldBroadcaster::onDisconnect(uint32_t peerId) {
    char msg[64];
    std::snprintf(msg, sizeof(msg), "peer %u disconnected", peerId);
    m_logger.log(LogLevel::Info, __FILE__, __LINE__, msg);

    auto it = m_peerEntities.find(peerId);
    if (it != m_peerEntities.end()) {
        // Tear down this peer's flight (#610) before its own entity goes away.
        //
        // The formation the peer ANCHORED (their own flight) is destroyed with its AI members: those
        // aircraft exist to fly on that player, and leaving them holding station on a dead anchor
        // would leak an entity and a controller per disconnect. Formations the peer merely COMMANDED
        // from outside (an AWACS's AI flights) survive — the aircraft are still flying, and the game
        // master can still order them; releasePeer just clears the command role.
        const fl::FormationId owned = m_formations.formationAnchoredOn(it->second);
        if (const fl::Formation* f = m_formations.get(owned)) {
            const std::vector<fl::FormationMember> members = f->members;
            for (const fl::FormationMember& m : members) {
                if (m.isAi()) {
                    m_controlledEntities.erase(m.id.index);
                    m_sensorSystem.removeObserver(m.id.index);
                    m_entityManager.kill(m.id);
                }
            }
            m_formations.destroy(owned);
        }
        // Remove the peer's own aircraft from any formation it was flying IN as a member (a human
        // wingman in someone else's flight), and drop its command role everywhere.
        m_formations.removeEntity(it->second);
        m_formations.releasePeer(peerId);

        // Tear down the controller (which points into m_peerInputs) before erasing the input slot.
        m_controlledEntities.erase(it->second.index);
        m_sensorSystem.removeObserver(it->second.index);
        m_entityManager.kill(it->second);
        m_peerEntities.erase(it);
    }
    m_peerInputs.erase(peerId);
    m_peerFloodState.erase(peerId);
    m_peerKnownGens.erase(peerId);
    m_peerPendingDespawn.erase(peerId);
    m_peerTraceWriters.erase(peerId); // close this peer's input trace (#560), if any
    m_activePeerCount.fetch_sub(1, std::memory_order_relaxed);

    m_pendingAdminDrains.erase(std::remove_if(m_pendingAdminDrains.begin(), m_pendingAdminDrains.end(),
                                              [peerId](const PendingAdminDrain& d) { return d.peerId == peerId; }),
                               m_pendingAdminDrains.end());
}

void WorldBroadcaster::onReceive(uint32_t peerId, const void* data, std::size_t size) {
    if (size < 1)
        return;
    uint8_t msgId;
    std::memcpy(&msgId, data, 1);

    if (msgId == static_cast<uint8_t>(MsgId::ClientInput)) {
        if (size < sizeof(MsgClientInput))
            return; // truncated; silently discard

        MsgClientInput msg;
        std::memcpy(&msg, data, sizeof(msg));

        if (msg.protocolVersion != kProtocolVersion) {
            char vmsg[96];
            std::snprintf(vmsg, sizeof(vmsg), "peer %u: ClientInput version mismatch (got %u, want %u) — discarding",
                          peerId, static_cast<unsigned>(msg.protocolVersion), static_cast<unsigned>(kProtocolVersion));
            m_logger.log(LogLevel::Warn, __FILE__, __LINE__, vmsg);
            return;
        }

        // Packet flood detection: disconnect peers that send faster than multiplier * tick rate.
        {
            auto& flood = m_peerFloodState[peerId];
            auto now = m_clock->now();
            if (now - flood.windowStart >= std::chrono::seconds(1)) {
                flood.windowStart = now;
                flood.packetCount = 0;
            }
            ++flood.packetCount;
            if (flood.packetCount > static_cast<uint32_t>(60 * m_floodMultiplier)) {
                char fmsg[96];
                std::snprintf(fmsg, sizeof(fmsg), "peer %u flooding — %u packets/s — disconnecting", peerId,
                              flood.packetCount);
                m_logger.log(LogLevel::Warn, __FILE__, __LINE__, fmsg);
                m_net.disconnectPeer(peerId);
                return;
            }
        }

        PeerInputState& stored = m_peerInputs[peerId];

        // Staleness guard: discard out-of-order and duplicate inputs.
        if (stored.hasSeq && !isNewerSeq(msg.seqNum, stored.lastSeqNum))
            return;

        // One-way delay estimate: ticks elapsed since the client last received a snapshot.
        if (msg.tickIndex <= m_currentTick)
            stored.estimatedDelayTicks = static_cast<uint32_t>(m_currentTick - msg.tickIndex);

        // Snapshot ack: the client echoes the last WorldSnapshot tick it processed plus a selective-ack
        // bitmask of recently decoded ticks (#566). Clamp to the present so a client can't ack a future
        // tick. Adopt the high-water tick and its mask together, only on a strict advance, so ackedTick
        // and ackMask always describe the same frame (a stale/reordered input keeps the prior pair).
        {
            const uint64_t ackT = std::min(msg.tickIndex, m_currentTick);
            if (ackT > stored.ackedTick) {
                stored.ackedTick = ackT;
                stored.ackMask = msg.ackMask;
            }
        }

        // On first input, seed the jitter buffer depth from the measured one-way delay,
        // capped at the configured global maximum.
        if (!stored.hasSeq) {
            const uint32_t maxD = m_jitterMaxDepth.load(std::memory_order_relaxed);
            const uint32_t depth = (stored.estimatedDelayTicks > 0u) ? std::min(stored.estimatedDelayTicks, maxD) : 1u;
            stored.jitterBuffer.setMaxDepth(depth);
        }

        // Update per-peer EWMA of one-way delay and inter-arrival jitter.
        // alpha = 1/adaptWindow; seeded on first packet, updated on each subsequent accepted input.
        {
            const float alpha = 1.0f / static_cast<float>(m_jitterAdaptWindow);
            if (!stored.ewmaSeeded) {
                stored.ewmaDelayTicks = static_cast<float>(stored.estimatedDelayTicks);
                stored.ewmaJitterTicks = 0.f;
                stored.lastInputTick = m_currentTick;
                stored.ewmaSeeded = true;
            } else {
                stored.ewmaDelayTicks =
                    alpha * static_cast<float>(stored.estimatedDelayTicks) + (1.f - alpha) * stored.ewmaDelayTicks;
                // RFC 3550 inter-arrival jitter: expected spacing is 1 tick (60 Hz client send rate).
                const uint64_t interArrival =
                    (m_currentTick > stored.lastInputTick) ? m_currentTick - stored.lastInputTick : 0u;
                const float deviation = std::abs(static_cast<float>(interArrival) - 1.f);
                stored.ewmaJitterTicks = alpha * deviation + (1.f - alpha) * stored.ewmaJitterTicks;
                stored.lastInputTick = m_currentTick;
            }
        }

        stored.lastSeqNum = msg.seqNum;
        stored.hasSeq = true;
        stored.lastActivityTick = m_currentTick;

        // Clamp and enqueue into the jitter buffer. Control fields (throttle etc.) are
        // written to stored in onTick when the buffer is drained — not here.
        // Sanitize control floats from an untrusted client: reject NaN/Inf FIRST (std::clamp passes
        // NaN straight through — NaN < lo and hi < NaN are both false), then clamp to the actuator
        // range. An unsanitized NaN would propagate into the flight sim and later trip UB at, e.g.,
        // the float->uint8 throttle telemetry cast.
        auto ctl = [](float v, float lo, float hi) { return std::isfinite(v) ? std::clamp(v, lo, hi) : 0.f; };
        BufferedInput bi;
        bi.throttle = ctl(msg.throttle, 0.f, 1.f);
        bi.elevator = ctl(msg.elevator, -1.f, 1.f);
        bi.aileron = ctl(msg.aileron, -1.f, 1.f);
        bi.rudder = ctl(msg.rudder, -1.f, 1.f);
        bi.buttons = msg.buttons;
        stored.jitterBuffer.push(bi);

        // Server-side input tracing (#560): append the accepted, sanitized control sample to this
        // peer's FLIT trace. The writer is opened lazily so trace_start mid-session captures already
        // connected peers too; tickRate is the fixed 60 Hz server step.
        if (!m_inputTraceDir.empty()) {
            auto& writer = m_peerTraceWriters[peerId];
            if (!writer) {
                char path[512];
                std::snprintf(path, sizeof(path), "%s/trace_peer%u_%u.flit", m_inputTraceDir.c_str(), peerId,
                              m_traceFileSeq++);
                writer = std::make_unique<InputTraceWriter>(std::string(path), 60u);
                if (!writer->good()) {
                    char wmsg[576];
                    std::snprintf(wmsg, sizeof(wmsg), "could not open input trace '%s' for peer %u — not tracing it",
                                  path, peerId);
                    m_logger.log(LogLevel::Warn, __FILE__, __LINE__, wmsg);
                }
            }
            if (writer && writer->good())
                writer->writeRecord(m_currentTick, bi.throttle, bi.elevator, bi.aileron, bi.rudder, bi.buttons);
        }

        // viewAxis is updated immediately — it is camera state, not a flight sim input.
        float vmag = std::sqrt(msg.viewAxis[0] * msg.viewAxis[0] + msg.viewAxis[1] * msg.viewAxis[1] +
                               msg.viewAxis[2] * msg.viewAxis[2]);
        if (vmag > 1e-6f) {
            stored.viewAxis[0] = msg.viewAxis[0] / vmag;
            stored.viewAxis[1] = msg.viewAxis[1] / vmag;
            stored.viewAxis[2] = msg.viewAxis[2] / vmag;
        }
        // else: degenerate viewAxis — retain previous good value
    } else if (msgId == static_cast<uint8_t>(MsgId::AdminCommand)) {
        // Feature gates: both password and dispatcher must be configured.
        if (m_operatorPassword.empty() || !m_adminDispatch)
            return;
        if (size < sizeof(MsgAdminCommand))
            return;

        // Extract IP once — used for both failure and success tracking below.
        std::string adminIp = extractIp(m_net.getPeerAddress(peerId));

        MsgAdminCommand msg;
        std::memcpy(&msg, data, sizeof(msg));
        msg.token[sizeof(msg.token) - 1] = '\0';
        msg.command[sizeof(msg.command) - 1] = '\0';
        uint16_t const reqId = msg.reqId;

        // Constant-time token comparison: XOR-accumulate the full fixed-size token field
        // to avoid a length or early-exit timing oracle.
        {
            const std::string& pw = m_operatorPassword;
            uint8_t diff = 0;
            for (std::size_t i = 0; i < sizeof(msg.token); ++i) {
                uint8_t a = static_cast<uint8_t>(msg.token[i]);
                uint8_t b = (i < pw.size()) ? static_cast<uint8_t>(pw[i]) : 0u;
                diff |= (a ^ b);
            }
            for (std::size_t i = sizeof(msg.token); i < pw.size(); ++i)
                diff |= static_cast<uint8_t>(pw[i]);
            if (diff != 0) {
                char lmsg[96];
                std::snprintf(lmsg, sizeof(lmsg), "peer %u: MsgAdminCommand bad token — discarding", peerId);
                m_logger.log(LogLevel::Warn, __FILE__, __LINE__, lmsg);
                if (!adminIp.empty() && m_adminAuthTracker.recordFailure(adminIp)) {
                    char lk[128];
                    std::snprintf(lk, sizeof(lk), "peer %u (%s): admin auth lockout triggered — kicking", peerId,
                                  adminIp.c_str());
                    m_logger.log(LogLevel::Warn, __FILE__, __LINE__, lk);
                    m_net.disconnectPeer(peerId);
                }
                return;
            }
        }

        std::string_view cmdView(msg.command);
        if (cmdView.empty())
            return;

        // Dispatch on the sim thread (same as stdin admin loop).
        // Mutating commands enqueue via gameLoop.enqueueSimCallback() internally.
        std::string result = m_adminDispatch(cmdView);
        if (!adminIp.empty())
            m_adminAuthTracker.recordSuccess(adminIp);

        {
            char lmsg[256];
            std::snprintf(lmsg, sizeof(lmsg), "peer %u [net-admin] %.*s -> %.*s", peerId,
                          static_cast<int>(cmdView.size()), cmdView.data(),
                          static_cast<int>(std::min(result.size(), std::size_t{80})), result.c_str());
            m_logger.log(LogLevel::Info, __FILE__, __LINE__, lmsg);
        }

        sendAdminResponse(m_net, peerId, reqId, result);

        // Queue a wall-clock-deferred drain: mark taken after dispatch (skips any sync
        // shell.print() calls made during dispatch); fires after kENetAdminDrainDelayMs ms,
        // giving enqueueSimCallback lambdas time to run regardless of tick-batch catch-up.
        if (m_adminShellMark && m_adminShellDrain)
            m_pendingAdminDrains.push_back({peerId, reqId, m_adminShellMark(),
                                            m_clock->now() + std::chrono::milliseconds(kENetAdminDrainDelayMs)});
    } else if (msgId == static_cast<uint8_t>(MsgId::Heartbeat)) {
        MsgHeartbeat hb;
        if (!readMsg(data, size, hb))
            return;
        auto& ps = m_peerInputs[peerId];
        ps.lastActivityTick = m_currentTick;
        if (hb.tickIndex <= m_currentTick)
            ps.estimatedDelayTicks = static_cast<uint32_t>(m_currentTick - hb.tickIndex);
        // Heartbeat also acks the last processed snapshot (idle clients with no MsgClientInput),
        // carrying the same selective-ack bitmask. Adopt tick + mask together on a strict advance.
        {
            const uint64_t ackT = std::min(hb.tickIndex, m_currentTick);
            if (ackT > ps.ackedTick) {
                ps.ackedTick = ackT;
                ps.ackMask = hb.ackMask;
            }
        }

        // Reply with the current delay estimate so the client can display "Ping: N ms".
        MsgPeerDelay pd;
        pd.delayTicks = static_cast<uint16_t>(std::min(ps.estimatedDelayTicks, 65535u));
        m_net.send(peerId, &pd, sizeof(pd), /*reliable=*/false);

    } else if (msgId == static_cast<uint8_t>(MsgId::WingmanCommand)) {
        handleWingmanCommand(peerId, data, size);
    }
    // Unknown msgIds: silently discard (no log spam; future protocol versions may add new IDs)
}

void WorldBroadcaster::sendWingmanAck(uint32_t peerId, uint8_t command, WingmanResult result, uint16_t flightId,
                                      uint8_t flightSize, uint32_t memberIdx, uint32_t targetIdx) {
    MsgWingmanAck ack{};
    ack.command = command;
    ack.result = static_cast<uint8_t>(result);
    ack.flightSize = flightSize;
    ack.memberIdx = memberIdx;
    ack.targetIdx = targetIdx;
    ack.flightId = flightId;
    m_net.send(peerId, &ack, sizeof(ack), /*reliable=*/true);
}

void WorldBroadcaster::handleWingmanCommand(uint32_t peerId, const void* data, std::size_t size) {
    MsgWingmanCommand msg;
    if (!readMsg(data, size, msg))
        return; // truncated; silently discard

    if (msg.protocolVersion != kProtocolVersion) {
        char vmsg[96];
        std::snprintf(vmsg, sizeof(vmsg), "peer %u: WingmanCommand version mismatch (got %u, want %u) — discarding",
                      peerId, static_cast<unsigned>(msg.protocolVersion), static_cast<unsigned>(kProtocolVersion));
        m_logger.log(LogLevel::Warn, __FILE__, __LINE__, vmsg);
        return;
    }

    // The order channel is off (no handler wired): discard rather than acking, so a server without
    // the feature is indistinguishable from one that never received the packet.
    if (!m_flightOrderHandler)
        return;

    auto& ps = m_peerInputs[peerId];

    // Dup/reorder guard. Reliable delivery is ordered, but a reconnecting client restarts its
    // counter, so accept the first packet unconditionally (hasSeq mirrors the MsgClientInput path).
    if (ps.hasWingmanSeq && !isNewerSeq(msg.seqNum, ps.lastWingmanSeq))
        return;
    ps.hasWingmanSeq = true;
    ps.lastWingmanSeq = msg.seqNum;

    // Per-peer order rate limit. Acked ONCE per window, never per packet: an ack for every rejected
    // packet would turn a flood into an amplifier pointed back at the sender.
    {
        const auto now = m_clock->now();
        if (now - ps.wingmanCmdWindowStart >= std::chrono::seconds(1)) {
            ps.wingmanCmdWindowStart = now;
            ps.wingmanCmdCount = 0;
            ps.wingmanRateLimitAcked = false;
        }
        ++ps.wingmanCmdCount;
        if (ps.wingmanCmdCount > static_cast<uint32_t>(m_flightCmdRateLimit)) {
            if (!ps.wingmanRateLimitAcked) {
                ps.wingmanRateLimitAcked = true;
                sendWingmanAck(peerId, msg.command, WingmanResult::RateLimited, msg.flightId, 0, kFlightAll, kNoTarget);
            }
            return;
        }
    }

    // Resolve the addressed formation. kOwnFlight = "the one I command" — the common case, so a
    // pilot who leads a single flight never has to know its id. A commander of several (an AWACS, a
    // package commander) MUST name one, because "my flight" is then ambiguous: refuse rather than
    // guess which of their formations they meant.
    fl::FormationId fid = msg.flightId;
    if (fid == kOwnFlight) {
        const std::vector<fl::FormationId> mine = m_formations.commandedBy(peerId);
        if (mine.size() != 1) {
            sendWingmanAck(peerId, msg.command, WingmanResult::NoFlight, kNoFlightId, 0, kFlightAll, kNoTarget);
            return;
        }
        fid = mine.front();
    }

    const fl::Formation* formation = m_formations.get(fid);

    // AUTHORITY. `commands()` walks UP the parent chain, so a package commander may order a flight
    // inside their package without being that flight's own commander. A peer that does not command
    // it gets NoFlight — deliberately the same code an unknown formation returns, so the order
    // channel cannot be used to enumerate which formations exist or who leads them.
    if (!formation || !m_formations.commands(peerId, fid)) {
        sendWingmanAck(peerId, msg.command, WingmanResult::NoFlight, kNoFlightId, 0, kFlightAll, kNoTarget);
        return;
    }

    if (!fl::ai::isWingmanCommandOrdinal(msg.command)) {
        sendWingmanAck(peerId, msg.command, WingmanResult::Rejected, fid, 0, kFlightAll, kNoTarget);
        return;
    }
    const auto cmd = static_cast<fl::ai::WingmanCommand>(msg.command);

    // Designate a target for attack_my_target from the COMMANDER's own boresight — state the server
    // already owns (PeerInputState::viewAxis, refreshed at 60 Hz from MsgClientInput). Nothing in the
    // cone means the order is REFUSED and behavior is unchanged: an attack order that quietly picks
    // its own target is worse than one that declines.
    EntityId designated{};
    if (cmd == fl::ai::WingmanCommand::AttackMyTarget) {
        const auto peerEnt = m_peerEntities.find(peerId);
        const EntityState* commander = peerEnt != m_peerEntities.end() ? m_entityManager.get(peerEnt->second) : nullptr;
        if (commander && m_targetDesignator) {
            designated = m_targetDesignator(*commander, ps.viewAxis);
        }
        if (!designated.valid()) {
            const auto liveNow = static_cast<uint8_t>(std::min<std::size_t>(formation->members.size(), 255));
            sendWingmanAck(peerId, msg.command, WingmanResult::NoTarget, fid, liveNow, msg.memberIdx, kNoTarget);
            return;
        }
    }

    const auto callerEnt = m_peerEntities.find(peerId);
    const uint32_t callerIdx = callerEnt != m_peerEntities.end() ? callerEnt->second.index : kFlightAll;

    const FlightOrderReport rep = dispatchOrder(fid, msg.command, msg.memberIdx, (msg.flags & kFlightFlagCascade) != 0,
                                                designated, peerId, callerIdx);

    // Fold the per-member outcomes into one answer for the commander.
    WingmanResult result = WingmanResult::NoFlight;
    if (rep.aiRetasked > 0) {
        result = WingmanResult::Acknowledged;
    } else if (rep.humansRelayed > 0) {
        // Every addressed member was human: the call went out, but no aircraft was retasked. Say so,
        // rather than letting the commander believe a machine is now obeying.
        result = WingmanResult::Relayed;
    } else if (rep.deadSkipped > 0) {
        result = WingmanResult::Unavailable;
    }

    const auto liveMembers = static_cast<uint8_t>(std::min(rep.aiRetasked + rep.humansRelayed, 255));
    sendWingmanAck(peerId, msg.command, result, fid, liveMembers, msg.memberIdx,
                   designated.valid() ? designated.index : kNoTarget);
}

WorldBroadcaster::FlightOrderReport WorldBroadcaster::applyFlightOrder(fl::FormationId fid, uint8_t command,
                                                                       uint32_t memberIdx, bool cascade,
                                                                       EntityId designatedTarget) {
    // kNoPeer = the game master: no peer-authority check (the console is authorized by the operator
    // password, not by commanding the formation), and relays carry no caller entity because the GM is
    // not flying one. Note this is NOT peer 0 — peer 0 is an ordinary player.
    return dispatchOrder(fid, command, memberIdx, cascade, designatedTarget, /*callerPeerId=*/fl::kNoPeer,
                         /*callerEntityIdx=*/kFlightAll);
}

WorldBroadcaster::FlightOrderReport WorldBroadcaster::dispatchOrder(fl::FormationId fid, uint8_t command,
                                                                    uint32_t memberIdx, bool cascade,
                                                                    EntityId designatedTarget, uint32_t callerPeerId,
                                                                    uint32_t callerEntityIdx) {
    FlightOrderReport rep{};
    if (!m_flightOrderHandler || !fl::ai::isWingmanCommandOrdinal(command))
        return rep;
    const auto cmd = static_cast<fl::ai::WingmanCommand>(command);

    // The formations an order reaches: just this one, or its whole subtree when cascading (a package
    // commander sending the whole package home, rather than each flight in turn).
    const std::vector<fl::FormationId> targets =
        cascade ? m_formations.subtree(fid) : std::vector<fl::FormationId>{fid};

    for (const fl::FormationId tid : targets) {
        const fl::Formation* f = m_formations.get(tid);
        if (!f)
            continue;

        // Copy the member list: the order handler retasks controllers, and a handler that spawns or
        // kills would otherwise invalidate the vector we are walking.
        const std::vector<fl::FormationMember> members = f->members;
        const auto flightSize = static_cast<uint8_t>(std::min<std::size_t>(members.size(), 255));

        for (const fl::FormationMember& m : members) {
            if (memberIdx != kFlightAll && m.id.index != memberIdx)
                continue;

            const EntityState* ms = m_entityManager.get(m.id);
            if (!ms || ms->dead) {
                ++rep.deadSkipped;
                continue;
            }

            if (m.isAi()) {
                // The server owns this aircraft: retask its controller.
                if (m_flightOrderHandler(*f, m, command, designatedTarget)) {
                    ++rep.aiRetasked;
                    // Record the weapons hold. It has no teeth until weapons land (#583) - this is
                    // where the firing trigger will read it - but the order is stored rather than
                    // dropped on the floor.
                    if (fl::Formation* mut = m_formations.get(tid)) {
                        for (fl::FormationMember& mm : mut->members) {
                            if (mm.id == m.id) {
                                if (cmd == fl::ai::WingmanCommand::HoldFire) {
                                    mm.weaponsHold = true;
                                } else if (fl::ai::clearsWeaponsHold(cmd)) {
                                    mm.weaponsHold = false; // an engage order implies weapons free
                                }
                            }
                        }
                    }
                }
            } else if (m.peerId != callerPeerId) {
                // A HUMAN member. The server cannot retask a person, and pretending otherwise would
                // be the dishonest design: relay the call to their client and let them decide.
                // memberIdx carries the CALLER's entity, so the recipient knows who is ordering them.
                sendWingmanAck(m.peerId, command, WingmanResult::Relayed, tid, flightSize, callerEntityIdx,
                               designatedTarget.valid() ? designatedTarget.index : kNoTarget);
                ++rep.humansRelayed;
            }
        }
    }
    return rep;
}

void WorldBroadcaster::sendAdminResponse(INetwork& net, uint32_t peerId, uint16_t reqId, const std::string& result) {
    if (result.size() <= kAdminResponseFastPathMax) {
        MsgAdminResponse resp{};
        resp.reqId = reqId;
        std::memcpy(resp.text, result.c_str(), result.size());
        resp.text[result.size()] = '\0';
        net.send(peerId, &resp, sizeof(resp), /*reliable=*/true);
        return;
    }
    uint16_t seq = 0;
    std::size_t offset = 0;
    while (offset < result.size()) {
        MsgAdminResponseChunk chunk{};
        chunk.reqId = reqId;
        chunk.seqNum = seq++;
        std::size_t n = std::min(result.size() - offset, kAdminChunkPayload);
        std::memcpy(chunk.body, result.data() + offset, n);
        chunk.body[n] = '\0';
        offset += n;
        if (offset >= result.size())
            chunk.flags = kChunkFlagEnd;
        net.send(peerId, &chunk, sizeof(chunk), /*reliable=*/true);
    }
}

std::shared_ptr<const FlightModelData> WorldBroadcaster::resolveFlightModel(EntityId id) {
    const EntityState* st = m_entityManager.get(id);
    if (!st)
        return nullptr;
    const EntityDef* def = m_registry.byIndex(st->typeIndex);
    if (!def || def->flightModelAsset.empty() || !m_flightModelResolver)
        return nullptr;
    std::shared_ptr<const FlightModelData> model = m_flightModelResolver(def->flightModelAsset);
    if (!model) {
        char wmsg[160];
        std::snprintf(wmsg, sizeof(wmsg), "flight model '%s' not found -- using builtin model",
                      def->flightModelAsset.c_str());
        m_logger.log(LogLevel::Warn, __FILE__, __LINE__, wmsg);
    }
    return model;
}

void WorldBroadcaster::addControlledEntity(EntityId id, std::unique_ptr<IEntityController> controller,
                                           std::shared_ptr<const FlightModelData> model, float initialThrottle,
                                           bool decimatable) {
    const EntityState* st = m_entityManager.get(id);
    if (!st)
        return;
    if (!model)
        model = BuiltinFlightModel::get();

    FlightState fs{};
    fs.pos_world[0] = st->transform.pos[0];
    fs.pos_world[1] = st->transform.pos[1];
    fs.pos_world[2] = st->transform.pos[2];
    fs.fuel_kg = model->geometry.fuel_kg;
    fs.mass_kg = model->geometry.mass_kg + fs.fuel_kg;
    fs.throttle_actual = initialThrottle;

    auto fi = std::make_unique<FlightIntegrator>(model);
    fi->setGravityField(*m_gravity);
    fi->reset(fs);
    // Give the controller the world's planet radius so its local-level (tangent-plane) guidance is
    // correct far from the origin. Every controller — peer or AI — enters through this single path.
    if (controller)
        controller->setPlanetRadius(static_cast<double>(m_planetRadiusKm) * 1000.0);
    ControlledEntity ce{id, std::move(fi), std::move(controller)};
    ce.decimatable = decimatable;
    m_controlledEntities[id.index] = std::move(ce);

    // Every controlled entity is an observer — PLAYERS INCLUDED. Player avionics (#526) inherits
    // exactly these semantics rather than growing a parallel "what can the player see" path, which is
    // the whole point of one vocabulary with three consumers.
    const EntityDef* def = m_registry.byIndex(st->typeIndex);
    const std::vector<std::string> sensorIds = def ? def->sensorIds : std::vector<std::string>{};
    const AiTuning tuning = (def && def->aiTuning) ? *def->aiTuning : AiTuning{};
    m_sensorSystem.addObserver(id.index, sensorIds, tuning.skill, tuning.reaction);

    // The candidate query is widened by the loudest signature in the registry, which can only change
    // as types are registered. Recomputing here is cheap (types are registered at startup) and keeps
    // it correct without a callback into the registry.
    m_sensorSystem.recomputeSignatureScale();
}

void WorldBroadcaster::registerController(EntityId id, std::unique_ptr<IEntityController> controller,
                                          std::shared_ptr<const FlightModelData> model) {
    // AI/scripted entities are decimatable — their sample() may be skipped under tick overrun (#514).
    addControlledEntity(id, std::move(controller), std::move(model), 0.f, /*decimatable=*/true);
}

// Grain size for the per-entity parallel passes: enough indices per chunk to amortise the
// dynamic-claim atomic without starving load balancing across workers.
static constexpr std::size_t kEntityPassGrain = 16;

void WorldBroadcaster::runEntityPass(std::size_t count, const std::function<void(std::size_t, std::size_t)>& fn) {
    if (count == 0)
        return;
    if (m_jobs)
        m_jobs->parallel_for(count, kEntityPassGrain, fn);
    else
        fn(0, count); // inline / serial fallback (unit tests, single-threaded servers)
}

// Grain size for the per-peer snapshot build: 1, because each peer is a heavy, heterogeneous-cost
// unit (a draw-distance interest query + priority/budget scheduler + quantized bitstream encode), so
// the finest grain gives the dynamic-claim cursor the best load balancing across workers.
static constexpr std::size_t kPeerPassGrain = 1;

void WorldBroadcaster::runPeerPass(std::size_t count, const std::function<void(std::size_t, std::size_t)>& fn) {
    if (count == 0)
        return;
    if (m_jobs)
        m_jobs->parallel_for(count, kPeerPassGrain, fn);
    else
        fn(0, count); // inline / serial fallback (unit tests, single-threaded servers)
}

void WorldBroadcaster::updateTerrainSteerCache() {
    // Lowest live entity index = a stable representative (m_controlledEntities order is unordered).
    const ControlledEntity* rep = nullptr;
    uint32_t repIdx = 0;
    for (const StepItem& it : m_stepItems) {
        if (!rep || it.idx < repIdx) {
            rep = it.ce;
            repIdx = it.idx;
        }
    }
    if (rep) {
        const FlightState& fs = rep->sim->state();
        m_entityX.store(fs.pos_world[0], std::memory_order_relaxed);
        m_entityZ.store(fs.pos_world[2], std::memory_order_relaxed);
    }
}

void WorldBroadcaster::stepFlightSim(FlightIntegrator& fi, EntityState& state, const ControlInput& ctrl, double simDt,
                                     uint32_t entityIdx, uint64_t tickIndex) {
    WindInfluence wind{};
    if (m_weather) {
        wind.wind_world[0] = m_weather->windX();
        wind.wind_world[2] = m_weather->windZ();
        float turb = m_weather->turbulenceAmplitude();
        // Per-entity deterministic turbulence: seed an LCG from (entityIdx, tickIndex) so the
        // perturbation is independent of evaluation order and identical across worker counts and
        // platforms (no shared RNG state mutated across entities — this is the parallel-safe form).
        uint32_t rng = entityIdx * 0x9E3779B1u + static_cast<uint32_t>(tickIndex) * 0x85EBCA77u +
                       static_cast<uint32_t>(tickIndex >> 32) * 0xC2B2AE3Du;
        rng = rng * 1664525u + 1013904223u;
        float r = static_cast<float>((rng >> 16) & 0xFFu) / 128.f - 1.f;
        wind.turbulence_body[0] = turb * r;
        wind.turbulence_body[1] = turb * 0.3f * r;
        wind.turbulence_body[2] = turb * 0.5f * r;
    }
    const float groundElev =
        m_groundQuery
            ? m_groundQuery(glm::dvec3{fi.state().pos_world[0], fi.state().pos_world[1], fi.state().pos_world[2]})
            : m_groundElevation.load(std::memory_order_relaxed);
    fi.step(static_cast<float>(simDt), ctrl, {}, wind, groundElev);

    const FlightState& fs = fi.state();
    // (Terrain-steer XZ cache moved to updateTerrainSteerCache(), run once after the integrate pass
    // — keeps this routine free of cross-entity writes so it is safe to call from worker threads.)

    // World velocity: rotate body velocity into world frame.
    // vel_body is double; cast to float here — wire protocol and render bridge stay float.
    float vel_body_f[3] = {float(fs.vel_body[0]), float(fs.vel_body[1]), float(fs.vel_body[2])};
    float wv[3];
    quatRotate(fs.quat, vel_body_f, wv);

    // Coordinate conventions are identical (both Y-up) — copy directly.
    state.transform.pos[0] = fs.pos_world[0];
    state.transform.pos[1] = fs.pos_world[1];
    state.transform.pos[2] = fs.pos_world[2];

    state.transform.vel[0] = wv[0];
    state.transform.vel[1] = wv[1];
    state.transform.vel[2] = wv[2];

    std::memcpy(state.transform.quat, fs.quat, 4 * sizeof(float));
}

void WorldBroadcaster::sendConnectAck(uint32_t peerId, EntityId assigned) {
    const uint32_t typeCount = m_registry.typeCount();

    std::vector<uint8_t> buf;
    buf.reserve(sizeof(MsgConnectAck) + typeCount * sizeof(MsgEntityTypeDef));

    MsgConnectAck ack;
    ack.msgId = static_cast<uint8_t>(MsgId::ConnectAck);
    ack.tickRateHz = 60;
    ack.typeCount = static_cast<uint16_t>(typeCount);
    ack.assignedEntityIdx = assigned.index;
    ack.assignedEntityGen = assigned.generation;
    ack.planetRadiusKm = m_planetRadiusKm;
    appendMsg(buf, ack);

    for (uint32_t i = 0; i < typeCount; ++i) {
        const EntityDef* def = m_registry.byIndex(i);
        if (!def)
            break;
        MsgEntityTypeDef typeDef{};
        typeDef.typeIndex = i;
        std::snprintf(typeDef.id, sizeof(typeDef.id), "%s", def->id.c_str());
        std::snprintf(typeDef.mesh, sizeof(typeDef.mesh), "%s", def->mesh.c_str());
        std::snprintf(typeDef.dmgMesh, sizeof(typeDef.dmgMesh), "%s", def->classicDamageMesh.c_str());
        // The client integrates the same aircraft we do, or its prediction is a fiction (#811).
        std::snprintf(typeDef.flightModel, sizeof(typeDef.flightModel), "%s", def->flightModelAsset.c_str());

        appendMsg(buf, typeDef);
    }

    m_net.send(peerId, buf.data(), buf.size(), /*reliable=*/true);
}

void WorldBroadcaster::sendConnectRefusal(uint32_t peerId, ConnectRefusalCode code, const char* reason) {
    MsgConnectRefusal msg{};
    msg.code = static_cast<uint8_t>(code);
    std::snprintf(msg.reason, sizeof(msg.reason), "%s", reason);
    m_net.send(peerId, &msg, sizeof(msg), /*reliable=*/true);
}

void WorldBroadcaster::rejectConnection(uint32_t peerId, const std::string& ip, ConnectRefusalCode code) {
    const RejectInfo info = rejectInfoFor(code);
    char msg[160];
    std::snprintf(msg, sizeof(msg), "peer %u from %s rejected (%s) -- disconnecting", peerId, ip.c_str(),
                  info.logPhrase);
    m_logger.log(info.level, __FILE__, __LINE__, msg);
    sendConnectRefusal(peerId, code, info.reason);
    m_net.disconnectPeer(peerId);
}

// ---------------------------------------------------------------------------
// Shutdown countdown
// ---------------------------------------------------------------------------

void WorldBroadcaster::setShutdownCallback(std::function<void()> fn) {
    m_shutdownCallback = std::move(fn);
}

void WorldBroadcaster::initiateShutdown(uint32_t secondsDelay, uint32_t warningIntervalS, std::string reason) {
    using namespace std::chrono;
    m_shuttingDown = true;
    m_shutdownAt = m_clock->now() + seconds(secondsDelay);
    m_warningIntervalS = warningIntervalS;
    m_nextNoticeAt = m_clock->now(); // fire on the very next tick
    m_shutdownReason = std::move(reason);
}

void WorldBroadcaster::cancelShutdown() {
    m_shuttingDown = false;
    m_shutdownReason.clear();
}

bool WorldBroadcaster::extendShutdown(uint32_t additionalSeconds) {
    if (!m_shuttingDown)
        return false;
    m_shutdownAt += std::chrono::seconds(additionalSeconds);
    m_nextNoticeAt = m_clock->now(); // immediate update notice on next tick
    return true;
}

uint32_t WorldBroadcaster::secondsUntilShutdown() const noexcept {
    if (!m_shuttingDown)
        return 0;
    using namespace std::chrono;
    auto now = m_clock->now();
    if (now >= m_shutdownAt)
        return 0;
    return static_cast<uint32_t>(duration_cast<seconds>(m_shutdownAt - now).count());
}

std::string WorldBroadcaster::makeShutdownMessage(uint32_t secsLeft, const std::string& reason) {
    if (reason.empty()) {
        if (secsLeft == 0)
            return "Server is shutting down now.";
        if (secsLeft <= 60)
            return "Server shutting down in 1 minute -- save your progress.";
        if (secsLeft < 3600)
            return "Server shutting down in " + std::to_string(secsLeft / 60) + " minutes.";
        return "Server shutting down in " + std::to_string(secsLeft / 3600) + " hour(s).";
    }
    if (secsLeft == 0)
        return reason + " -- shutting down now.";
    if (secsLeft <= 60)
        return reason + " -- shutting down in 1 minute.";
    if (secsLeft < 3600)
        return reason + " -- shutting down in " + std::to_string(secsLeft / 60) + " minutes.";
    return reason + " -- shutting down in " + std::to_string(secsLeft / 3600) + " hour(s).";
}

void WorldBroadcaster::broadcastShutdownNotice(uint16_t secsLeft, const char* text) {
    MsgServerNotice notice;
    notice.secondsRemaining = secsLeft;
    if (std::strlen(text) >= sizeof(notice.text))
        m_logger.log(LogLevel::Warn, __FILE__, __LINE__,
                     "Shutdown notice truncated: reason too long for MsgServerNotice::text.");
    std::snprintf(notice.text, sizeof(notice.text), "%s", text);
    m_net.broadcast(&notice, sizeof(notice), /*reliable=*/true);
}

} // namespace fl
