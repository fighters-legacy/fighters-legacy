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
#include "mock_log.h"
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
#include "vg_flight_model.h" // makeVgFlightModelToml — the #1195 swing-wing fixture
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

// Disable the graceful tick-overrun governor (#514) on a broadcaster under test.
//
// The governor reads WALL-CLOCK tick time (TickProfiler::lastTotalMs) and sheds snapshots when the
// tick exceeds its budget. That is exactly right in production and exactly wrong in a test that
// asserts "a snapshot was sent on tick N": on a loaded machine — a CI runner, or a developer running
// `ctest -j` with 24 sanitized processes in flight — an instrumented tick easily blows 16.6 ms, the
// governor correctly concludes the server is overloaded, and the snapshot the test is waiting for is
// legitimately never sent. The test then fails for a reason that has nothing to do with what it is
// testing (#787).
//
// So any test that asserts on snapshot PRESENCE or byte-identity must call this. Tests that are
// specifically exercising the governor obviously must not.
void disableOverrunGovernor(fl::WorldBroadcaster& b) {
    fl::TickGovernorParams gp;
    gp.enabled = false;
    b.setGovernorParams(gp);
}

// Read the SnapshotDespawn TLV (uint32[] of removed indices) from a snapshot packet.
inline std::vector<uint32_t> decodeDespawns(const std::vector<uint8_t>& pkt) {
    std::vector<uint32_t> ids;
    fl::MsgWorldSnapshotHeader hdr = parseSnapshotHeader(pkt);
    const std::size_t extOffset =
        sizeof(hdr) + static_cast<std::size_t>(hdr.originCount) * 3u * sizeof(double) + hdr.bitstreamBytes;
    if (pkt.size() <= extOffset)
        return ids;
    uint16_t valueLen{};
    const uint8_t* p = fl::findExt(pkt.data() + extOffset, pkt.size() - extOffset,
                                   static_cast<uint16_t>(fl::ExtTag::SnapshotDespawn), valueLen);
    if (!p)
        return ids;
    for (uint16_t i = 0; i + 4u <= valueLen; i += 4u) {
        uint32_t v{};
        std::memcpy(&v, p + i, 4u);
        ids.push_back(v);
    }
    return ids;
}

// Build CongestionParams with a fast (every-tick) eval cadence for deterministic tests.
inline fl::CongestionParams testCongestion(bool enabled = true) {
    fl::CongestionParams p = fl::makeCongestionParams(enabled, /*minSendHz=*/10.f, /*lossThreshold=*/0.02f,
                                                      /*budgetFloorBytes=*/400u);
    p.evalIntervalTicks = 1u; // step AIMD every tick so back-off/recovery is observable over few ticks
    return p;
}

inline fl::PeerLinkStats lossLink(float loss) {
    fl::PeerLinkStats s;
    s.packetLoss = loss;
    return s;
}

// Sends one MsgClientInput carrying the articulation commands.
void sendArticulationInput(fl::WorldBroadcaster& wb, uint32_t peerId, uint32_t seq, uint8_t artButtons, uint8_t flaps) {
    fl::MsgClientInput in{};
    in.seqNum = seq;
    in.throttle = 0.5f;
    in.artButtons = artButtons;
    in.flaps = flaps;
    std::vector<uint8_t> buf;
    fl::appendMsg(buf, in);
    wb.onReceive(peerId, buf.data(), buf.size());
}

// Reads the TLV extension block of a peer's most recent snapshot.
struct SnapshotExt {
    const uint8_t* data{nullptr};
    std::size_t size{0};
    std::vector<uint8_t> pkt;
};

SnapshotExt lastSnapshotExt(const MockNetwork& net, uint32_t peerId) {
    SnapshotExt out;
    auto snaps = snapshotsFor(net, peerId);
    if (snaps.empty())
        return out;
    out.pkt = snaps.back();
    const auto hdr = parseSnapshotHeader(out.pkt);
    const std::size_t off = sizeof(fl::MsgWorldSnapshotHeader) +
                            static_cast<std::size_t>(hdr.originCount) * 3u * sizeof(double) + hdr.bitstreamBytes;
    if (out.pkt.size() < off)
        return out;
    out.data = out.pkt.data() + off;
    out.size = out.pkt.size() - off;
    return out;
}

// Stub controller: drives the entity at a fixed throttle with no peer connection. Records how many
// times it is sampled so the test can confirm onTick steps non-peer entities.
struct ConstantController : fl::IEntityController {
    float throttle{1.0f};
    int sampleCount{0};
    fl::ControlInput sample(const fl::EntityState&, uint64_t, double, const fl::AiTickContext&) override {
        ++sampleCount;
        fl::ControlInput ctrl{};
        ctrl.throttle = throttle;
        return ctrl;
    }
};

