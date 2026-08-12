// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Shared fixtures for the two broadcaster-side suites. Extracted when the admission cases moved to
// test_peer_admission.cpp (#1085): both suites drive the connect handshake through ONE
// connectPilotPeer/connectObserverPeer and decode snapshots with one decoder, because two copies
// would be free to disagree about what "a connected pilot" is — the drift that makes a green test
// meaningless.

#include "IClock.h"
#include "ILogger.h"
#include "INetwork.h"
#include "atc/AtcService.h"
#include "content/ContentBootstrap.h"
#include "entity/DamageDef.h"
#include "entity/EntityDef.h"
#include "entity/EntityManager.h"
#include "entity/EntityTypeRegistry.h"
#include "entity/IEntityController.h"
#include "flight/CentralGravityField.h"
#include "flight/EngineFailFlags.h"
#include "flight/FlightIntegrator.h"
#include "job/JobSystem.h"
#include "net/AdminChannel.h"
#include "net/BitStream.h"
#include "net/GameProtocol.h"
#include "net/InputTraceReader.h"
#include "net/SnapshotCodec.h"
#include "net/SnapshotCompression.h"
#include "net/WireCodec.h"
#include "net/WorldBroadcaster.h"
#include "render/RenderSnapshot.h"
#include "sensor/BuiltinSensors.h"
#include "weapon/BuiltinWeapon.h"
#include "weapon/ProjectileSystem.h"
#include "weapon/WeaponDefParser.h"
#include "weapon/WeaponRegistry.h"
#include "weather/WeatherController.h"
#include "world/AirportRegistry.h"
#include "world/BuiltinAirport.h"

#include "mock_network.h"

#include <algorithm>
#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

// Test-only, and included by exactly those two suites, so the directive each of them already opened
// with lives here rather than being repeated per fixture.
using namespace fl;

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

struct MockLogger : ILogger {
    void log(LogLevel, const char*, int, const char*) override {}
    void setMinLevel(LogLevel) override {}
    void flush() override {}
};

// Records Warn-level messages so a test can assert the #872 required-pack warn-only policy fires.
struct CapturingLogger : ILogger {
    std::vector<std::string> warnings;
    void log(LogLevel lvl, const char*, int, const char* msg) override {
        if (lvl == LogLevel::Warn && msg)
            warnings.emplace_back(msg);
    }
    void setMinLevel(LogLevel) override {}
    void flush() override {}
};

// Records broadcasts/sends/disconnects + resolves configurable peer addresses (see mock_network.h).
using MockNetwork = TrackingNetwork;

inline fl::EntityDef makeDebugDef(const char* id = "builtin:debug-entity") {
    fl::EntityDef def;
    def.id = id;
    def.name = "Debug";
    def.category = fl::ObjectCategory::AirVehicle;
    def.maxHp = 100.0f;
    return def;
}

// Validate that the first send is a well-formed MsgHello with the current protocol version.
inline fl::MsgHello parseSendHello(const MockNetwork& net) {
    REQUIRE(net.sends.size() >= 1u);
    REQUIRE(net.sends[0].size() == sizeof(fl::MsgHello));
    fl::MsgHello hello{};
    std::memcpy(&hello, net.sends[0].data(), sizeof(hello));
    return hello;
}

// Parse the MsgConnectAck from the second send packet (sends[1], after MsgHello).
inline fl::MsgConnectAck parseSendAck(const MockNetwork& net) {
    REQUIRE(net.sends.size() >= 2u);
    REQUIRE(net.sends[1].size() >= sizeof(fl::MsgConnectAck));
    fl::MsgConnectAck ack{};
    std::memcpy(&ack, net.sends[1].data(), sizeof(ack));
    return ack;
}

// Parse a MsgConnectRefusal from the first (and only) send on a rejected connection.
inline fl::MsgConnectRefusal parseSendRefusal(const MockNetwork& net) {
    REQUIRE(net.sends.size() == 1u);
    REQUIRE(net.sends[0].size() == sizeof(fl::MsgConnectRefusal));
    fl::MsgConnectRefusal ref{};
    std::memcpy(&ref, net.sends[0].data(), sizeof(ref));
    return ref;
}

