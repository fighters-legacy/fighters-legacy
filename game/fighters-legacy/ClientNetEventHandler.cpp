// SPDX-License-Identifier: GPL-3.0-or-later
#include "ClientNetEventHandler.h"
#include "ClientEffectRouter.h"
#include "ServerNotice.h"

#include "ILogger.h"
#include "INetwork.h"
#include "console/GameConsole.h"
#include "entity/EntityDef.h"
#include "entity/EntityTypeRegistry.h"
#include "net/AckWindow.h"
#include "net/BitStream.h"
#include "net/GameProtocol.h"
#include "net/SnapshotCodec.h"
#include "net/SnapshotCompression.h"
#include "net/SnapshotScheduler.h" // kSnapshotRetentionTicks
#include "net/WireCodec.h"
#include "render/RenderSnapshot.h"
#include "render/SimRenderBridge.h"
#include "weather/WeatherController.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <glm/gtc/quaternion.hpp>
#include <sstream>
#include <vector>

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

    // Unified connect handshake (#853): the client speaks first now. Send MsgConnectRequest with the
    // requested role, aircraft, and mounted-pack manifest; the server replies MsgConnectAck (granted
    // role + assigned entity) or MsgConnectRefusal. Followed by the pack manifest records; the ext
    // block (RFC #871 entitlement token) is reserved and empty.
    fl::MsgConnectRequest req{};
    req.requestedRole = static_cast<uint8_t>(requestedRole);
    req.packCount = static_cast<uint16_t>(std::min<std::size_t>(packManifest.size(), 0xFFFFu));
    std::snprintf(req.requestedEntityType, sizeof(req.requestedEntityType), "%s", requestedEntityType.c_str());
    std::snprintf(req.callsign, sizeof(req.callsign), "%s", requestedCallsign.c_str()); // match roster (#996)

    std::vector<uint8_t> buf;
    buf.reserve(sizeof(req) + packManifest.size() * sizeof(fl::PackManifestEntry));
    fl::appendMsg(buf, req);
    for (uint16_t i = 0; i < req.packCount; ++i)
        fl::appendMsg(buf, packManifest[i]);
    // Reconnect identity (#524): the client GUID, so the server can restore team + score on reconnect.
    if (!requestedGuid.empty()) {
        const std::string g = requestedGuid.substr(0, 40);
        fl::appendExtRaw(buf, static_cast<uint16_t>(fl::ExtTag::ConnectIdentity), g.data(),
                         static_cast<uint16_t>(g.size()));
    }
    // Join password (#998): sent only when the client supplies one for a passworded server.
    if (!requestedJoinPassword.empty()) {
        const std::string p = requestedJoinPassword.substr(0, 64);
        fl::appendExtRaw(buf, static_cast<uint16_t>(fl::ExtTag::ConnectJoinPassword), p.data(),
                         static_cast<uint16_t>(p.size()));
    }
    net.send(0, buf.data(), buf.size(), /*reliable=*/true);
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
    // the MsgHello/MsgConnectRefusal handlers wins via signalFailure's first-writer-wins CAS). Keyed on
    // "no ConnectAck arrived", not assignedEntityIdx==0: an observer's valid ack carries idx 0 (#853/#857).
    if (m_connected && !m_gotConnectAck)
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
        m_selfPeerId = ack.peerId; // our own participant id, for roster "you" + chat self-echo (#996)
        m_planetRadiusKm = ack.planetRadiusKm;
        m_grantedRole =
            fl::isPeerRoleOrdinal(ack.grantedRole) ? static_cast<fl::PeerRole>(ack.grantedRole) : fl::PeerRole::Pilot;
        m_gotConnectAck = true; // admitted — an observer's ack has idx 0, so this (not idx) marks success
        std::size_t off = sizeof(ack);
        for (uint16_t i = 0; i < ack.typeCount; ++i) {
            fl::MsgEntityTypeDef td;
            if (!fl::readRecordAt(data, size, off, td))
                break;
            off += sizeof(td);
            // Force-terminate the fixed-size wire char[] fields before treating them as C strings: a
            // malicious server may send a fully-populated (non-NUL-terminated) field, which would
            // otherwise over-read past the array in findById/std::string (matches how every other
            // handler here terminates its char[] fields).
            td.id[sizeof(td.id) - 1] = '\0';
            td.mesh[sizeof(td.mesh) - 1] = '\0';
            td.dmgMesh[sizeof(td.dmgMesh) - 1] = '\0';
            td.flightModel[sizeof(td.flightModel) - 1] = '\0';
            td.name[sizeof(td.name) - 1] = '\0';
            if (registry.findById(td.id))
                continue; // already registered
            fl::EntityDef def;
            def.id = td.id;
            def.mesh = td.mesh;
            def.classicDamageMesh = td.dmgMesh;
            // Friendly display name for the observer entity picker (#860); empty on the wire means the
            // picker falls back to the id.
            def.name = td.name[0] ? td.name : td.id;
            // Which aircraft to integrate, and what its default loadout costs (#811/#812). Both
            // arrive on the wire because the client must not re-derive them: when it did, it
            // derived them wrong and flew a different aeroplane from the server.
            def.flightModelAsset = td.flightModel;
            def.payloadMassKg = td.payloadMassKg;
            def.payloadCd0 = td.payloadCd0;
            // Category + projectile weapon class (#886) select the builtin placeholder silhouette.
            // Wire ordinals are gated before the enum cast (a malicious server may send any byte);
            // invalid values fall back to the pre-#886 defaults (AirVehicle / None).
            def.category = fl::isObjectCategoryOrdinal(td.category) ? static_cast<fl::ObjectCategory>(td.category)
                                                                    : fl::ObjectCategory::AirVehicle;
            def.projectileKind = fl::isProjectileKindOrdinal(td.projectileKind)
                                     ? static_cast<fl::ProjectileKind>(td.projectileKind)
                                     : fl::ProjectileKind::None;
            // Flight-deck footprint (#38): non-zero length = this type carries a deck the client's
            // prediction floor must compose (max(terrain, moving deck), same as the server). Only
            // the footprint travels; catapult/arrest are server-authoritative and stay there.
            if (td.deckLengthM > 0.f && td.deckWidthM > 0.f) {
                fl::DeckDef deck;
                deck.lengthM = td.deckLengthM;
                deck.widthM = td.deckWidthM;
                deck.heightM = td.deckHeightM;
                def.deck = deck;
                def.acceptsLandings = true;
            }
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
        // (Deliberately checked BEFORE decompression — the header is always raw, so a stale packet
        // never costs a decompress.)
        if (m_haveSnapshot && hdr.tickIndex <= m_lastSnapshotTick)
            return;

        // Compressed payload (#775): everything after the raw 24-byte header is one zstd frame.
        // Rebuild [header][decompressed payload] in the reused scratch and repoint data/size, so the
        // whole parse path below runs unchanged on either form. decompressSnapshotPayload fails
        // closed on an oversized claim, a malformed frame, or a length mismatch — drop the packet
        // (the ack mask simply never marks the tick; the server keeps that entity on fulls).
        if (hdr.flags & fl::kSnapshotFlagCompressed) {
            const auto* comp = static_cast<const uint8_t*>(data) + sizeof(fl::MsgWorldSnapshotHeader);
            const std::size_t compSize = size - sizeof(fl::MsgWorldSnapshotHeader);
            if (!fl::decompressSnapshotPayload(comp, compSize, hdr.uncompressedBytes, m_decompressScratch))
                return;
            m_snapshotScratch.resize(sizeof(fl::MsgWorldSnapshotHeader) + m_decompressScratch.size());
            std::memcpy(m_snapshotScratch.data(), data, sizeof(fl::MsgWorldSnapshotHeader));
            std::memcpy(m_snapshotScratch.data() + sizeof(fl::MsgWorldSnapshotHeader), m_decompressScratch.data(),
                        m_decompressScratch.size());
            data = m_snapshotScratch.data();
            size = m_snapshotScratch.size();
        }

        // The priority/budget scheduler (#516) may omit low-priority entities from any given
        // snapshot, so the rendered set is a persistent cache (m_entityCache) updated by each packet,
        // not rebuilt from scratch. Order of operations:
        //   1. Apply the SnapshotDespawn TLV first (so a kill-then-reuse-same-idx, where the despawn of
        //      the old gen and the full record of the new gen share one packet, resolves to the new
        //      entity rather than deleting it).
        //   2. Decode + upsert this packet's records.
        //   3. Age out entries not seen within kSnapshotRetentionTicks (interest-out / lost despawns).
        //   4. Build the RenderSnapshot from the whole cache.
        // Body layout (#725): [origin table: originCount x double[3]][stitched record stream][TLV].
        const std::size_t originBytes = static_cast<std::size_t>(hdr.originCount) * 3u * sizeof(double);
        const std::size_t recordOffset = sizeof(fl::MsgWorldSnapshotHeader) + originBytes;
        const std::size_t extOffset = recordOffset + hdr.bitstreamBytes;
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

        // Read the shared-origin table (double[3] each; unaligned-safe memcpy, bounded by the packet).
        // originsOk == false means a truncated packet — skip record decode and keep the existing cache.
        // Only allocate the table when the packet actually contains it: a malformed header can claim a
        // huge originCount, so never size the buffer off the claim alone (allocation-amplification DoS).
        const bool originsOk = recordOffset <= size;
        std::vector<double> originTable;
        if (originsOk && hdr.originCount > 0) {
            originTable.resize(static_cast<std::size_t>(hdr.originCount) * 3u);
            std::memcpy(originTable.data(), static_cast<const uint8_t*>(data) + sizeof(fl::MsgWorldSnapshotHeader),
                        originBytes);
        }

        // 2. Decode the stitched record stream (#725). Each record carries an origin index into the
        // table above; positions are relative to that shared origin. Full records carry typeIndex + gen,
        // deltas reuse the per-entity cache (m_knownEntities).
        const std::size_t recordAvail = (size > recordOffset) ? (size - recordOffset) : 0u;
        const std::size_t recordBytes = std::min<std::size_t>(hdr.bitstreamBytes, recordAvail);
        fl::BitReader reader(static_cast<const uint8_t*>(data) + recordOffset, recordBytes);
        for (uint16_t i = 0; originsOk && i < hdr.recordCount; ++i) {
            fl::QuantEntity qe;
            bool genPresent = false;
            if (!fl::decodeStandaloneRecord(reader, qe, originTable.data(), hdr.originCount, genPresent))
                break; // truncated/malformed — stop, keep what decoded

            auto kit = m_knownEntities.find(qe.idx);
            if (qe.isFull) {
                // Full record: typeIndex + factionIndex + gen on the wire; refresh the cache.
                m_knownEntities[qe.idx] = {static_cast<uint16_t>(qe.gen), qe.typeIndex, qe.factionIndex};
            } else {
                if (kit == m_knownEntities.end())
                    continue; // full record was dropped; entity reappears on the next baseline tick
                if (genPresent && static_cast<uint16_t>(qe.gen) != kit->second.gen)
                    continue; // stale generation
                if (!genPresent)
                    qe.gen = kit->second.gen; // cached generation
                qe.typeIndex = kit->second.typeIndex;
                qe.factionIndex = kit->second.factionIndex; // #860: cached like typeIndex
            }

            fl::EntityRenderEntry re;
            re.entityIdx = qe.idx;
            re.entityGen = qe.gen;
            re.typeIndex = qe.typeIndex;
            re.factionIndex = qe.factionIndex;
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
            // Own-record loadout block (#625) — travels with omega, own entity only.
            re.hasLoadout = qe.hasOmega;
            re.selectedStation = qe.selectedStation;
            re.stationRounds = qe.stationRounds;
            re.weaponFlags = qe.weaponFlags;
            re.payloadMassKg = qe.payloadMassKg;
            re.payloadCd0 = qe.payloadCd0;
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
        uint32_t ackedSeqNum = kNoAckedSeqNum; // #427: overwritten below if the server reported one
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

            // Exact acked seqNum (#427): the seqNum the server last applied for us. Present only once
            // the server has applied one of our inputs; absent → prediction falls back to delay ticks.
            uint32_t acked{};
            if (fl::readExtValue(ext, extSz, static_cast<uint16_t>(fl::ExtTag::SnapshotLastAckedSeqNum), acked))
                ackedSeqNum = acked;

            // Cosmetic weapon effects (#625): variable-length record list, routed to particles
            // (audio/haptics join via the same router in #631). Missing router = effects dropped.
            if (effects) {
                uint16_t fxLen = 0;
                if (const uint8_t* fx =
                        fl::findExt(ext, extSz, static_cast<uint16_t>(fl::ExtTag::SnapshotEffects), fxLen);
                    fx && fxLen > 0)
                    routeEffectsTlv(*effects, fx, fxLen);
            }

            // Live crew turret pose (#972): decode each crewed entity's mount-frame az/el for the
            // gunner station / turret rendering (#975/#979). Absent for a single-seat-only interest set.
            uint16_t crewLen = 0;
            if (const uint8_t* cp = fl::findExt(ext, extSz, static_cast<uint16_t>(fl::ExtTag::SnapshotCrew), crewLen);
                cp && crewLen > 0)
                applyCrewTurretTlv(cp, crewLen);
        }

        // Advance the selective-ack decoded-tick mask before moving the high-water mark (#566). This
        // path only runs for snapshots we accept and DECODE, so every bit ackAdvance sets corresponds to
        // a tick whose records were actually applied — a dropped or reordered-and-discarded tick leaves
        // its bit 0. stampAck() then reports {m_lastSnapshotTick, m_ackMask} to the server.
        m_ackMask = fl::ackAdvance(m_ackMask, m_lastSnapshotTick, hdr.tickIndex, m_haveSnapshot);
        m_lastSnapshotTick = hdr.tickIndex;
        m_haveSnapshot = true;

        char traceBuf[96];
        std::snprintf(traceBuf, sizeof(traceBuf), "WorldSnapshot: records=%u bytes=%u built=%zu", hdr.recordCount,
                      hdr.bitstreamBytes, snap.entries.size());
        logger.log(LogLevel::Trace, __FILE__, __LINE__, traceBuf);
        if (snapshotCallback)
            snapshotCallback(snap, snap.tickIndex, m_estimatedDelayTicks, ackedSeqNum);
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
        env.turbulenceAmp = ws.turbulenceAmp; // #426: fed to weatherTurbulence() in ClientPrediction
        // #481: store the shared UTC clock; Game.cpp computes the per-camera geographic sun each frame
        // (the direction depends on the camera lat/lon, which moves faster than these ~6 Hz packets).
        m_utcJulianDay = ws.utcJulianDay;

        // Altitude wind profile TLV (#489): parse the tail after the fixed struct into env.windProfile.
        // Absent (old server / no profile) leaves count 0 -> ClientPrediction uses the datum scalar.
        env.windProfileCount = 0;
        if (size > sizeof(fl::MsgWeatherState)) {
            const uint8_t* ext = static_cast<const uint8_t*>(data) + sizeof(fl::MsgWeatherState);
            const std::size_t extSize = size - sizeof(fl::MsgWeatherState);
            uint16_t valueLen = 0;
            const uint8_t* p =
                fl::findExt(ext, extSize, static_cast<uint16_t>(fl::ExtTag::WeatherWindProfile), valueLen);
            if (p && valueLen >= 1) {
                const uint8_t count = p[0];
                const std::size_t need = 1u + static_cast<std::size_t>(count) * 12u;
                if (count <= fl::EnvironmentState::kWindProfileMaxKnots && valueLen >= need) {
                    for (int i = 0; i < count; ++i) {
                        const uint8_t* rec = p + 1 + static_cast<std::size_t>(i) * 12u;
                        std::memcpy(&env.windProfile[i].altM, rec, 4);
                        std::memcpy(&env.windProfile[i].windX, rec + 4, 4);
                        std::memcpy(&env.windProfile[i].windZ, rec + 8, 4);
                    }
                    env.windProfileCount = count;
                }
            }
        }
    } else if (msgId == static_cast<uint8_t>(fl::MsgId::MusicState)) {
        // Server-driven music transition (#413/#166): a Lua/mission script called world.set_music_state.
        // Hand the GameState ordinal to the game, which drives MusicManager on the main thread.
        fl::MsgMusicState ms;
        if (!fl::readMsg(data, size, ms))
            return;
        if (musicStateCallback)
            musicStateCallback(ms.state);
    } else if (msgId == static_cast<uint8_t>(fl::MsgId::Haptic)) {
        // Scripted rumble (#128): a Lua script's rumble()/stop_rumble() plays on the local gamepad.
        fl::MsgHaptic hp;
        if (!fl::readMsg(data, size, hp))
            return;
        if (hapticCallback && fl::isHapticKindOrdinal(hp.kind))
            hapticCallback(hp.kind, hp.a, hp.b, hp.durationMs);
    } else if (msgId == static_cast<uint8_t>(fl::MsgId::MissionOutcome)) {
        // The objective evaluator ended the mission (#584): record the real result so the debrief
        // reports success/failure instead of a hardcoded success.
        fl::MsgMissionOutcome mo;
        if (!fl::readMsg(data, size, mo))
            return;
        if (fl::isMissionResultOrdinal(mo.outcome))
            m_missionOutcome = static_cast<fl::MissionResultCode>(mo.outcome);
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
    } else if (msgId == static_cast<uint8_t>(fl::MsgId::RadioTransmission)) {
        fl::MsgRadioTransmission rt;
        if (!fl::readMsg(data, size, rt))
            return;
        rt.speaker[sizeof(rt.speaker) - 1] = '\0';
        rt.voiceKey[sizeof(rt.voiceKey) - 1] = '\0';
        rt.text[sizeof(rt.text) - 1] = '\0';
        if (console) {
            char line[224];
            std::snprintf(line, sizeof(line), "[radio] %s: %s", rt.speaker, rt.text);
            console->print(std::string(line));
        }
        if (radioCallback)
            radioCallback(rt.speaker, rt.text, rt.voiceKey, rt.displaySeconds);
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
        case fl::ConnectRefusalCode::RoleDenied:
            f = SessionFailure::RoleDenied;
            break;
        case fl::ConnectRefusalCode::MissingRequiredPack:
            f = SessionFailure::MissingRequiredPack;
            // The refusal reason carries the specific missing-pack list (#872). Surface it to the console
            // so the player sees which packs to install, not just the generic banner string.
            ref.reason[sizeof(ref.reason) - 1] = '\0';
            if (console && ref.reason[0] != '\0')
                console->print(std::string("[server] ") + ref.reason);
            break;
        case fl::ConnectRefusalCode::EntitlementRequired:
            f = SessionFailure::EntitlementRequired;
            break;
        case fl::ConnectRefusalCode::MatchFull:
            f = SessionFailure::MatchFull;
            break;
        case fl::ConnectRefusalCode::BadPassword:
            f = SessionFailure::BadPassword;
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
    } else if (msgId == static_cast<uint8_t>(fl::MsgId::WingmanAck)) {
        // Order outcome, the on-connect flight check-in, or a radio call relayed to us as a human
        // member of someone's flight (#610). All three land on the menu, which renders the brevity.
        fl::MsgWingmanAck ack;
        if (!fl::readMsg(data, size, ack))
            return;
        if (wingman)
            wingman->onAck(ack);
    } else if (msgId == static_cast<uint8_t>(fl::MsgId::CombatEvent)) {
        // Kill feed + own combat stats (#626). Reliable, so a kill credit is never lost the way a
        // cosmetic effect can be.
        fl::MsgCombatEventHeader hdr;
        if (!fl::readMsg(data, size, hdr))
            return;
        for (uint8_t i = 0; i < hdr.count; ++i) {
            fl::CombatEventRecord rec;
            if (!fl::readRecordAt(data, size, sizeof(hdr) + std::size_t(i) * sizeof(rec), rec))
                break; // truncated packet: fail closed on the remainder

            if (rec.type == static_cast<uint8_t>(fl::CombatEventType::Stats)) {
                m_sessionStats.kills = rec.a;
                m_sessionStats.losses = rec.b;
                m_sessionStats.score = rec.c;
                continue;
            }
            if (rec.type != static_cast<uint8_t>(fl::CombatEventType::Kill))
                continue; // unknown record types are skipped — the vocabulary grows without breaking us

            const bool youDied = rec.subjectIdx == assignedEntityIdx && rec.subjectGen != 0 &&
                                 rec.subjectGen == static_cast<uint16_t>(assignedEntityGen);
            const bool youKilled = rec.instigatorIdx == assignedEntityIdx && rec.instigatorGen != 0 &&
                                   rec.instigatorGen == static_cast<uint16_t>(assignedEntityGen);

            // Per-class kill tally for the pilot logbook (#674): classify the victim by its entity
            // category (via the cached typeIndex + type registry) when this peer scored the kill.
            if (youKilled) {
                int cls = 0; // ObjectCategory::AirVehicle default when the victim type is unknown
                if (auto kit = m_knownEntities.find(rec.subjectIdx); kit != m_knownEntities.end()) {
                    if (const fl::EntityDef* vd = registry.byIndex(kit->second.typeIndex))
                        cls = static_cast<int>(vd->category);
                }
                if (cls >= 0 && cls < 8)
                    ++m_sessionStats.killsByClass[cls];
            }

            // Names arrive with chat/scoreboard (Epic E); until then the feed speaks in entities.
            char who[32];
            char whom[32];
            if (youKilled)
                std::snprintf(who, sizeof(who), "you");
            else if (rec.a != fl::kNoOwningPeer)
                std::snprintf(who, sizeof(who), "peer %u", rec.a);
            else if (rec.instigatorIdx == 0xFFFFFFFFu)
                std::snprintf(who, sizeof(who), "the environment");
            else
                std::snprintf(who, sizeof(who), "entity %u", rec.instigatorIdx);
            if (youDied)
                std::snprintf(whom, sizeof(whom), "you");
            else if (rec.b != fl::kNoOwningPeer)
                std::snprintf(whom, sizeof(whom), "peer %u", rec.b);
            else
                std::snprintf(whom, sizeof(whom), "entity %u", rec.subjectIdx);

            if (console) {
                char line[96];
                std::snprintf(line, sizeof(line), "[kill] %s destroyed %s", who, whom);
                console->print(line);
            }
            if (notice && youDied)
                notice->setNotice("YOU WERE DESTROYED", 0, 5);
            else if (notice && youKilled) {
                char banner[64];
                std::snprintf(banner, sizeof(banner), "DESTROYED %s", whom);
                notice->setNotice(banner, 0, 5);
            }
        }
    } else if (msgId == static_cast<uint8_t>(fl::MsgId::FactionDef)) {
        // Faction index -> name table (#860), one reliable packet of concatenated records. Store the
        // display names so the observer entity picker can label an entity's faction from its snapshot
        // factionIndex. Force-terminate the fixed char[] fields before treating them as C strings.
        const std::size_t count = size / sizeof(fl::MsgFactionDef);
        for (std::size_t i = 0; i < count; ++i) {
            fl::MsgFactionDef fd;
            if (!fl::readRecordAt(data, size, i * sizeof(fd), fd))
                break;
            fd.id[sizeof(fd.id) - 1] = '\0';
            fd.name[sizeof(fd.name) - 1] = '\0';
            m_factionNames[fd.factionIndex] = fd.name[0] ? fd.name : fd.id;
        }
    } else if (msgId == static_cast<uint8_t>(fl::MsgId::PlayerRoster)) {
        // Match roster upsert/leave stream (#996): header + count records. Dedup by participantId; a
        // kRosterLeave entry removes the row. The single name source for chat/kill feed/scoreboard.
        fl::MsgPlayerRosterHeader hdr;
        if (!fl::readMsg(data, size, hdr))
            return;
        std::size_t off = sizeof(hdr);
        for (uint8_t i = 0; i < hdr.count; ++i) {
            fl::PlayerRosterEntry e;
            if (!fl::readRecordAt(data, size, off, e))
                break;
            off += sizeof(e);
            if (e.flags & fl::kRosterLeave) {
                m_roster.erase(e.participantId);
                continue;
            }
            e.callsign[sizeof(e.callsign) - 1] = '\0'; // untrusted char[]: force-terminate
            RosterEntry re;
            re.callsign = e.callsign;
            re.factionIndex = e.factionIndex;
            re.role = e.role;
            re.isBot = (e.flags & fl::kRosterBot) != 0;
            m_roster[e.participantId] = std::move(re);
        }
    } else if (msgId == static_cast<uint8_t>(fl::MsgId::MissionRoster)) {
        // Mission object id -> entity idx/gen (#914), one reliable packet of concatenated records (sent
        // after ConnectAck, plus single-record deltas as player slots bind). Lets the cinematic recorder
        // resolve an entity-relative camera shot's target to a network entity. Force-terminate the id.
        const std::size_t count = size / sizeof(fl::MsgMissionRoster);
        for (std::size_t i = 0; i < count; ++i) {
            fl::MsgMissionRoster mr;
            if (!fl::readRecordAt(data, size, i * sizeof(mr), mr))
                break;
            mr.objectId[sizeof(mr.objectId) - 1] = '\0';
            if (mr.objectId[0])
                m_missionRoster[mr.objectId] = {mr.entityIdx, mr.entityGen};
        }
    } else if (msgId == static_cast<uint8_t>(fl::MsgId::Datalink)) {
        handleDatalink(data, size);
    } else if (msgId == static_cast<uint8_t>(fl::MsgId::CrewRoster)) {
        handleCrewRoster(data, size);
    } else if (msgId == static_cast<uint8_t>(fl::MsgId::SeatResult)) {
        fl::MsgSeatResult res;
        if (fl::readMsg(data, size, res)) {
            m_lastSeatResult.valid = true;
            m_lastSeatResult.fresh = true;
            m_lastSeatResult.code = res.code;
            m_lastSeatResult.seatIndex = res.seatIndex;
            m_lastSeatResult.entityIdx = res.entityIdx;
            // A granted JOIN (entityIdx set) means "I occupy a non-fly seat" — the join protocol never
            // grants the Fly seat. A granted LEAVE (entityIdx 0) sends the peer to observer. #975.
            if (res.code == static_cast<uint8_t>(fl::SeatResultCode::Granted))
                m_inCrewSeat = (res.entityIdx != 0u);
        }
    }
    // Unknown msgIds: silently discard
}

