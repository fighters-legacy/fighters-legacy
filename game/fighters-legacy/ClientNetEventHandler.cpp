// SPDX-License-Identifier: GPL-3.0-or-later
#include "ClientNetEventHandler.h"
#include "ServerNotice.h"

#include "ILogger.h"
#include "INetwork.h"
#include "console/GameConsole.h"
#include "entity/EntityDef.h"
#include "entity/EntityTypeRegistry.h"
#include "net/BitStream.h"
#include "net/GameProtocol.h"
#include "net/SnapshotCodec.h"
#include "net/SnapshotScheduler.h" // kSnapshotRetentionTicks
#include "net/WireCodec.h"
#include "render/RenderSnapshot.h"
#include "render/SimRenderBridge.h"
#include "weather/WeatherController.h"

#include <algorithm>
#include <cstring>
#include <glm/gtc/quaternion.hpp>
#include <sstream>

namespace fl {

static void printAdminLines(GameConsole* console, const std::string& text) {
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        console->print(std::string("[admin] ") + line);
    }
}

void ClientNetEventHandler::onConnect(uint32_t /*peerId*/) {
    m_connected = true;
    logger.log(LogLevel::Info, __FILE__, __LINE__, "connected to local fl-server");
}

void ClientNetEventHandler::signalFailure(SessionFailure f) {
    if (!sessionFailure)
        return;
    SessionFailure expected = SessionFailure::None;
    sessionFailure->compare_exchange_strong(expected, f, std::memory_order_release, std::memory_order_relaxed);
}

void ClientNetEventHandler::onDisconnect(uint32_t /*peerId*/) {
    logger.log(LogLevel::Info, __FILE__, __LINE__, "disconnected from local fl-server");
    // ENet-level rejection before MsgConnectAck — generic fallback (a specific reason set earlier by
    // the MsgHello/MsgConnectRefusal handlers wins via signalFailure's first-writer-wins CAS).
    if (m_connected && assignedEntityIdx == 0)
        signalFailure(SessionFailure::ConnectionRefused);
}