// Drive the #853 connect handshake for a PILOT peer: onConnect (which now only sends MsgHello) followed
// by the client's MsgConnectRequest, which is what admits the peer (spawn + MsgConnectAck + MOTD +
// flight check-in). This reproduces the exact net.sends sequence the pre-#853 auto-spawning onConnect
// produced, so it is a drop-in replacement everywhere a test just needs a spawned pilot. If onConnect
// REJECTED the peer (ban / allowlist / rate-limit / per-IP cap / lockout), the only send is a
// MsgConnectRefusal and no request is injected — matching a rejected connection's old behavior exactly.
inline void connectPilotPeer(fl::WorldBroadcaster& b, MockNetwork& net, uint32_t peerId, const char* entityType = "") {
    const std::size_t before = net.sends.size();
    b.onConnect(peerId);
    const bool rejected = net.sends.size() > before && !net.sends.back().empty() &&
                          net.sends.back()[0] == static_cast<uint8_t>(fl::MsgId::ConnectRefusal);
    if (rejected)
        return;
    fl::MsgConnectRequest req{};
    req.requestedRole = static_cast<uint8_t>(fl::PeerRole::Pilot);
    std::snprintf(req.requestedEntityType, sizeof(req.requestedEntityType), "%s", entityType);
    b.onReceive(peerId, &req, sizeof(req));
}

// Drive the #853 handshake for an OBSERVER peer (#857): admitted with a role but NO entity/controller.
// It still receives snapshots (interest centered on its camera eye, #858), so this is the fixture for
// observer interest tests. Returns without injecting a request if onConnect rejected the peer.
inline void connectObserverPeer(fl::WorldBroadcaster& b, MockNetwork& net, uint32_t peerId) {
    const std::size_t before = net.sends.size();
    b.onConnect(peerId);
    const bool rejected = net.sends.size() > before && !net.sends.back().empty() &&
                          net.sends.back()[0] == static_cast<uint8_t>(fl::MsgId::ConnectRefusal);
    if (rejected)
        return;
    fl::MsgConnectRequest req{};
    req.requestedRole = static_cast<uint8_t>(fl::PeerRole::Observer);
    b.onReceive(peerId, &req, sizeof(req));
}

// Build a MsgClientInput carrying only a camera eye world-position (#858) — the observer's viewpoint.
inline fl::MsgClientInput cameraInput(uint32_t seq, double ex, double ey, double ez) {
    fl::MsgClientInput inp{};
    inp.seqNum = seq;
    inp.cameraEye[0] = ex;
    inp.cameraEye[1] = ey;
    inp.cameraEye[2] = ez;
    return inp;
}

// ---------------------------------------------------------------------------
// Interest-management snapshot helpers
// ---------------------------------------------------------------------------

// Return all MsgWorldSnapshot packets sent to a specific peer, in order.
// After #346, snapshots are per-peer unicast (in perPeerSends) rather than broadcast.
inline std::vector<std::vector<uint8_t>> snapshotsFor(const MockNetwork& net, uint32_t peerId) {
    std::vector<std::vector<uint8_t>> result;
    for (const auto& [pid, pkt] : net.perPeerSends)
        if (pid == peerId && !pkt.empty() && pkt[0] == static_cast<uint8_t>(fl::MsgId::WorldSnapshot))
            result.push_back(pkt);
    return result;
}

// Clear all per-peer unicast sends (equivalent to old clearSnapshots(net) for snapshot tests).
inline void clearSnapshots(MockNetwork& net) {
    net.perPeerSends.clear();
}

// Parse the MsgWorldSnapshotHeader from a raw snapshot packet.
// A pilot spawns PARKED and therefore gear-down (#639), so its snapshot carries one
// SnapshotArticulation record (#843): 4 B tag/len + 4 B entityIdx + 2 B mask + 1 value byte.
inline constexpr std::size_t kParkedGearArtTlvBytes = 11u;