std::map<uint32_t, std::vector<std::vector<uint8_t>>> runSnapshotScenario(fl::JobSystem* jobs, uint64_t killAtTick,
                                                                          bool compress = false) {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    disableOverrunGovernor(broadcaster); // byte-identity must not depend on how busy the host is
    if (jobs)
        broadcaster.setJobSystem(*jobs);
    broadcaster.setSnapshotCompression(compress);

    // Distinct peer spawn positions so each peer gets a different frameOrigin, own-entity record, and
    // interest set — making the per-peer byte comparison non-trivial. All within the 200 km default
    // draw distance so peers see overlapping (but not identical) entity sets.
    broadcaster.setSpawnPoints(
        {{0.0, 1000.0, 0.0}, {40000.0, 1000.0, 0.0}, {0.0, 1000.0, 40000.0}, {40000.0, 1000.0, 40000.0}});
    const std::vector<uint32_t> peerIds{0u, 1u, 2u, 3u};
    for (uint32_t pid : peerIds)
        connectPilotPeer(broadcaster, net, pid);

    // 16 shared entities spread across the peers' interest region.
    std::vector<fl::EntityId> ids;
    for (int i = 0; i < 16; ++i) {
        fl::EntityTransform t{};
        t.pos[0] = i * 2000.0;
        t.pos[1] = 1000.0;
        t.pos[2] = (i % 4) * 5000.0;
        fl::EntityId id = em.spawn("builtin:debug-entity", t);
        REQUIRE(id.valid());
        auto controller = std::make_unique<ConstantController>();
        controller->throttle = 1.0f;
        broadcaster.registerController(id, std::move(controller));
        ids.push_back(id);
    }

    for (uint64_t tick = 1; tick <= 120; ++tick) {
        if (killAtTick > 0 && tick == killAtTick)
            em.kill(ids[7]); // remove a shared entity mid-run -> despawn detection on the next tick
        broadcaster.onTick(1.0 / 60.0, tick);
    }

    std::map<uint32_t, std::vector<std::vector<uint8_t>>> out;
    for (uint32_t pid : peerIds)
        out[pid] = snapshotsFor(net, pid);
    return out;
}

struct InterestShedResult {
    std::map<uint32_t, std::vector<std::vector<uint8_t>>> snaps;
    float finalInterestScale{1.f};
};

// A fake clock that advances a fixed delta on every now() call. Because onTick reads the clock a
// fixed number of times per tick (per-pass phase timing, worker-count-independent), each tick measures
// a constant, over-budget wall span — so the governor degrades deterministically and identically
// across worker counts. Sim-thread only (no synchronization); mutable m_now so the const now() can step.
class AutoAdvanceClock final : public fl::IClock {
  public:
    explicit AutoAdvanceClock(std::chrono::steady_clock::duration step) : m_step(step) {}
    std::chrono::steady_clock::time_point now() const override {
        m_now += m_step;
        return m_now;
    }

  private:
    mutable std::chrono::steady_clock::time_point m_now{};
    std::chrono::steady_clock::duration m_step;
};

// Does a snapshot carry the #576 server-throttle tag?
[[nodiscard]] inline bool hasServerThrottleTlv(const std::vector<uint8_t>& snap) {
    fl::MsgWorldSnapshotHeader hdr{};
    if (!fl::readMsg(snap.data(), snap.size(), hdr))
        return false;
    const std::size_t extOff =
        sizeof(hdr) + static_cast<std::size_t>(hdr.originCount) * 3u * sizeof(double) + hdr.bitstreamBytes;
    if (extOff >= snap.size())
        return false;
    std::uint16_t len = 0;
    return fl::findExt(snap.data() + extOff, snap.size() - extOff,
                       static_cast<uint16_t>(fl::ExtTag::SnapshotServerThrottle), len) != nullptr;
}