void ClientNetEventHandler::onReceive(uint32_t /*peerId*/, const void* data, std::size_t size) {
    if (size < 1)
        return;
    const uint8_t msgId = *static_cast<const uint8_t*>(data);

    if (msgId == static_cast<uint8_t>(fl::MsgId::Hello)) {
        fl::MsgHello hello;
        if (!fl::readMsg(data, size, hello))
            return;
        if (hello.protocolVersion != fl::kProtocolVersion) {
            logger.log(LogLevel::Error, __FILE__, __LINE__, "server protocol version mismatch — disconnecting");
            signalFailure(SessionFailure::VersionMismatch);
            net.disconnect();
        }
        return;
    }

    if (msgId == static_cast<uint8_t>(fl::MsgId::ConnectAck)) {
        fl::MsgConnectAck ack;
        if (!fl::readMsg(data, size, ack))
            return;
        assignedEntityIdx = ack.assignedEntityIdx;
        assignedEntityGen = ack.assignedEntityGen;
        m_planetRadiusKm = ack.planetRadiusKm;
        std::size_t off = sizeof(ack);
        for (uint16_t i = 0; i < ack.typeCount; ++i) {
            fl::MsgEntityTypeDef td;
            if (!fl::readRecordAt(data, size, off, td))
                break;
            off += sizeof(td);
            if (registry.findById(td.id))
                continue; // already registered
            fl::EntityDef def;
            def.id = td.id;
            def.mesh = td.mesh;
            def.classicDamageMesh = td.dmgMesh;
            def.maxHp = 100.0f;
            registry.registerType(std::move(def));
        }
    } else if (msgId == static_cast<uint8_t>(fl::MsgId::WorldSnapshot)) {
        fl::MsgWorldSnapshotHeader hdr;
        if (!fl::readMsg(data, size, hdr))
            return;

        // Out-of-order / duplicate guard: UDP can reorder, so ignore any snapshot not newer than the
        // last one processed. This keeps m_lastSnapshotTick a monotonic high-water mark — it is echoed
        // back to the server (MsgClientInput/MsgHeartbeat tickIndex) as the snapshot ack that drives
        // client-acked delta baselines, and it prevents a stale packet from clobbering newer state.
        if (m_haveSnapshot && hdr.tickIndex <= m_lastSnapshotTick)
            return;

        // The priority/budget scheduler (#516) may omit low-priority entities from any given
        // snapshot, so the rendered set is a persistent cache (m_entityCache) updated by each packet,
        // not rebuilt from scratch. Order of operations:
        //   1. Apply the SnapshotDespawn TLV first (so a kill-then-reuse-same-idx, where the despawn of
        //      the old gen and the full record of the new gen share one packet, resolves to the new
        //      entity rather than deleting it).
        //   2. Decode + upsert this packet's records.
        //   3. Age out entries not seen within kSnapshotRetentionTicks (interest-out / lost despawns).
        //   4. Build the RenderSnapshot from the whole cache.
        const std::size_t extOffset = sizeof(fl::MsgWorldSnapshotHeader) + hdr.bitstreamBytes;
        const uint8_t* ext = (size > extOffset) ? static_cast<const uint8_t*>(data) + extOffset : nullptr;
        const std::size_t extSz = (size > extOffset) ? size - extOffset : 0u;

        // 1. Explicit despawns (applied before record upsert).
        if (ext) {
            uint16_t despawnLen{};
            const uint8_t* dp = fl::findExt(ext, extSz, static_cast<uint16_t>(fl::ExtTag::SnapshotDespawn), despawnLen);
            for (uint16_t off = 0; dp && off + 4u <= despawnLen; off += 4u) {
                uint32_t idx{};
                std::memcpy(&idx, dp + off, 4u); // payload is unaligned — read per element
                m_entityCache.erase(idx);
                m_knownEntities.erase(idx);
            }
        }

        // 2. Decode the quantized record bitstream. Positions are relative to hdr.frameOrigin; full
        // records carry typeIndex + gen, deltas reuse the per-entity cache (m_knownEntities).
        const std::size_t bodyAvail = (size >= sizeof(hdr)) ? (size - sizeof(hdr)) : 0u;
        const std::size_t bodyBytes = std::min<std::size_t>(hdr.bitstreamBytes, bodyAvail);
        fl::BitReader reader(static_cast<const uint8_t*>(data) + sizeof(hdr), bodyBytes);
        uint32_t prevIdx = 0;
        for (uint16_t i = 0; i < hdr.recordCount; ++i) {
            fl::QuantEntity qe;
            bool genPresent = false;
            if (!fl::decodeRecord(reader, qe, prevIdx, hdr.frameOrigin, genPresent))
                break; // truncated/malformed — stop, keep what decoded

            auto kit = m_knownEntities.find(qe.idx);
            if (qe.isFull) {
                // Full record: typeIndex + gen on the wire; refresh the cache.
                m_knownEntities[qe.idx] = {static_cast<uint16_t>(qe.gen), qe.typeIndex};
            } else {
                if (kit == m_knownEntities.end())
                    continue; // full record was dropped; entity reappears on the next baseline tick
                if (genPresent && static_cast<uint16_t>(qe.gen) != kit->second.gen)
                    continue; // stale generation
                if (!genPresent)
                    qe.gen = kit->second.gen; // cached generation
                qe.typeIndex = kit->second.typeIndex;
            }

            fl::EntityRenderEntry re;
            re.entityIdx = qe.idx;
            re.entityGen = qe.gen;
            re.typeIndex = qe.typeIndex;
            re.position = {qe.pos[0], qe.pos[1], qe.pos[2]};
            re.velocity = {qe.vel[0], qe.vel[1], qe.vel[2]};
            // Wire quaternion order x,y,z,w — glm::quat constructor is (w,x,y,z).
            re.orientation = glm::quat(qe.quat[3], qe.quat[0], qe.quat[1], qe.quat[2]);
            re.damageLevel = qe.damageLevel;
            re.playerOwned = qe.playerOwned;
            re.throttle = qe.throttle;
            re.fuelPct = qe.fuelPct;
            re.abEngaged = qe.abEngaged;
            re.engineFailFlags = qe.engineFailFlags;
            re.omega = {qe.omega[0], qe.omega[1], qe.omega[2]};
            m_entityCache[qe.idx] = {re, hdr.tickIndex};
        }

        // 3. Age out entities not refreshed within the retention window (the backstop for interest-out
        // and dropped despawn packets). Evict from both caches together.
        for (auto it = m_entityCache.begin(); it != m_entityCache.end();) {
            const uint64_t age =
                (hdr.tickIndex >= it->second.lastSeenTick) ? (hdr.tickIndex - it->second.lastSeenTick) : 0u;
            if (age > fl::kSnapshotRetentionTicks) {
                m_knownEntities.erase(it->first);
                it = m_entityCache.erase(it);
            } else {
                ++it;
            }
        }

        // 4. Build the RenderSnapshot from the retained cache.
        fl::RenderSnapshot snap;
        snap.tickIndex = hdr.tickIndex;
        snap.entries.reserve(m_entityCache.size());
        for (const auto& [idx, cached] : m_entityCache)
            snap.entries.push_back(cached.re);

        // Remaining TLVs (order-independent).
        if (ext) {
            uint16_t pc{};
            if (fl::readExtValue(ext, extSz, static_cast<uint16_t>(fl::ExtTag::SnapshotPeerCount), pc))
                m_serverPeerCount.store(pc, std::memory_order_relaxed);

            uint16_t lat{};
            if (fl::readExtValue(ext, extSz, static_cast<uint16_t>(fl::ExtTag::SnapshotPeerLatency), lat)) {
                m_snapshotLatencyMs = lat;
                m_hasSnapshotLatency = true;
            }

            uint16_t delayTicks{};
            if (fl::readExtValue(ext, extSz, static_cast<uint16_t>(fl::ExtTag::SnapshotPeerDelayTicks), delayTicks))
                m_estimatedDelayTicks = delayTicks;
        }

        m_lastSnapshotTick = hdr.tickIndex;
        m_haveSnapshot = true;

        char traceBuf[96];
        std::snprintf(traceBuf, sizeof(traceBuf), "WorldSnapshot: records=%u bytes=%u built=%zu", hdr.recordCount,
                      hdr.bitstreamBytes, snap.entries.size());
        logger.log(LogLevel::Trace, __FILE__, __LINE__, traceBuf);
        if (snapshotCallback)
            snapshotCallback(snap, snap.tickIndex, m_estimatedDelayTicks);
        bridge.publishExternal(std::move(snap));
        tickAlpha.markNewTick();
    } else if (msgId == static_cast<uint8_t>(fl::MsgId::WeatherState)) {
        fl::MsgWeatherState ws;
        if (!fl::readMsg(data, size, ws))
            return;
        float tod = static_cast<float>(ws.timeOfDayTenths) / 10.f;
        env.fogDensity = ws.fogDensity;
        env.fogStartDist = ws.fogStartDist;
        env.timeOfDay = tod;
        fl::WeatherController::applyPresetToEnv(static_cast<fl::WeatherPreset>(ws.preset), tod, env);
        env.windX = ws.windX;
        env.windZ = ws.windZ;
    } else if (msgId == static_cast<uint8_t>(fl::MsgId::ServerNotice)) {
        fl::MsgServerNotice sn;
        if (!fl::readMsg(data, size, sn))
            return;
        sn.text[59] = '\0';
        char noticeBuf[72];
        std::snprintf(noticeBuf, sizeof(noticeBuf), "[server] %s", sn.text);
        if (console)
            console->print(std::string(noticeBuf));
        if (notice)
            notice->setNotice(noticeBuf, sn.secondsRemaining);
    } else if (msgId == static_cast<uint8_t>(fl::MsgId::AdminResponse)) {
        fl::MsgAdminResponse resp;
        if (!fl::readMsg(data, size, resp))
            return;
        resp.text[sizeof(resp.text) - 1] = '\0';
        if (console && resp.text[0] != '\0')
            printAdminLines(console, resp.text);
    } else if (msgId == static_cast<uint8_t>(fl::MsgId::AdminResponseChunk)) {
        fl::MsgAdminResponseChunk chunk{};
        if (!fl::readMsg(data, size, chunk))
            return;
        chunk.body[sizeof(chunk.body) - 1] = '\0';
        std::size_t bodyLen = std::strlen(chunk.body);
        if (m_chunkBufActive && m_chunkBuf.size() + bodyLen > kMaxChunkAssemblyBytes) {
            m_chunkBuf.clear();
            m_chunkBufActive = false;
            return;
        }
        m_chunkBufActive = true;
        m_chunkBuf.append(chunk.body, bodyLen);
        if (chunk.flags & fl::kChunkFlagEnd) {
            if (console && !m_chunkBuf.empty())
                printAdminLines(console, m_chunkBuf);
            m_chunkBuf.clear();
            m_chunkBufActive = false;
        }
    } else if (msgId == static_cast<uint8_t>(fl::MsgId::Motd)) {
        fl::MsgMotdHeader mh;
        if (!fl::readMsg(data, size, mh))
            return;
        const uint32_t effectiveSecs =
            mh.displaySeconds > 0 ? static_cast<uint32_t>(mh.displaySeconds) : motdDisplaySeconds;
        const std::size_t textLen = std::min(size - sizeof(mh), fl::kMaxMotdBytes);
        std::string text(static_cast<const char*>(data) + sizeof(mh), textLen);
        while (!text.empty() && text.back() == '\0')
            text.pop_back();
        std::istringstream stream(text);
        std::string line;
        bool first = true;
        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty())
                continue;
            std::string prefixed = std::string("[server] ") + line;
            if (console)
                console->print(prefixed);
            if (notice && first)
                notice->setNotice(prefixed, 0, effectiveSecs);
            first = false;
        }
    } else if (msgId == static_cast<uint8_t>(fl::MsgId::ConnectRefusal)) {
        fl::MsgConnectRefusal ref{};
        if (!fl::readMsg(data, size, ref))
            return;
        SessionFailure f = SessionFailure::ConnectionRefused;
        switch (static_cast<fl::ConnectRefusalCode>(ref.code)) {
        case fl::ConnectRefusalCode::Banned:
            f = SessionFailure::Banned;
            break;
        case fl::ConnectRefusalCode::AccessDenied:
        case fl::ConnectRefusalCode::AdminLockout:
            f = SessionFailure::AccessDenied;
            break;
        case fl::ConnectRefusalCode::RateLimited:
            f = SessionFailure::RateLimited;
            break;
        case fl::ConnectRefusalCode::TooManyConnections:
            f = SessionFailure::TooManyConnections;
            break;
        case fl::ConnectRefusalCode::Generic:
            break; // ConnectionRefused
        }
        signalFailure(f);
    } else if (msgId == static_cast<uint8_t>(fl::MsgId::PeerDelay)) {
        fl::MsgPeerDelay pd;
        if (!fl::readMsg(data, size, pd))
            return;
        if (pd.delayTicks > 0) {
            m_lastRttMs = static_cast<uint32_t>(pd.delayTicks) * 1000u / 60u;
            m_rttValid = true;
        }
    }
    // Unknown msgIds: silently discard
}

void ClientNetEventHandler::sendHeartbeatIfNeeded() {
    if (m_lastSnapshotTick == 0)
        return; // guard: tickIndex=0 would yield a bogus server-side delay estimate

    using namespace std::chrono;
    const auto now = m_clock->now();
    if (now - m_lastHeartbeatSentAt < seconds(1))
        return;
    m_lastHeartbeatSentAt = now;

    fl::MsgHeartbeat hb;
    hb.tickIndex = m_lastSnapshotTick;
    net.send(0, &hb, sizeof(hb), /*reliable=*/false);
}

} // namespace fl