inline fl::MsgWorldSnapshotHeader parseSnapshotHeader(const std::vector<uint8_t>& pkt) {
    REQUIRE(pkt.size() >= sizeof(fl::MsgWorldSnapshotHeader));
    fl::MsgWorldSnapshotHeader hdr{};
    std::memcpy(&hdr, pkt.data(), sizeof(hdr));
    return hdr;
}

// Decoded entity record. Field names mirror the old MsgEntityEntry so assertions read the same.
// `ori` is x,y,z,w (= quat); `flags` bit 0 = playerOwned. `isFull`/`genPresent` report the wire
// classification (full record carries typeIndex + gen; delta omits them).
struct DecodedEntity {
    uint32_t entityIdx{0};
    uint32_t entityGen{0};
    uint32_t typeIndex{0};
    double pos[3]{};
    float vel[3]{};
    float ori[4]{0, 0, 0, 1};
    uint8_t damageLevel{0};
    uint8_t engineFailFlags{0};
    uint8_t throttle{0};
    uint8_t fuelPct{0};
    uint8_t abEngaged{0};
    uint8_t flags{0};
    float omega[3]{};
    bool isFull{false};
    bool genPresent{false};
    bool hasLoadout{false};       // the own-record loadout block was on the wire (#625)
    uint8_t selectedStation{255}; //
    uint16_t stationRounds{0};    //
    uint8_t weaponFlags{0};       // bit 0 = seeker LOCK cue (#628)
};

// Decode every quantized record in a snapshot packet (full + delta). Stateless: typeIndex/gen are
// only populated for records that carry them on the wire (full / genPresent), which is all the
// assertions need. Use decodeEntitiesCached for cross-snapshot delta decoding.
inline std::vector<DecodedEntity> decodeEntities(const std::vector<uint8_t>& pkt) {
    fl::MsgWorldSnapshotHeader hdr = parseSnapshotHeader(pkt);
    std::vector<DecodedEntity> out;
    // Body layout (#725): [origin table: originCount x double[3]][record stream].
    const std::size_t originBytes = static_cast<std::size_t>(hdr.originCount) * 3u * sizeof(double);
    const std::size_t recordOffset = sizeof(hdr) + originBytes;
    std::vector<double> originTable(static_cast<std::size_t>(hdr.originCount) * 3u);
    if (hdr.originCount > 0 && recordOffset <= pkt.size())
        std::memcpy(originTable.data(), pkt.data() + sizeof(hdr), originBytes);
    const std::size_t bodyAvail = (pkt.size() > recordOffset) ? (pkt.size() - recordOffset) : 0u;
    const std::size_t bodyBytes = std::min<std::size_t>(hdr.bitstreamBytes, bodyAvail);
    fl::BitReader r(pkt.data() + recordOffset, bodyBytes);
    for (uint16_t i = 0; i < hdr.recordCount; ++i) {
        fl::QuantEntity qe;
        bool gp = false;
        if (!fl::decodeStandaloneRecord(r, qe, originTable.data(), hdr.originCount, gp))
            break;
        DecodedEntity d;
        d.entityIdx = qe.idx;
        d.entityGen = qe.gen;
        d.typeIndex = qe.typeIndex;
        d.pos[0] = qe.pos[0];
        d.pos[1] = qe.pos[1];
        d.pos[2] = qe.pos[2];
        d.vel[0] = qe.vel[0];
        d.vel[1] = qe.vel[1];
        d.vel[2] = qe.vel[2];
        d.ori[0] = qe.quat[0];
        d.ori[1] = qe.quat[1];
        d.ori[2] = qe.quat[2];
        d.ori[3] = qe.quat[3];
        d.damageLevel = qe.damageLevel;
        d.engineFailFlags = qe.engineFailFlags;
        d.throttle = qe.throttle;
        d.fuelPct = qe.fuelPct;
        d.abEngaged = qe.abEngaged ? 1u : 0u;
        d.flags = qe.playerOwned ? 1u : 0u;
        d.omega[0] = qe.omega[0];
        d.omega[1] = qe.omega[1];
        d.omega[2] = qe.omega[2];
        d.isFull = qe.isFull;
        d.genPresent = gp;
        d.hasLoadout = qe.hasOmega; // the loadout block rides the same own-record bit (#625)
        d.selectedStation = qe.selectedStation;
        d.stationRounds = qe.stationRounds;
        d.weaponFlags = qe.weaponFlags;
        out.push_back(d);
    }
    return out;
}