void ClientNetEventHandler::handleCrewRoster(const void* data, std::size_t size) {
    // One crewed aircraft's seat roster (#972). Header + seatCount CrewRosterSeat records. Force-
    // terminate the fixed role field before treating it as a C string; a short/malformed packet leaves
    // the previous roster in place rather than replacing it with a partial one.
    fl::MsgCrewRosterHeader hdr;
    if (!fl::readMsg(data, size, hdr))
        return;
    const std::size_t need = sizeof(hdr) + static_cast<std::size_t>(hdr.seatCount) * sizeof(fl::CrewRosterSeat);
    if (size < need)
        return;

    CrewRosterInfo info;
    info.entityIdx = hdr.entityIdx;
    info.entityGen = hdr.entityGen;
    info.turretCount = hdr.turretCount;
    info.seats.reserve(hdr.seatCount);
    for (uint8_t s = 0; s < hdr.seatCount; ++s) {
        fl::CrewRosterSeat rec;
        if (!fl::readRecordAt(data, size, sizeof(hdr) + std::size_t(s) * sizeof(rec), rec))
            return;
        rec.role[sizeof(rec.role) - 1] = '\0';
        CrewSeatInfo si;
        si.seatIndex = rec.seatIndex;
        si.occupancy = rec.occupancy;
        si.capabilities = rec.capabilities;
        si.occupantPeerId = rec.occupantPeerId;
        si.skillPct = rec.skillPct;
        si.turretIndex = rec.turretIndex;
        si.knockedOut = rec.knockedOut != 0; // #978
        si.role = rec.role;
        info.seats.push_back(std::move(si));
    }
    m_crewRosters[hdr.entityIdx] = std::move(info);
}

