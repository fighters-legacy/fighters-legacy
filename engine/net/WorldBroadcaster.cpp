// SPDX-License-Identifier: GPL-3.0-or-later
#include "net/WorldBroadcaster.h"

#include "ILogger.h"
#include "INetwork.h"
#include "entity/EntityManager.h"
#include "entity/EntityState.h"
#include "entity/EntityTypeRegistry.h"
#include "flight/BuiltinFlightModel.h"
#include "flight/FlightIntegrator.h"
#include "net/GameProtocol.h"
#include "net/NetworkUtils.h"
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

void WorldBroadcaster::setClockOverride(std::function<std::chrono::steady_clock::time_point()> fn) {
    m_now = std::move(fn);
}

void WorldBroadcaster::setOperatorPassword(std::string password) {
    m_operatorPassword = std::move(password);
}

void WorldBroadcaster::setAdminDispatch(std::function<std::string(std::string_view)> fn) {
    m_adminDispatch = std::move(fn);
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
        auto cutoff = m_now() - std::chrono::seconds(m_connectRateWindowS);
        for (auto it = m_connectRecords.begin(); it != m_connectRecords.end();) {
            auto& ts = it->second.timestamps;
            while (!ts.empty() && ts.front() < cutoff)
                ts.pop_front();
            if (ts.empty())
                it = m_connectRecords.erase(it);
            else
                ++it;
        }
    }

    // Step each peer's FlightIntegrator from stored inputs, then copy state to the entity.
    for (auto& [peerId, inp] : m_peerInputs) {
        auto eit = m_peerEntities.find(peerId);
        if (eit == m_peerEntities.end())
            continue;
        auto fit = m_peerFlightSims.find(peerId);
        assert(fit != m_peerFlightSims.end()); // invariant: added together in onConnect
        EntityState* state = m_entityManager.get(eit->second);
        if (!state || state->dead)
            continue;
        stepFlightSim(*fit->second, *state, inp, simDt);
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
    };
    std::unordered_map<uint32_t, TelemetryEntry> entityTelemetry;
    for (auto& [pid, fi] : m_peerFlightSims) {
        auto eit = m_peerEntities.find(pid);
        if (eit != m_peerEntities.end()) {
            const auto& s = fi->state();
            entityTelemetry[eit->second.index] = {
                static_cast<uint8_t>(s.throttle_actual * 100.f),
                static_cast<uint8_t>(std::clamp(s.fuel_kg / 4000.f * 100.f, 0.f, 100.f))};
        }
    }

    // Write header placeholder; fill entityCount after iteration.
    MsgWorldSnapshotHeader hdr;
    hdr.msgId = static_cast<uint8_t>(MsgId::WorldSnapshot);
    hdr.protocolVersion = static_cast<uint8_t>(kProtocolVersion);
    hdr.entityCount = 0;
    hdr.tickIndex = tickIndex;

    const std::size_t hdrOffset = buf.size();
    buf.resize(buf.size() + sizeof(MsgWorldSnapshotHeader));

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

        buf.resize(buf.size() + sizeof(MsgEntityEntry));
        std::memcpy(buf.data() + buf.size() - sizeof(MsgEntityEntry), &entry, sizeof(entry));
        ++count;
    });

    hdr.entityCount = count;
    std::memcpy(buf.data() + hdrOffset, &hdr, sizeof(hdr));

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
        auto now = m_now();
        if (now >= m_shutdownAt) {
            broadcastShutdownNotice(0, "Server is shutting down now.");
            m_shuttingDown = false;
            if (m_shutdownCallback)
                m_shutdownCallback();
        } else if (now >= m_nextNoticeAt) {
            auto secsLeft = static_cast<uint32_t>(duration_cast<seconds>(m_shutdownAt - now).count());
            broadcastShutdownNotice(static_cast<uint16_t>(secsLeft), makeShutdownMessage(secsLeft).c_str());
            // Always squeeze in a T-60s notice: if the next interval would skip past it, clamp.
            auto nextInterval = now + seconds(m_warningIntervalS);
            auto oneMinBefore = m_shutdownAt - seconds(60);
            m_nextNoticeAt = (nextInterval > oneMinBefore && oneMinBefore > now) ? oneMinBefore : nextInterval;
        }
    }

    m_net.service(0);
}