// Count of full vs delta records in a snapshot (replaces the old fullEntityCount/updateCount split).
inline uint16_t fullRecordCount(const std::vector<uint8_t>& pkt) {
    uint16_t n = 0;
    for (const auto& e : decodeEntities(pkt))
        if (e.isFull)
            ++n;
    return n;
}
inline uint16_t deltaRecordCount(const std::vector<uint8_t>& pkt) {
    uint16_t n = 0;
    for (const auto& e : decodeEntities(pkt))
        if (!e.isFull)
            ++n;
    return n;
}

// Total visible entities for this peer = recordCount.
inline uint16_t totalEntityCount(const fl::MsgWorldSnapshotHeader& hdr) {
    return hdr.recordCount;
}

// Decode the full records of a snapshot packet (back-compat shim for assertions that expected only
// the full-entry list; now returns every full record decoded from the bitstream).
inline std::vector<DecodedEntity> parseFullEntries(const std::vector<uint8_t>& pkt) {
    std::vector<DecodedEntity> full;
    for (const auto& e : decodeEntities(pkt))
        if (e.isFull)
            full.push_back(e);
    return full;
}

// Simulate a client acknowledging snapshot `tick` by feeding an MsgClientInput that echoes it (the
// realistic ack path for client-acked delta baselines: an entity stays full until the peer confirms it
// decoded the tick its full streak started on). seqNum must strictly increase per peer across calls (the
// broadcaster's staleness guard discards non-newer inputs). The default all-ones selective-ack mask
// (#566) reports "every in-window tick decoded", matching the pre-#566 high-water semantics; tests
// exercising a specific drop pass an explicit mask with the relevant bit cleared.
inline void ackTick(fl::WorldBroadcaster& b, uint32_t peerId, uint64_t tick, uint32_t seqNum,
                    uint32_t ackMask = 0xFFFFFFFFu) {
    fl::MsgClientInput inp{};
    inp.seqNum = seqNum;
    inp.tickIndex = tick;
    inp.ackMask = ackMask;
    b.onReceive(peerId, &inp, sizeof(inp));
}

// Admit a pilot whose MsgConnectRequest carries the given pack manifest (id + optional version).
inline void connectPilotWithPackVersions(fl::WorldBroadcaster& b, uint32_t peerId,
                                         const std::vector<std::pair<std::string, std::string>>& packs) {
    b.onConnect(peerId);
    fl::MsgConnectRequest req{};
    req.requestedRole = static_cast<uint8_t>(fl::PeerRole::Pilot);
    req.packCount = static_cast<uint16_t>(packs.size());
    std::vector<uint8_t> buf;
    fl::appendMsg(buf, req);
    for (const auto& [id, version] : packs) {
        fl::PackManifestEntry pe{};
        std::snprintf(pe.id, sizeof(pe.id), "%s", id.c_str());
        std::snprintf(pe.version, sizeof(pe.version), "%s", version.c_str());
        fl::appendMsg(buf, pe);
    }
    b.onReceive(peerId, buf.data(), buf.size());
}

// Admit a pilot whose MsgConnectRequest carries the given pack-id manifest (version unset).
inline void connectPilotWithPacks(fl::WorldBroadcaster& b, uint32_t peerId, const std::vector<std::string>& packIds) {
    std::vector<std::pair<std::string, std::string>> packs;
    for (const std::string& id : packIds)
        packs.emplace_back(id, std::string{});
    connectPilotWithPackVersions(b, peerId, packs);
}

// Find the first sent packet whose leading msgId matches; returns it decoded, or fails the test.
inline fl::MsgConnectRefusal findSentRefusal(const MockNetwork& net) {
    for (const auto& pkt : net.sends)
        if (!pkt.empty() && pkt[0] == static_cast<uint8_t>(fl::MsgId::ConnectRefusal)) {
            fl::MsgConnectRefusal ref{};
            std::memcpy(&ref, pkt.data(), sizeof(ref));
            return ref;
        }
    FAIL_CHECK("no MsgConnectRefusal was sent"); // non-terminating: the return below stays reachable (MSVC C4702)
    return {};
}

