// SPDX-License-Identifier: GPL-3.0-or-later
#include "net/WorldBroadcaster.h"

#include "ILogger.h"
#include "INetwork.h"
#include "entity/EntityManager.h"
#include "entity/EntityState.h"
#include "entity/EntityTypeRegistry.h"
#include "net/GameProtocol.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

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

// Hamilton product: out = a * b.
static void quatMul(const float a[4], const float b[4], float out[4]) {
    out[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    out[1] = a[3] * b[1] + a[1] * b[3] + a[2] * b[0] - a[0] * b[2];
    out[2] = a[3] * b[2] + a[2] * b[3] + a[0] * b[1] - a[1] * b[0];
    out[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

static void quatNormalize(float q[4]) {
    float mag = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (mag > 1e-6f) {
        q[0] /= mag;
        q[1] /= mag;
        q[2] /= mag;
        q[3] /= mag;
    } // else: degenerate quaternion — unreachable in practice; left as safety guard // GCOV_EXCL_LINE
}

// ---------------------------------------------------------------------------
// Kinematics constants
// ---------------------------------------------------------------------------

static constexpr float kMaxSpeedMps = 340.f; // Mach 1 cap for sandbox
static constexpr float kMaxRateRadS = 1.f;   // max angular rate (rad/s)

namespace fl {

WorldBroadcaster::WorldBroadcaster(EntityManager& entityManager, EntityTypeRegistry& registry, INetwork& net,
                                   ILogger& logger)
    : m_entityManager(entityManager), m_registry(registry), m_net(net), m_logger(logger) {}

void WorldBroadcaster::onTick(double simDt, uint64_t tickIndex) {
    // Apply stored client inputs to owned entities before the housekeeping tick
    // so the render snapshot captures updated positions.
    for (auto& [peerId, inp] : m_peerInputs) {
        auto eit = m_peerEntities.find(peerId);
        if (eit == m_peerEntities.end())
            continue;
        EntityState* state = m_entityManager.get(eit->second);
        if (!state || state->dead)
            continue;
        applyPeerInput(*state, inp, simDt);
    }

    m_entityManager.onTick(simDt, tickIndex);

    // Build world snapshot packet.
    // Header + one entry per live entity.
    std::vector<uint8_t> buf;
    buf.reserve(sizeof(MsgWorldSnapshotHeader) + 64 * sizeof(MsgEntityEntry));

    // Write header placeholder; fill entityCount after iteration.
    MsgWorldSnapshotHeader hdr;
    hdr.msgId = static_cast<uint8_t>(MsgId::WorldSnapshot);
    hdr._pad = 0;
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
        entry._pad[0] = 0;
        entry._pad[1] = 0;

        buf.resize(buf.size() + sizeof(MsgEntityEntry));
        std::memcpy(buf.data() + buf.size() - sizeof(MsgEntityEntry), &entry, sizeof(entry));
        ++count;
    });

    hdr.entityCount = count;
    std::memcpy(buf.data() + hdrOffset, &hdr, sizeof(hdr));

    m_net.broadcast(buf.data(), buf.size(), /*reliable=*/false);
    m_net.service(0);
}

void WorldBroadcaster::onConnect(uint32_t peerId) {
    char msg[64];
    std::snprintf(msg, sizeof(msg), "peer %u connected", peerId);
    m_logger.log(LogLevel::Info, __FILE__, __LINE__, msg);

    EntityTransform t{};
    t.pos[1] = 500.0; // spawn at 500 m altitude
    EntityId id = m_entityManager.spawn("builtin:debug-entity", t, peerId);
    if (id.valid()) {
        m_peerEntities[peerId] = id;
        m_peerInputs[peerId] = {};
    }
    sendConnectAck(peerId, id);
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
    }
    // Unknown msgIds: silently discard (no log spam; future protocol versions may add new IDs)
}

void WorldBroadcaster::applyPeerInput(EntityState& state, const PeerInputState& inp, double simDt) {
    // Compute forward direction: entity body +X axis rotated into world space.
    float fwd[3];
    const float bodyFwd[3] = {1.f, 0.f, 0.f};
    quatRotate(state.transform.quat, bodyFwd, fwd);

    float speed = inp.throttle * kMaxSpeedMps;
    state.transform.vel[0] = fwd[0] * speed;
    state.transform.vel[1] = fwd[1] * speed;
    state.transform.vel[2] = fwd[2] * speed;

    state.transform.pos[0] += static_cast<double>(state.transform.vel[0]) * simDt;
    state.transform.pos[1] += static_cast<double>(state.transform.vel[1]) * simDt;
    state.transform.pos[2] += static_cast<double>(state.transform.vel[2]) * simDt;

    // Build incremental rotation quaternion from angular rates (small-angle approximation).
    float halfDt = static_cast<float>(simDt) * 0.5f;
    float dq[4] = {
        inp.elevator * kMaxRateRadS * halfDt, // pitch (X axis)
        inp.rudder * kMaxRateRadS * halfDt,   // yaw   (Y axis)
        inp.aileron * kMaxRateRadS * halfDt,  // roll  (Z axis)
        1.f,
    };
    quatNormalize(dq);

    float nq[4];
    quatMul(state.transform.quat, dq, nq);
    quatNormalize(nq);
    std::memcpy(state.transform.quat, nq, sizeof(nq));
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

} // namespace fl
