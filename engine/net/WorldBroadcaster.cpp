// SPDX-License-Identifier: GPL-3.0-or-later
#include "net/WorldBroadcaster.h"

#include "ILogger.h"
#include "INetwork.h"
#include "entity/EntityManager.h"
#include "entity/EntityState.h"
#include "entity/EntityTypeRegistry.h"
#include "net/GameProtocol.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace fl {

WorldBroadcaster::WorldBroadcaster(EntityManager& entityManager, EntityTypeRegistry& registry, INetwork& net,
                                   ILogger& logger)
    : m_entityManager(entityManager), m_registry(registry), m_net(net), m_logger(logger) {}

void WorldBroadcaster::onTick(double simDt, uint64_t tickIndex) {
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
    sendConnectAck(peerId);
}

void WorldBroadcaster::onDisconnect(uint32_t peerId) {
    char msg[64];
    std::snprintf(msg, sizeof(msg), "peer %u disconnected", peerId);
    m_logger.log(LogLevel::Info, __FILE__, __LINE__, msg);
}

void WorldBroadcaster::onReceive(uint32_t /*peerId*/, const void* /*data*/, std::size_t /*size*/) {
    // Phase 2 sandbox: no client→server messages expected; silently discard.
}

void WorldBroadcaster::sendConnectAck(uint32_t peerId) {
    const uint32_t typeCount = m_registry.typeCount();

    std::vector<uint8_t> buf;
    buf.reserve(sizeof(MsgConnectAck) + typeCount * sizeof(MsgEntityTypeDef));

    MsgConnectAck ack;
    ack.msgId = static_cast<uint8_t>(MsgId::ConnectAck);
    ack.tickRateHz = 60;
    ack.typeCount = static_cast<uint16_t>(typeCount);
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