inline bool anySentIs(const MockNetwork& net, fl::MsgId id) {
    for (const auto& pkt : net.sends)
        if (!pkt.empty() && pkt[0] == static_cast<uint8_t>(id))
            return true;
    return false;
}

// Drive the connect handshake for a pilot carrying a client GUID (#524) in the ConnectRequest TLV.
inline void connectPilotWithGuid(fl::WorldBroadcaster& b, uint32_t peerId, const char* guid) {
    b.onConnect(peerId);
    fl::MsgConnectRequest req{};
    req.requestedRole = static_cast<uint8_t>(fl::PeerRole::Pilot);
    std::vector<uint8_t> buf;
    fl::appendMsg(buf, req);
    fl::appendExtRaw(buf, static_cast<uint16_t>(fl::ExtTag::ConnectIdentity), guid,
                     static_cast<uint16_t>(std::strlen(guid)));
    b.onReceive(peerId, buf.data(), buf.size());
}

// The last Stats record unicast to `peerId`, if any.
inline std::optional<fl::CombatEventRecord> lastStatsFor(const MockNetwork& net, uint32_t peerId) {
    std::optional<fl::CombatEventRecord> out;
    for (const auto& [pid, pkt] : net.perPeerSends) {
        if (pid != peerId || pkt.empty() || pkt[0] != static_cast<uint8_t>(fl::MsgId::CombatEvent))
            continue;
        fl::MsgCombatEventHeader hdr;
        if (!fl::readMsg(pkt.data(), pkt.size(), hdr))
            continue;
        for (uint8_t i = 0; i < hdr.count; ++i) {
            fl::CombatEventRecord rec;
            if (fl::readRecordAt(pkt.data(), pkt.size(), sizeof(hdr) + std::size_t(i) * sizeof(rec), rec) &&
                rec.type == static_cast<uint8_t>(fl::CombatEventType::Stats))
                out = rec;
        }
    }
    return out;
}

// The live entity owned by `peerId` (the pilot's aircraft), or nullptr.
inline const fl::EntityState* slotPeerEntity(const fl::EntityManager& em, uint32_t peerId) {
    const fl::EntityState* found = nullptr;
    em.forEach([&](const fl::EntityState& s) {
        if (!s.dead && s.ownerId == peerId)
            found = &s;
    });
    return found;
}

// A minimal crewed bomber def for the replication tests: a Fly+Fire pilot on station 0 and a Fire
// tail-gunner aiming a turret that mounts station 1. Valid one-owner partition.
inline fl::EntityDef makeCrewBomberDef() {
    fl::EntityDef d;
    d.id = "test:crewbomber";
    d.name = "CrewBomber";
    d.category = fl::ObjectCategory::AirVehicle;
    d.maxHp = 300.f;
    fl::Hardpoint hp0;
    hp0.slot = 0;
    hp0.allowed = {"test:rkt"};
    hp0.defaultWeapon = "test:rkt";
    fl::Hardpoint hp1;
    hp1.slot = 1;
    hp1.allowed = {"test:rkt"};
    hp1.defaultWeapon = "test:rkt";
    d.hardpoints = {hp0, hp1};
    fl::TurretDef t;
    t.id = "tail";
    t.stations = {1};
    d.turrets = {t};
    fl::SeatDef pilot;
    pilot.role = "pilot";
    pilot.capabilities = fl::withCapability(fl::withCapability(fl::CrewCapabilityMask{0}, fl::CrewCapability::Fly),
                                            fl::CrewCapability::Fire);
    pilot.stations = {0};
    fl::SeatDef gunner;
    gunner.role = "tail-gunner";
    gunner.capabilities = fl::withCapability(fl::CrewCapabilityMask{0}, fl::CrewCapability::Fire);
    gunner.turret = "tail";
    d.crew = {pilot, gunner};
    return d;
}