void WorldBroadcaster::onConnect(uint32_t peerId) {
    // Ban check — reject banned IPs before any state is created.
    std::string ip = extractIp(m_net.getPeerAddress(peerId));
    if (!ip.empty() && m_bannedAddresses.count(ip)) {
        char msg[128];
        std::snprintf(msg, sizeof(msg), "peer %u from %s is banned — disconnecting", peerId, ip.c_str());
        m_logger.log(LogLevel::Info, __FILE__, __LINE__, msg);
        m_net.disconnectPeer(peerId);
        return;
    }

    // Allowlist check — if non-empty, only listed IPs may connect.
    if (!ip.empty() && !m_allowedAddresses.empty() && !m_allowedAddresses.count(ip)) {
        char msg[128];
        std::snprintf(msg, sizeof(msg), "peer %u from %s not on allowlist — disconnecting", peerId, ip.c_str());
        m_logger.log(LogLevel::Info, __FILE__, __LINE__, msg);
        m_net.disconnectPeer(peerId);
        return;
    }

    // Connection rate limit — sliding window per IP.
    if (!ip.empty()) {
        auto now = m_now();
        auto& rec = m_connectRecords[ip];
        auto cutoff = now - std::chrono::seconds(m_connectRateWindowS);
        while (!rec.timestamps.empty() && rec.timestamps.front() < cutoff)
            rec.timestamps.pop_front();
        rec.timestamps.push_back(now);
        if (static_cast<int>(rec.timestamps.size()) > m_connectRateLimit) {
            char msg[128];
            std::snprintf(msg, sizeof(msg), "peer %u from %s rate-limited — disconnecting", peerId, ip.c_str());
            m_logger.log(LogLevel::Info, __FILE__, __LINE__, msg);
            m_net.disconnectPeer(peerId);
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
            char msg[128];
            std::snprintf(msg, sizeof(msg), "peer %u from %s exceeds per-IP connection limit (%d) -- disconnecting",
                          peerId, ip.c_str(), m_maxConnectionsPerIp);
            m_logger.log(LogLevel::Info, __FILE__, __LINE__, msg);
            m_net.disconnectPeer(peerId);
            return;
        }
    }

    char msg[64];
    std::snprintf(msg, sizeof(msg), "peer %u connected", peerId);
    m_logger.log(LogLevel::Info, __FILE__, __LINE__, msg);

    MsgHello hello;
    m_net.send(peerId, &hello, sizeof(hello), /*reliable=*/true);

    EntityTransform t{};
    t.pos[1] = 2000.0; // spawn at 2000 m MSL — clears ~550 m procedural terrain base by ~1450 m
    EntityId id = m_entityManager.spawn("builtin:debug-entity", t, peerId);
    if (id.valid()) {
        m_peerEntities[peerId] = id;
        m_peerInputs[peerId] = {};

        // Initialise FlightIntegrator at spawn position with some forward airspeed
        // so the craft generates lift immediately.
        FlightState fs{};
        fs.pos_world[0] = static_cast<float>(t.pos[0]);
        fs.pos_world[1] = static_cast<float>(t.pos[1]); // Y = altitude (both Y-up)
        fs.pos_world[2] = static_cast<float>(t.pos[2]);
        fs.vel_body[0] = 40.f; // 40 m/s forward → lift from first tick
        fs.fuel_kg = BuiltinFlightModel::get()->geometry.fuel_kg;
        fs.mass_kg = BuiltinFlightModel::get()->geometry.mass_kg + fs.fuel_kg;
        fs.throttle_actual = 0.4f; // pre-spooled to avoid initial stall

        auto fi = std::make_unique<FlightIntegrator>(BuiltinFlightModel::get());
        fi->reset(fs);
        m_peerFlightSims.emplace(peerId, std::move(fi));
    }
    sendConnectAck(peerId, id);
    m_activePeerCount.fetch_add(1, std::memory_order_relaxed);
}