void ClientNetEventHandler::applyCrewTurretTlv(const uint8_t* payload, std::size_t len) {
    // Payload: uint8 entryCount, then entryCount x { uint32 entityIdx (LE), uint8 turretCount,
    // turretCount x { int16 azQ, int16 elQ } }. Bounds-checked; a truncated tail stops the scan. The
    // wire quant is symmetric: az over [-pi,pi], el over [-pi/2,pi/2], both to int16.
    constexpr float kPi = 3.14159265358979323846f;
    if (len < 1)
        return;
    const uint8_t entryCount = payload[0];
    std::size_t off = 1;
    for (uint8_t e = 0; e < entryCount; ++e) {
        if (off + 5 > len)
            return;
        uint32_t idx = 0;
        std::memcpy(&idx, payload + off, 4);
        const uint8_t tc = payload[off + 4];
        off += 5;
        if (off + static_cast<std::size_t>(tc) * 4u > len)
            return;
        std::vector<CrewTurretPose> poses;
        poses.reserve(tc);
        for (uint8_t t = 0; t < tc; ++t) {
            int16_t azQ = 0, elQ = 0;
            std::memcpy(&azQ, payload + off, 2);
            std::memcpy(&elQ, payload + off + 2, 2);
            off += 4;
            CrewTurretPose p;
            p.azRad = static_cast<float>(azQ) / 32767.f * kPi;
            p.elRad = static_cast<float>(elQ) / 32767.f * (kPi * 0.5f);
            poses.push_back(p);
        }
        m_crewTurretPoses[idx] = std::move(poses);
    }
}