inline fl::WeaponDef makeRktWeapon() {
    fl::WeaponDef w;
    w.id = "test:rkt";
    w.name = "Rkt";
    w.type = fl::WeaponType::Rocket;
    w.category = fl::WeaponCategory::AirToGround;
    w.performance.maxRangeM = 4000.f;
    w.performance.maxSpeedMps = 500.f;
    w.load.massKg = 20.f;
    w.load.rounds = 40;
    return w;
}

struct StillCtl : fl::IEntityController {
    fl::ControlInput sample(const fl::EntityState&, uint64_t, double, const fl::AiTickContext&) override {
        return fl::ControlInput{};
    }
};

inline fl::MsgSeatRequest joinReq(fl::EntityId host, uint8_t seat) {
    fl::MsgSeatRequest req{};
    req.seatIndex = seat;
    req.entityIdx = host.index;
    req.entityGen = host.generation;
    return req;
}

// Find the first MsgServerNotice in broadcasts from index `from`.
inline bool findNotice(const std::vector<std::vector<uint8_t>>& broadcasts, std::size_t from,
                       fl::MsgServerNotice& out) {
    for (std::size_t i = from; i < broadcasts.size(); ++i) {
        const auto& pkt = broadcasts[i];
        if (pkt.size() == sizeof(fl::MsgServerNotice) && pkt[0] == static_cast<uint8_t>(fl::MsgId::ServerNotice)) {
            std::memcpy(&out, pkt.data(), sizeof(out));
            return true;
        }
    }
    return false;
}

// Drive the connect handshake for a pilot carrying a specific callsign (#996).
inline void connectPilotWithCallsign(fl::WorldBroadcaster& b, uint32_t peerId, const char* callsign) {
    b.onConnect(peerId);
    fl::MsgConnectRequest req{};
    req.requestedRole = static_cast<uint8_t>(fl::PeerRole::Pilot);
    std::snprintf(req.callsign, sizeof(req.callsign), "%s", callsign);
    b.onReceive(peerId, &req, sizeof(req));
}

// Collect every PlayerRoster upsert/leave record delivered to `peerId` across all recorded sends.
inline std::vector<fl::PlayerRosterEntry> collectRosterFor(const MockNetwork& net, uint32_t peerId) {
    std::vector<fl::PlayerRosterEntry> out;
    for (const auto& [pid, pkt] : net.perPeerSends) {
        if (pid != peerId || pkt.empty() || pkt[0] != static_cast<uint8_t>(fl::MsgId::PlayerRoster))
            continue;
        fl::MsgPlayerRosterHeader hdr{};
        if (!fl::readMsg(pkt.data(), pkt.size(), hdr))
            continue;
        std::size_t off = sizeof(hdr);
        for (uint8_t i = 0; i < hdr.count; ++i) {
            fl::PlayerRosterEntry e{};
            if (!fl::readRecordAt(pkt.data(), pkt.size(), off, e))
                break;
            off += sizeof(e);
            e.callsign[sizeof(e.callsign) - 1] = '\0';
            out.push_back(e);
        }
    }
    return out;
}

// Every MsgConnectAck sent to this peer, in order.
inline std::vector<std::vector<uint8_t>> connectAcksFor(const MockNetwork& net, uint32_t peerId) {
    std::vector<std::vector<uint8_t>> out;
    for (const auto& [pid, pkt] : net.perPeerSends)
        if (pid == peerId && !pkt.empty() && pkt[0] == static_cast<uint8_t>(fl::MsgId::ConnectAck))
            out.push_back(pkt);
    return out;
}

inline bool ackSkipsTypes(const std::vector<uint8_t>& ack) {
    fl::MsgConnectAck hdr{};
    REQUIRE(fl::readMsg(ack.data(), ack.size(), hdr));
    const std::size_t off = sizeof(hdr) + static_cast<std::size_t>(hdr.typeCount) * sizeof(fl::MsgEntityTypeDef);
    if (off > ack.size())
        return false;
    uint16_t len = 0;
    return fl::findExt(ack.data() + off, ack.size() - off, static_cast<uint16_t>(fl::ExtTag::ConnectAckTypesUnchanged),
                       len) != nullptr;
}