void WorldBroadcaster::onDisconnect(uint32_t peerId) {
    char msg[64];
    std::snprintf(msg, sizeof(msg), "peer %u disconnected", peerId);
    m_logger.log(LogLevel::Info, __FILE__, __LINE__, msg);

    auto it = m_peerEntities.find(peerId);
    if (it != m_peerEntities.end()) {
        m_entityManager.kill(it->second);
        m_peerEntities.erase(it);
    }
    m_peerInputs.erase(peerId);
    m_peerFlightSims.erase(peerId);
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
            auto now = m_now();
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
                return;
            }
        }

        std::string_view cmdView(msg.command);
        if (cmdView.empty())
            return;

        // Dispatch on the sim thread (same as stdin admin loop).
        // Mutating commands enqueue via gameLoop.enqueueSimCallback() internally.
        std::string result = m_adminDispatch(cmdView);

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

void WorldBroadcaster::stepFlightSim(FlightIntegrator& fi, EntityState& state, const PeerInputState& inp,
                                     double simDt) {
    ControlInput ctrl{};
    ctrl.throttle = inp.throttle;
    ctrl.elevator = inp.elevator;
    ctrl.aileron = inp.aileron;
    ctrl.rudder = inp.rudder;

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
    fi.step(static_cast<float>(simDt), ctrl, {}, wind);

    const FlightState& fs = fi.state();

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
    buf.resize(sizeof(MsgConnectAck));
    std::memcpy(buf.data(), &ack, sizeof(ack));

    for (uint32_t i = 0; i < typeCount; ++i) {
        const EntityDef* def = m_registry.byIndex(i);
        if (!def)
            break;
        MsgEntityTypeDef typeDef{};
        typeDef.typeIndex = i;
        std::snprintf(typeDef.id, sizeof(typeDef.id), "%s", def->id.c_str());
        std::snprintf(typeDef.mesh, sizeof(typeDef.mesh), "%s", def->mesh.c_str());
        std::snprintf(typeDef.dmgMesh, sizeof(typeDef.dmgMesh), "%s", def->classicDamageMesh.c_str());

        buf.resize(buf.size() + sizeof(MsgEntityTypeDef));
        std::memcpy(buf.data() + buf.size() - sizeof(MsgEntityTypeDef), &typeDef, sizeof(typeDef));
    }

    m_net.send(peerId, buf.data(), buf.size(), /*reliable=*/true);
}

// ---------------------------------------------------------------------------
// Shutdown countdown
// ---------------------------------------------------------------------------

void WorldBroadcaster::setShutdownCallback(std::function<void()> fn) {
    m_shutdownCallback = std::move(fn);
}

void WorldBroadcaster::initiateShutdown(uint32_t secondsDelay, uint32_t warningIntervalS) {
    using namespace std::chrono;
    m_shuttingDown = true;
    m_shutdownAt = m_now() + seconds(secondsDelay);
    m_warningIntervalS = warningIntervalS;
    m_nextNoticeAt = m_now(); // fire on the very next tick
}

void WorldBroadcaster::cancelShutdown() {
    m_shuttingDown = false;
}

bool WorldBroadcaster::extendShutdown(uint32_t additionalSeconds) {
    if (!m_shuttingDown)
        return false;
    m_shutdownAt += std::chrono::seconds(additionalSeconds);
    m_nextNoticeAt = m_now(); // immediate update notice on next tick
    return true;
}

uint32_t WorldBroadcaster::secondsUntilShutdown() const noexcept {
    if (!m_shuttingDown)
        return 0;
    using namespace std::chrono;
    auto now = m_now();
    if (now >= m_shutdownAt)
        return 0;
    return static_cast<uint32_t>(duration_cast<seconds>(m_shutdownAt - now).count());
}

std::string WorldBroadcaster::makeShutdownMessage(uint32_t secsLeft) {
    if (secsLeft <= 60)
        return "Server shutting down in 1 minute -- save your progress.";
    if (secsLeft < 3600)
        return "Server shutting down in " + std::to_string(secsLeft / 60) + " minutes.";
    return "Server shutting down in " + std::to_string(secsLeft / 3600) + " hour(s).";
}

void WorldBroadcaster::broadcastShutdownNotice(uint16_t secsLeft, const char* text) {
    MsgServerNotice notice;
    notice.secondsRemaining = secsLeft;
    std::snprintf(notice.text, sizeof(notice.text), "%s", text);
    m_net.broadcast(&notice, sizeof(notice), /*reliable=*/true);
}

} // namespace fl