void ClientNetEventHandler::handleDatalink(const void* data, std::size_t size) {
    // The fused team track picture + RWR (#528). Reconstruct absolute world positions from the header
    // origin + each record's relative offset, so the HUD needs no coordinate frame of its own. A
    // malformed / truncated packet leaves the previous picture in place rather than clearing it.
    fl::MsgDatalinkHeader hdr;
    if (!fl::readMsg(data, size, hdr))
        return;

    const std::size_t trackBytes = static_cast<std::size_t>(hdr.trackCount) * sizeof(fl::DatalinkTrack);
    const std::size_t threatBytes = static_cast<std::size_t>(hdr.threatCount) * sizeof(fl::DatalinkThreat);
    if (size < sizeof(hdr) + trackBytes + threatBytes)
        return; // short packet — do not partially apply

    std::vector<RadarTrack> tracks;
    tracks.reserve(hdr.trackCount);
    for (uint16_t i = 0; i < hdr.trackCount; ++i) {
        fl::DatalinkTrack r;
        if (!fl::readRecordAt(data, size, sizeof(hdr) + std::size_t(i) * sizeof(r), r))
            break;
        RadarTrack t;
        t.pos[0] = hdr.origin[0] + static_cast<double>(r.relPos[0]);
        t.pos[1] = hdr.origin[1] + static_cast<double>(r.relPos[1]);
        t.pos[2] = hdr.origin[2] + static_cast<double>(r.relPos[2]);
        t.vel[0] = r.relVel[0];
        t.vel[1] = r.relVel[1];
        t.vel[2] = r.relVel[2];
        t.entityIdx = r.targetIdx;
        t.entityGen = r.targetGen;
        t.state = r.state;
        t.ident = r.ident;
        t.sensorTypeMask = r.sensorTypeMask;
        t.firingQuality = (r.flags & fl::kDatalinkFlagFiringQuality) != 0;
        t.ownSensor = (r.flags & fl::kDatalinkFlagOwnSensor) != 0;
        tracks.push_back(t);
    }

    std::vector<RwrStrobe> strobes;
    strobes.reserve(hdr.threatCount);
    const std::size_t threatBase = sizeof(hdr) + trackBytes;
    for (uint16_t i = 0; i < hdr.threatCount; ++i) {
        fl::DatalinkThreat r;
        if (!fl::readRecordAt(data, size, threatBase + std::size_t(i) * sizeof(r), r))
            break;
        RwrStrobe s;
        s.emitterPos[0] = hdr.origin[0] + static_cast<double>(r.relPos[0]);
        s.emitterPos[1] = hdr.origin[1] + static_cast<double>(r.relPos[1]);
        s.emitterPos[2] = hdr.origin[2] + static_cast<double>(r.relPos[2]);
        s.emitterIdx = r.emitterIdx;
        s.channel = r.channel;
        s.level = r.level;
        s.ident = r.ident;
        strobes.push_back(s);
    }

    m_radarTracks = std::move(tracks);
    m_rwrStrobes = std::move(strobes);
    m_haveDatalink = true;
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
    stampAck(hb);
    net.send(0, &hb, sizeof(hb), /*reliable=*/false);
}

void ClientNetEventHandler::sendSeatRequest(uint32_t entityIdx, uint32_t entityGen, uint8_t seat) {
    fl::MsgSeatRequest req;
    req.seatIndex = seat;
    req.entityIdx = entityIdx;
    req.entityGen = entityGen;
    net.send(0, &req, sizeof(req), /*reliable=*/true);
}

void ClientNetEventHandler::sendSeatLeave() {
    fl::MsgSeatRequest req;
    req.flags = fl::kSeatRequestFlagLeave;
    net.send(0, &req, sizeof(req), /*reliable=*/true);
}

void ClientNetEventHandler::stampAck(fl::MsgClientInput& in) const noexcept {
    in.tickIndex = m_lastSnapshotTick;
    in.ackMask = m_ackMask;
}

void ClientNetEventHandler::stampAck(fl::MsgHeartbeat& hb) const noexcept {
    hb.tickIndex = m_lastSnapshotTick;
    hb.ackMask = m_ackMask;
}

} // namespace fl