inline InterestShedResult runInterestShedScenario(fl::JobSystem* jobs) {
    NullLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    AutoAdvanceClock clock(std::chrono::milliseconds(3));
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setClock(clock);
    if (jobs)
        broadcaster.setJobSystem(*jobs);
    fl::TickGovernorParams gp = fl::makeTickGovernorParams(true, 0.90f, 0.60f, 15.0f, 4u, 400u, 0.5f);
    gp.evalIntervalTicks = 1u;
    gp.ewmaAlpha = 1.0f;
    broadcaster.setGovernorParams(gp);

    // Two peers at distinct spawn points so their (shrunk) interest sets differ.
    broadcaster.setSpawnPoints({{0.0, 1000.0, 0.0}, {40'000.0, 1000.0, 0.0}});
    const std::vector<uint32_t> peerIds{0u, 1u};
    for (uint32_t pid : peerIds)
        connectPilotPeer(broadcaster, net, pid);

    // Static entities from 10 km out to 150 km: all inside the full 200 km radius, the far ones
    // outside the floor-scaled 100 km radius of at least one peer.
    for (int i = 0; i < 15; ++i) {
        fl::EntityTransform t{};
        t.pos[0] = 10'000.0 + i * 10'000.0;
        t.pos[1] = 1000.0;
        fl::EntityId id = em.spawn("builtin:debug-entity", t);
        REQUIRE(id.valid());
    }

    for (uint64_t tick = 1; tick <= 120; ++tick)
        broadcaster.onTick(1.0 / 60.0, tick);

    InterestShedResult res;
    res.finalInterestScale = broadcaster.getOverrunStatus().interestScale;
    for (uint32_t pid : peerIds)
        res.snaps[pid] = snapshotsFor(net, pid);
    return res;
}

// Build a MsgChat packet (header + NUL-terminated text).
std::vector<uint8_t> makeChatPkt(fl::ChatChannel ch, std::string_view text) {
    fl::MsgChatHeader hdr{};
    hdr.channel = static_cast<uint8_t>(ch);
    std::vector<uint8_t> pkt;
    fl::appendMsg(pkt, hdr);
    pkt.insert(pkt.end(), text.begin(), text.end());
    pkt.push_back('\0');
    return pkt;
}

int countChatEvents(const MockNetwork& net) {
    int n = 0;
    for (const auto& p : net.sends)
        if (!p.empty() && p[0] == static_cast<uint8_t>(fl::MsgId::ChatEvent))
            ++n;
    return n;
}

// Return the decoded text of the last MsgChatEvent sent to `peerId`, or empty if none.
std::string lastChatEventText(const MockNetwork& net, uint32_t peerId) {
    for (auto it = net.perPeerSends.rbegin(); it != net.perPeerSends.rend(); ++it) {
        const auto& [pid, pkt] = *it;
        if (pid == peerId && !pkt.empty() && pkt[0] == static_cast<uint8_t>(fl::MsgId::ChatEvent) &&
            pkt.size() >= sizeof(fl::MsgChatEventHeader)) {
            std::string t(reinterpret_cast<const char*>(pkt.data()) + sizeof(fl::MsgChatEventHeader),
                          pkt.size() - sizeof(fl::MsgChatEventHeader));
            if (const auto z = t.find('\0'); z != std::string::npos)
                t.resize(z);
            return t;
        }
    }
    return {};
}

// Find a packet by message id rather than by position. The connect handshake has grown a message
// on nearly every epic (PlayerRoster #996, VoiceNetDef #532, ...), and every index-based assertion
// broke each time while testing nothing about ordering.
inline const std::vector<uint8_t>* findSend(const MockNetwork& net, fl::MsgId id) {
    for (const auto& pkt : net.sends)
        if (!pkt.empty() && pkt[0] == static_cast<uint8_t>(id))
            return &pkt;
    return nullptr;
}

inline std::string parseMotdText(const std::vector<uint8_t>& pkt) {
    if (pkt.size() < sizeof(fl::MsgMotdHeader) + 1u || pkt[0] != static_cast<uint8_t>(fl::MsgId::Motd))
        return {};
    // exclude the 4-byte MsgMotdHeader and the trailing NUL.
    return std::string(reinterpret_cast<const char*>(pkt.data() + sizeof(fl::MsgMotdHeader)),
                       pkt.size() - sizeof(fl::MsgMotdHeader) - 1u);
}

inline fl::MsgRadioCommand makeRadioCmd(const char* text) {
    fl::MsgRadioCommand cmd{};
    std::snprintf(cmd.command, sizeof(cmd.command), "%s", text);
    return cmd;
}

// Find the most recent RadioTransmission (#703) unicast to `peerId` in the tracked per-peer sends.
inline std::optional<fl::MsgRadioTransmission> lastRadioTo(const MockNetwork& net, uint32_t peerId) {
    std::optional<fl::MsgRadioTransmission> out;
    for (const auto& [pid, pkt] : net.perPeerSends) {
        if (pid != peerId || pkt.size() < sizeof(fl::MsgRadioTransmission))
            continue;
        if (pkt[0] != static_cast<uint8_t>(fl::MsgId::RadioTransmission))
            continue;
        fl::MsgRadioTransmission rt{};
        std::memcpy(&rt, pkt.data(), sizeof(rt));
        out = rt;
    }
    return out;
}

// All MsgCombatEvent packets broadcast so far, decoded into records.
std::vector<fl::CombatEventRecord> combatBroadcasts(const MockNetwork& net) {
    std::vector<fl::CombatEventRecord> out;
    for (const auto& pkt : net.broadcasts) {
        if (pkt.empty() || pkt[0] != static_cast<uint8_t>(fl::MsgId::CombatEvent))
            continue;
        fl::MsgCombatEventHeader hdr;
        REQUIRE(fl::readMsg(pkt.data(), pkt.size(), hdr));
        for (uint8_t i = 0; i < hdr.count; ++i) {
            fl::CombatEventRecord rec;
            REQUIRE(fl::readRecordAt(pkt.data(), pkt.size(), sizeof(hdr) + std::size_t(i) * sizeof(rec), rec));
            out.push_back(rec);
        }
    }
    return out;
}

// One 20 ms voice frame on `netId` with an opaque payload the server never decodes.
inline std::vector<uint8_t> makeVoiceFrame(uint8_t netId, uint16_t seq, std::size_t payloadBytes = 40) {
    fl::MsgVoiceFrameHeader hdr{};
    hdr.netId = netId;
    hdr.seq = seq;
    hdr.payloadBytes = static_cast<uint16_t>(payloadBytes);
    std::vector<uint8_t> pkt;
    fl::appendMsg(pkt, hdr);
    pkt.insert(pkt.end(), payloadBytes, uint8_t{0xA5});
    return pkt;
}
