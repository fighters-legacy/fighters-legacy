// SPDX-License-Identifier: GPL-3.0-or-later
#include "net/WorldBroadcaster.h"
#include "render/RenderSnapshot.h"

#include "ILogger.h"
#include "INetwork.h"
#include "entity/EntityManager.h"
#include "entity/EntityState.h"
#include "entity/EntityTypeRegistry.h"
#include "entity/IEntityController.h"
#include "flight/BuiltinFlightModel.h"
#include "flight/FlightIntegrator.h"
#include "net/GameProtocol.h"
#include "net/NetworkUtils.h"
#include "net/WireCodec.h"
#include "weather/WeatherController.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

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

    fl::ControlInput sample(const fl::EntityState& /*state*/, uint64_t /*tick*/, double /*dt*/) override {
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

WorldBroadcaster::WorldBroadcaster(EntityManager& entityManager, EntityTypeRegistry& registry, INetwork& net,
                                   ILogger& logger, WeatherController* weather)
    : m_entityManager(entityManager), m_registry(registry), m_net(net), m_logger(logger), m_weather(weather) {}

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

void WorldBroadcaster::setClock(const IClock& clock) {
    m_clock = &clock;
    m_adminAuthTracker.setClock(clock);
}

void WorldBroadcaster::setMotd(std::string motd) {
    m_motd = std::move(motd);
}

void WorldBroadcaster::setMotdDisplaySeconds(uint16_t seconds) noexcept {
    m_motdDisplaySeconds = seconds;
}

void WorldBroadcaster::setFlightModelResolver(FlightModelResolver fn) {
    m_flightModelResolver = std::move(fn);
}

void WorldBroadcaster::setOperatorPassword(std::string password) {
    m_operatorPassword = std::move(password);
}

void WorldBroadcaster::setAdminDispatch(std::function<std::string(std::string_view)> fn) {
    m_adminDispatch = std::move(fn);
}

void WorldBroadcaster::setAdminAuthParams(int maxFailures, int lockoutSeconds) {
    m_adminAuthTracker = AuthTracker(maxFailures, lockoutSeconds);
    m_adminAuthTracker.setClock(*m_clock);
}

void WorldBroadcaster::applyConfig(const WorldBroadcasterConfig& cfg) {
    setRateLimitParams(cfg.connectRateLimit, cfg.connectRateWindowS, cfg.floodMultiplier);
    setMaxConnectionsPerIp(cfg.maxConnectionsPerIp);
    setAdminAuthParams(cfg.adminAuthMaxFailures, cfg.adminAuthLockoutSeconds);
    setMotd(cfg.motd);
    setMotdDisplaySeconds(cfg.motdDisplaySeconds);
    setOperatorPassword(cfg.operatorPassword);
}

void WorldBroadcaster::forEachPeer(
    std::function<void(uint32_t peerId, const std::string& addr, EntityId eid)> fn) const {
    for (const auto& [peerId, eid] : m_peerEntities) {
        const char* raw = m_net.getPeerAddress(peerId);
        std::string addr = raw ? raw : "";
        fn(peerId, addr, eid);
    }
}

void WorldBroadcaster::onTick(double simDt, uint64_t tickIndex) {
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

    // Step every controlled entity from its control source (peer/AI/script), then copy state back.
    for (auto& [entityIdx, ce] : m_controlledEntities) {
        EntityState* state = m_entityManager.get(ce.id);
        if (!state || state->dead)
            continue;
        const ControlInput ctrl = ce.controller->sample(*state, tickIndex, simDt);
        stepFlightSim(*ce.sim, *state, ctrl, simDt);

        // NaN/Inf detection — log immediately so the cause is visible before any crash.
        const FlightState& fs = ce.sim->state();
        const bool badPos =
            !std::isfinite(fs.pos_world[0]) || !std::isfinite(fs.pos_world[1]) || !std::isfinite(fs.pos_world[2]);
        const bool badVel =
            !std::isfinite(fs.vel_body[0]) || !std::isfinite(fs.vel_body[1]) || !std::isfinite(fs.vel_body[2]);
        if (badPos || badVel) {
            char msg[256];
            std::snprintf(msg, sizeof(msg),
                          "[flight entity=%u] NaN/Inf — pos=(%.3g,%.3g,%.3g) vel_body=(%.3g,%.3g,%.3g)", entityIdx,
                          fs.pos_world[0], fs.pos_world[1], fs.pos_world[2], fs.vel_body[0], fs.vel_body[1],
                          fs.vel_body[2]);
            m_logger.log(LogLevel::Error, __FILE__, __LINE__, msg);
        }
        // Periodic state trace: once per second (60 Hz sim) for trajectory diagnostics.
        if (tickIndex % 60 == 0) {
            char msg[256];
            std::snprintf(msg, sizeof(msg),
                          "[flight entity=%u] tick=%llu pos=(%.1f,%.1f,%.1f) vel_body=(%.1f,%.1f,%.1f) thr=%.0f%%",
                          entityIdx, static_cast<unsigned long long>(tickIndex), fs.pos_world[0], fs.pos_world[1],
                          fs.pos_world[2], fs.vel_body[0], fs.vel_body[1], fs.vel_body[2], fs.throttle_actual * 100.f);
            m_logger.log(LogLevel::Trace, __FILE__, __LINE__, msg);
        }
    }

    m_entityManager.onTick(simDt, tickIndex);

    // Build world snapshot packet.
    // Header + one entry per live entity.
    std::vector<uint8_t> buf;
    buf.reserve(sizeof(MsgWorldSnapshotHeader) + 64 * sizeof(MsgEntityEntry));

    // Build entityIdx -> throttle/fuelPct map from peer flight integrators
    // so the forEach lambda can fill telemetry fields without a map lookup per entity.
    struct TelemetryEntry {
        uint8_t throttle;
        uint8_t fuelPct;
        uint8_t abEngaged;
        uint8_t engineFailFlags;
    };
    std::unordered_map<uint32_t, TelemetryEntry> entityTelemetry;
    for (auto& [entityIdx, ce] : m_controlledEntities) {
        const auto& s = ce.sim->state();
        entityTelemetry[entityIdx] = {static_cast<uint8_t>(s.throttle_actual * 100.f),
                                      static_cast<uint8_t>(std::clamp(s.fuel_kg / 4000.f * 100.f, 0.f, 100.f)),
                                      static_cast<uint8_t>(s.ab_engaged ? 1u : 0u), s.engineFailFlags};
    }

    // Write header placeholder; fill entityCount after iteration.
    MsgWorldSnapshotHeader hdr;
    hdr.msgId = static_cast<uint8_t>(MsgId::WorldSnapshot);
    hdr.protocolVersion = static_cast<uint8_t>(kProtocolVersion);
    hdr.entityCount = 0;
    hdr.tickIndex = tickIndex;

    const std::size_t hdrOffset = buf.size();
    appendMsg(buf, hdr); // placeholder; entityCount patched in after iteration

    uint16_t count = 0;
    m_entityManager.forEach([&](const EntityState& state) {
        MsgEntityEntry entry;
        entry.entityIdx = state.id.index;
        entry.entityGen = state.id.generation;
        entry.typeIndex = state.typeIndex;
        entry.pos[0] = state.transform.pos[0];
        entry.pos[1] = state.transform.pos[1];
        entry.pos[2] = state.transform.pos[2];
        entry.vel[0] = state.transform.vel[0];
        entry.vel[1] = state.transform.vel[1];
        entry.vel[2] = state.transform.vel[2];
        entry.ori[0] = state.transform.quat[0]; // x
        entry.ori[1] = state.transform.quat[1]; // y
        entry.ori[2] = state.transform.quat[2]; // z
        entry.ori[3] = state.transform.quat[3]; // w
        entry.damageLevel = static_cast<uint8_t>(state.damageLevel);
        entry.flags = state.playerOwned ? 1u : 0u;
        auto tit = entityTelemetry.find(state.id.index);
        entry.throttle = (tit != entityTelemetry.end()) ? tit->second.throttle : 0u;
        entry.fuelPct = (tit != entityTelemetry.end()) ? tit->second.fuelPct : 0u;
        entry.abEngaged = (tit != entityTelemetry.end()) ? tit->second.abEngaged : 0u;
        entry.engineFailFlags = (tit != entityTelemetry.end()) ? tit->second.engineFailFlags : 0u;
        if (static_cast<uint8_t>(state.damageLevel) >= 2u)
            entry.engineFailFlags |= fl::kEngineFailGeneric;

        appendMsg(buf, entry);
        ++count;
    });

    hdr.entityCount = count;
    writeMsgAt(buf, hdrOffset, hdr);

    m_net.broadcast(buf.data(), buf.size(), /*reliable=*/false);

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
    constexpr double kSpawnAGL = 500.0;
    t.pos[1] = static_cast<double>(m_groundElevation.load(std::memory_order_relaxed)) + kSpawnAGL;
    EntityId id = m_entityManager.spawn("builtin:debug-entity", t, peerId);
    if (id.valid()) {
        m_peerEntities[peerId] = id;
        m_peerInputs[peerId] = {};

        // Resolve the entity type's flight model (server-authoritative; never sent on the wire).
        // Empty id, no resolver, or an unknown id falls back to the builtin UFO model.
        std::shared_ptr<const FlightModelData> model = resolveFlightModel(id);

        // PeerController reads the peer's stable input slot (pointer valid across rehash, slot torn
        // down after the controller on disconnect). Pre-spooled to 0.4 to match the client's initial
        // throttle state.
        addControlledEntity(id, std::make_unique<PeerController>(&m_peerInputs[peerId]), std::move(model), 0.4f);
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
    m_activePeerCount.fetch_add(1, std::memory_order_relaxed);
}

void WorldBroadcaster::onDisconnect(uint32_t peerId) {
    char msg[64];
    std::snprintf(msg, sizeof(msg), "peer %u disconnected", peerId);
    m_logger.log(LogLevel::Info, __FILE__, __LINE__, msg);

    auto it = m_peerEntities.find(peerId);
    if (it != m_peerEntities.end()) {
        // Tear down the controller (which points into m_peerInputs) before erasing the input slot.
        m_controlledEntities.erase(it->second.index);
        m_entityManager.kill(it->second);
        m_peerEntities.erase(it);
    }
    m_peerInputs.erase(peerId);
    m_peerFloodState.erase(peerId);
    m_activePeerCount.fetch_sub(1, std::memory_order_relaxed);
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

        PeerInputState inp;
        inp.throttle = std::clamp(msg.throttle, 0.f, 1.f);
        inp.elevator = std::clamp(msg.elevator, -1.f, 1.f);
        inp.aileron = std::clamp(msg.aileron, -1.f, 1.f);
        inp.rudder = std::clamp(msg.rudder, -1.f, 1.f);
        inp.buttons = msg.buttons;

        float vmag = std::sqrt(msg.viewAxis[0] * msg.viewAxis[0] + msg.viewAxis[1] * msg.viewAxis[1] +
                               msg.viewAxis[2] * msg.viewAxis[2]);
        if (vmag > 1e-6f) {
            inp.viewAxis[0] = msg.viewAxis[0] / vmag;
            inp.viewAxis[1] = msg.viewAxis[1] / vmag;
            inp.viewAxis[2] = msg.viewAxis[2] / vmag;
        }
        // else: degenerate viewAxis — keep default {1,0,0} fallback set in PeerInputState

        m_peerInputs[peerId] = inp;
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

        MsgAdminResponse resp{};
        resp.msgId = static_cast<uint8_t>(MsgId::AdminResponse);
        std::size_t copyLen = std::min(result.size(), sizeof(resp.text) - 1u);
        std::memcpy(resp.text, result.c_str(), copyLen);
        resp.text[copyLen] = '\0';
        m_net.send(peerId, &resp, sizeof(resp), /*reliable=*/true);
    }
    // Unknown msgIds: silently discard (no log spam; future protocol versions may add new IDs)
}

std::shared_ptr<const FlightModelData> WorldBroadcaster::resolveFlightModel(EntityId id) {
    const EntityState* st = m_entityManager.get(id);
    if (!st)
        return nullptr;
    const EntityDef* def = m_registry.byIndex(st->typeIndex);
    if (!def || def->flightModelId.empty() || !m_flightModelResolver)
        return nullptr;
    std::shared_ptr<const FlightModelData> model = m_flightModelResolver(def->flightModelId);
    if (!model) {
        char wmsg[160];
        std::snprintf(wmsg, sizeof(wmsg), "flight model '%s' not found -- using builtin model",
                      def->flightModelId.c_str());
        m_logger.log(LogLevel::Warn, __FILE__, __LINE__, wmsg);
    }
    return model;
}

void WorldBroadcaster::addControlledEntity(EntityId id, std::unique_ptr<IEntityController> controller,
                                           std::shared_ptr<const FlightModelData> model, float initialThrottle) {
    const EntityState* st = m_entityManager.get(id);
    if (!st)
        return;
    if (!model)
        model = BuiltinFlightModel::get();

    FlightState fs{};
    fs.pos_world[0] = static_cast<float>(st->transform.pos[0]);
    fs.pos_world[1] = static_cast<float>(st->transform.pos[1]);
    fs.pos_world[2] = static_cast<float>(st->transform.pos[2]);
    fs.fuel_kg = model->geometry.fuel_kg;
    fs.mass_kg = model->geometry.mass_kg + fs.fuel_kg;
    fs.throttle_actual = initialThrottle;

    auto fi = std::make_unique<FlightIntegrator>(model);
    fi->reset(fs);
    m_controlledEntities[id.index] = ControlledEntity{id, std::move(fi), std::move(controller)};
}

void WorldBroadcaster::registerController(EntityId id, std::unique_ptr<IEntityController> controller,
                                          std::shared_ptr<const FlightModelData> model) {
    addControlledEntity(id, std::move(controller), std::move(model), 0.f);
}

void WorldBroadcaster::stepFlightSim(FlightIntegrator& fi, EntityState& state, const ControlInput& ctrl, double simDt) {
    WindInfluence wind{};
    if (m_weather) {
        wind.wind_world[0] = m_weather->windX();
        wind.wind_world[2] = m_weather->windZ();
        float turb = m_weather->turbulenceAmplitude();
        m_turbRng = m_turbRng * 1664525u + 1013904223u;
        float r = static_cast<float>((m_turbRng >> 16) & 0xFFu) / 128.f - 1.f;
        wind.turbulence_body[0] = turb * r;
        wind.turbulence_body[1] = turb * 0.3f * r;
        wind.turbulence_body[2] = turb * 0.5f * r;
    }
    fi.step(static_cast<float>(simDt), ctrl, {}, wind, m_groundElevation.load(std::memory_order_relaxed));

    const FlightState& fs = fi.state();
    // Cache entity XZ so the main thread can steer terrain loading and update the floor.
    m_entityX.store(fs.pos_world[0], std::memory_order_relaxed);
    m_entityZ.store(fs.pos_world[2], std::memory_order_relaxed);

    // World velocity: rotate body velocity into world frame.
    float wv[3];
    quatRotate(fs.quat, fs.vel_body, wv);

    // Coordinate conventions are identical (both Y-up) — copy directly.
    state.transform.pos[0] = static_cast<double>(fs.pos_world[0]);
    state.transform.pos[1] = static_cast<double>(fs.pos_world[1]);
    state.transform.pos[2] = static_cast<double>(fs.pos_world[2]);

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
