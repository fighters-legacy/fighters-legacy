// SPDX-License-Identifier: GPL-3.0-or-later
#include "IClock.h"
#include "ILogger.h"
#include "INetwork.h"
#include "entity/DamageDef.h"
#include "entity/EntityDef.h"
#include "entity/EntityManager.h"
#include "entity/EntityTypeRegistry.h"
#include "entity/IEntityController.h"
#include "flight/CentralGravityField.h"
#include "job/JobSystem.h"
#include "net/BitStream.h"
#include "net/GameProtocol.h"
#include "net/SnapshotCodec.h"
#include "net/WireCodec.h"
#include "net/WorldBroadcaster.h"
#include "render/RenderSnapshot.h"
#include "weather/WeatherController.h"

#include "mock_network.h"

#include <algorithm>
#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

using namespace fl;

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

struct MockLogger : ILogger {
    void log(LogLevel, const char*, int, const char*) override {}
    void setMinLevel(LogLevel) override {}
    void flush() override {}
};

// Records broadcasts/sends/disconnects + resolves configurable peer addresses (see mock_network.h).
using MockNetwork = TrackingNetwork;

static fl::EntityDef makeDebugDef(const char* id = "builtin:debug-entity") {
    fl::EntityDef def;
    def.id = id;
    def.name = "Debug";
    def.category = fl::ObjectCategory::AirVehicle;
    def.maxHp = 100.0f;
    return def;
}

// Validate that the first send is a well-formed MsgHello with the current protocol version.
static fl::MsgHello parseSendHello(const MockNetwork& net) {
    REQUIRE(net.sends.size() >= 1u);
    REQUIRE(net.sends[0].size() == sizeof(fl::MsgHello));
    fl::MsgHello hello{};
    std::memcpy(&hello, net.sends[0].data(), sizeof(hello));
    return hello;
}

// Parse the MsgConnectAck from the second send packet (sends[1], after MsgHello).
static fl::MsgConnectAck parseSendAck(const MockNetwork& net) {
    REQUIRE(net.sends.size() >= 2u);
    REQUIRE(net.sends[1].size() >= sizeof(fl::MsgConnectAck));
    fl::MsgConnectAck ack{};
    std::memcpy(&ack, net.sends[1].data(), sizeof(ack));
    return ack;
}

// Parse a MsgConnectRefusal from the first (and only) send on a rejected connection.
static fl::MsgConnectRefusal parseSendRefusal(const MockNetwork& net) {
    REQUIRE(net.sends.size() == 1u);
    REQUIRE(net.sends[0].size() == sizeof(fl::MsgConnectRefusal));
    fl::MsgConnectRefusal ref{};
    std::memcpy(&ref, net.sends[0].data(), sizeof(ref));
    return ref;
}

// ---------------------------------------------------------------------------
// Interest-management snapshot helpers
// ---------------------------------------------------------------------------

// Return all MsgWorldSnapshot packets sent to a specific peer, in order.
// After #346, snapshots are per-peer unicast (in perPeerSends) rather than broadcast.
static std::vector<std::vector<uint8_t>> snapshotsFor(const MockNetwork& net, uint32_t peerId) {
    std::vector<std::vector<uint8_t>> result;
    for (const auto& [pid, pkt] : net.perPeerSends)
        if (pid == peerId && !pkt.empty() && pkt[0] == static_cast<uint8_t>(fl::MsgId::WorldSnapshot))
            result.push_back(pkt);
    return result;
}

// Clear all per-peer unicast sends (equivalent to old clearSnapshots(net) for snapshot tests).
static void clearSnapshots(MockNetwork& net) {
    net.perPeerSends.clear();
}

// Parse the MsgWorldSnapshotHeader from a raw snapshot packet.
static fl::MsgWorldSnapshotHeader parseSnapshotHeader(const std::vector<uint8_t>& pkt) {
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
};

// Decode every quantized record in a snapshot packet (full + delta). Stateless: typeIndex/gen are
// only populated for records that carry them on the wire (full / genPresent), which is all the
// assertions need. Use decodeEntitiesCached for cross-snapshot delta decoding.
static std::vector<DecodedEntity> decodeEntities(const std::vector<uint8_t>& pkt) {
    fl::MsgWorldSnapshotHeader hdr = parseSnapshotHeader(pkt);
    std::vector<DecodedEntity> out;
    const std::size_t bodyAvail = pkt.size() - sizeof(hdr);
    const std::size_t bodyBytes = std::min<std::size_t>(hdr.bitstreamBytes, bodyAvail);
    fl::BitReader r(pkt.data() + sizeof(hdr), bodyBytes);
    uint32_t prevIdx = 0;
    for (uint16_t i = 0; i < hdr.recordCount; ++i) {
        fl::QuantEntity qe;
        bool gp = false;
        if (!fl::decodeRecord(r, qe, prevIdx, hdr.frameOrigin, gp))
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
        out.push_back(d);
    }
    return out;
}

// Count of full vs delta records in a snapshot (replaces the old fullEntityCount/updateCount split).
static uint16_t fullRecordCount(const std::vector<uint8_t>& pkt) {
    uint16_t n = 0;
    for (const auto& e : decodeEntities(pkt))
        if (e.isFull)
            ++n;
    return n;
}
static uint16_t deltaRecordCount(const std::vector<uint8_t>& pkt) {
    uint16_t n = 0;
    for (const auto& e : decodeEntities(pkt))
        if (!e.isFull)
            ++n;
    return n;
}

// Total visible entities for this peer = recordCount.
static uint16_t totalEntityCount(const fl::MsgWorldSnapshotHeader& hdr) {
    return hdr.recordCount;
}

// Decode the full records of a snapshot packet (back-compat shim for assertions that expected only
// the full-entry list; now returns every full record decoded from the bitstream).
static std::vector<DecodedEntity> parseFullEntries(const std::vector<uint8_t>& pkt) {
    std::vector<DecodedEntity> full;
    for (const auto& e : decodeEntities(pkt))
        if (e.isFull)
            full.push_back(e);
    return full;
}

// Simulate a client acknowledging snapshot `tick` by feeding an MsgClientInput that echoes it (the
// realistic ack path for client-acked delta baselines: an entity stays full until the peer acks the
// tick its full streak started on). seqNum must strictly increase per peer across calls (the
// broadcaster's staleness guard discards non-newer inputs).
static void ackTick(fl::WorldBroadcaster& b, uint32_t peerId, uint64_t tick, uint32_t seqNum) {
    fl::MsgClientInput inp{};
    inp.seqNum = seqNum;
    inp.tickIndex = tick;
    b.onReceive(peerId, &inp, sizeof(inp));
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_CASE("WorldBroadcaster: onTick broadcasts WorldSnapshot for N entities", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);

    registry.registerType(makeDebugDef());

    // Spawn 3 entities before GameLoop starts (no sim thread yet).
    for (int i = 0; i < 3; ++i) {
        fl::EntityTransform t{};
        t.pos[0] = static_cast<double>(i * 10);
        t.pos[1] = 500.0;
        em.spawn("builtin:debug-entity", t);
    }

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.onConnect(0u); // peer required for per-peer unicast snapshots

    // Drive one tick manually.
    broadcaster.onTick(1.0 / 60.0, 1u);

    // All entities are new to the peer (and unacked), so they appear as full entries.
    auto snaps = snapshotsFor(net, 0);
    REQUIRE(snaps.size() == 1u);
    const auto& pkt = snaps[0];
    REQUIRE(pkt.size() >= sizeof(fl::MsgWorldSnapshotHeader));

    auto hdr = parseSnapshotHeader(pkt);
    CHECK(hdr.msgId == static_cast<uint8_t>(fl::MsgId::WorldSnapshot));
    CHECK(totalEntityCount(hdr) == 4u); // 3 pre-spawned + 1 peer entity
    CHECK(hdr.tickIndex == 1u);

    // Verify one of the full entries has pos[1] ~= 500 (quantized relative to the frame origin).
    const auto fullEntries = parseFullEntries(pkt);
    REQUIRE(!fullEntries.empty());
    CHECK(fullEntries[0].pos[1] == Catch::Approx(500.0).margin(fl::kPosStepM));
}

// Stub controller: drives the entity at a fixed throttle with no peer connection. Records how many
// times it is sampled so the test can confirm onTick steps non-peer entities.
struct ConstantController : fl::IEntityController {
    float throttle{1.0f};
    int sampleCount{0};
    fl::ControlInput sample(const fl::EntityState&, uint64_t, double, const fl::SpatialIndex*) override {
        ++sampleCount;
        fl::ControlInput ctrl{};
        ctrl.throttle = throttle;
        return ctrl;
    }
};

TEST_CASE("WorldBroadcaster: registerController steps a non-peer entity and serializes it", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::EntityTransform t{};
    t.pos[1] = 1000.0;
    fl::EntityId id = em.spawn("builtin:debug-entity", t);
    REQUIRE(id.valid());

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    // Connect a peer so snapshots are sent (per-peer unicast model requires at least one peer).
    broadcaster.onConnect(0u);

    // Register an AI/scripted controller for the pre-spawned entity.
    auto controller = std::make_unique<ConstantController>();
    ConstantController* ctrlPtr = controller.get();
    broadcaster.registerController(id, std::move(controller));

    for (uint64_t tick = 1; tick <= 120; ++tick)
        broadcaster.onTick(1.0 / 60.0, tick);

    // The controller was sampled once per tick — proof the non-peer entity is stepped.
    CHECK(ctrlPtr->sampleCount == 120);

    // The entity moved under its own controller (full-throttle builtin model accelerates forward).
    const fl::EntityState* st = em.get(id);
    REQUIRE(st != nullptr);
    const bool moved = st->transform.pos[0] != 0.0 || st->transform.pos[2] != 0.0 || st->transform.pos[1] != 1000.0;
    CHECK(moved);

    // It serializes into the peer's snapshot with live throttle telemetry.
    auto snaps = snapshotsFor(net, 0);
    REQUIRE(!snaps.empty());
    const auto& pkt = snaps.back();
    auto hdr = parseSnapshotHeader(pkt);
    REQUIRE(totalEntityCount(hdr) >= 1u);
    // AI entity appears in full entries (the peer never acks, so records stay full every tick)
    bool foundAiEntity = false;
    for (const auto& e : parseFullEntries(pkt)) {
        if (e.entityIdx == id.index) {
            CHECK(e.throttle > 0u); // throttle spooled up toward the commanded 100%
            foundAiEntity = true;
        }
    }
    CHECK(foundAiEntity);
}

// Run a fixed multi-entity scenario and capture final entity state. When `jobs` is non-null the
// per-entity AI + integrate passes run data-parallel; otherwise they run inline. Used to prove the
// parallel path is serial-equivalent.
namespace {
struct FinalState {
    uint32_t idx;
    double pos[3];
    float quat[4];
};

std::vector<FinalState> runParallelScenario(fl::JobSystem* jobs) {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::WeatherController weather;
    weather.setPreset(fl::WeatherPreset::Storm); // turbulence on -> exercises the per-entity RNG

    fl::WorldBroadcaster broadcaster(em, registry, net, logger, &weather);
    if (jobs)
        broadcaster.setJobSystem(*jobs);
    broadcaster.onConnect(0u);

    std::vector<fl::EntityId> ids;
    for (int i = 0; i < 16; ++i) {
        fl::EntityTransform t{};
        t.pos[0] = i * 100.0;
        t.pos[1] = 1000.0;
        t.pos[2] = i * 50.0;
        fl::EntityId id = em.spawn("builtin:debug-entity", t);
        REQUIRE(id.valid());
        auto controller = std::make_unique<ConstantController>();
        controller->throttle = 1.0f;
        broadcaster.registerController(id, std::move(controller));
        ids.push_back(id);
    }

    for (uint64_t tick = 1; tick <= 120; ++tick)
        broadcaster.onTick(1.0 / 60.0, tick);

    std::vector<FinalState> out;
    for (fl::EntityId id : ids) {
        const fl::EntityState* st = em.get(id);
        REQUIRE(st != nullptr);
        FinalState fsv{};
        fsv.idx = id.index;
        for (int k = 0; k < 3; ++k)
            fsv.pos[k] = st->transform.pos[k];
        for (int k = 0; k < 4; ++k)
            fsv.quat[k] = st->transform.quat[k];
        out.push_back(fsv);
    }
    std::sort(out.begin(), out.end(), [](const FinalState& a, const FinalState& b) { return a.idx < b.idx; });
    return out;
}
} // namespace

TEST_CASE("WorldBroadcaster: parallel sim tick is serial-equivalent across worker counts", "[world_broadcaster]") {
    const std::vector<FinalState> baseline = runParallelScenario(nullptr); // inline / serial reference

    for (unsigned total : {1u, 2u, 8u}) {
        fl::JobSystem jobs(total);
        const std::vector<FinalState> got = runParallelScenario(&jobs);
        REQUIRE(got.size() == baseline.size());
        for (size_t i = 0; i < baseline.size(); ++i) {
            CHECK(got[i].idx == baseline[i].idx);
            // Bit-identical, not Approx: each entity integrates independently with no cross-entity
            // reduction, so parallelism must not change a single bit.
            for (int k = 0; k < 3; ++k)
                CHECK(got[i].pos[k] == baseline[i].pos[k]);
            for (int k = 0; k < 4; ++k)
                CHECK(got[i].quat[k] == baseline[i].quat[k]);
        }
    }
}

// Run a fixed multi-peer scenario and capture every per-peer WorldSnapshot packet, keyed by peerId.
// When `jobs` is non-null the per-peer snapshot assembly runs data-parallel via runPeerPass; else it
// runs inline. Used to prove the parallel snapshot build is byte-identical to the serial path. When
// `killAtTick > 0`, one shared entity is killed at that tick so the per-peer despawn-detection +
// SnapshotDespawn TLV path is exercised under parallelism.
namespace {
std::map<uint32_t, std::vector<std::vector<uint8_t>>> runSnapshotScenario(fl::JobSystem* jobs, uint64_t killAtTick) {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    if (jobs)
        broadcaster.setJobSystem(*jobs);

    // Distinct peer spawn positions so each peer gets a different frameOrigin, own-entity record, and
    // interest set — making the per-peer byte comparison non-trivial. All within the 200 km default
    // draw distance so peers see overlapping (but not identical) entity sets.
    broadcaster.setSpawnPoints(
        {{0.0, 1000.0, 0.0}, {40000.0, 1000.0, 0.0}, {0.0, 1000.0, 40000.0}, {40000.0, 1000.0, 40000.0}});
    const std::vector<uint32_t> peerIds{0u, 1u, 2u, 3u};
    for (uint32_t pid : peerIds)
        broadcaster.onConnect(pid);

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
} // namespace

TEST_CASE("WorldBroadcaster: parallel per-peer snapshot build is serial-equivalent across worker counts",
          "[world_broadcaster]") {
    for (uint64_t killAtTick : {uint64_t{0}, uint64_t{60}}) {
        const auto baseline = runSnapshotScenario(nullptr, killAtTick); // inline / serial reference

        for (unsigned total : {1u, 2u, 8u}) {
            fl::JobSystem jobs(total);
            const auto got = runSnapshotScenario(&jobs, killAtTick);
            REQUIRE(got.size() == baseline.size());
            for (const auto& [peerId, baseSnaps] : baseline) {
                auto it = got.find(peerId);
                REQUIRE(it != got.end());
                const auto& gotSnaps = it->second;
                INFO("workers=" << total << " peer=" << peerId << " killAtTick=" << killAtTick);
                // Each peer must receive the same number of snapshot packets...
                REQUIRE(gotSnaps.size() == baseSnaps.size());
                // ...and each packet must be byte-identical: the per-peer build performs no cross-peer
                // writes and no RNG, so parallelism must not change a single byte on the same binary.
                // Compare via a bool so Catch2 never stringifies the (large) byte vectors.
                for (size_t i = 0; i < baseSnaps.size(); ++i) {
                    const bool identical = (gotSnaps[i] == baseSnaps[i]);
                    INFO("snapshot packet index " << i);
                    CHECK(identical);
                }
            }
        }
    }
}

TEST_CASE("WorldBroadcaster: a congested peer is decimated while a healthy peer keeps sending", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.onConnect(0u); // healthy peer
    broadcaster.onConnect(1u); // congested peer

    // Report sustained high packet loss for peer 1 only; peer 0 (absent from the map) reads zeros.
    // Above the 0.02 loss threshold, the AIMD controller backs the throttle off, stretching peer 1's
    // snapshot send interval > 1 tick — so it is skipped on some ticks by the gather-time decimation
    // filter while peer 0 sends every tick.
    fl::PeerLinkStats bad{};
    bad.packetLoss = 0.5f;
    net.peerLinkStats[1u] = bad;

    for (uint64_t tick = 1; tick <= 120; ++tick)
        broadcaster.onTick(1.0 / 60.0, tick);

    const std::size_t healthySnaps = snapshotsFor(net, 0u).size();
    const std::size_t congestedSnaps = snapshotsFor(net, 1u).size();
    CHECK(congestedSnaps < healthySnaps); // decimation skipped the congested peer on some ticks
    CHECK(congestedSnaps > 0u);           // but it still receives snapshots at the floor rate
}

TEST_CASE("WorldBroadcaster: getTickBudget records per-phase timing after onTick", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::EntityTransform t{};
    t.pos[1] = 1000.0;
    fl::EntityId id = em.spawn("builtin:debug-entity", t);
    REQUIRE(id.valid());

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.onConnect(0u);
    broadcaster.registerController(id, std::make_unique<ConstantController>());

    // Before any tick, nothing is sampled.
    CHECK(broadcaster.getTickBudget().ticksSampled == 0u);

    for (uint64_t tick = 1; tick <= 5; ++tick)
        broadcaster.onTick(1.0 / 60.0, tick);

    const fl::TickBudget tb = broadcaster.getTickBudget();
    CHECK(tb.ticksSampled == 5u);
    CHECK(tb.ticksTotal == 5u);
    // Timing magnitudes are environment-dependent; assert only that every phase is finite and >= 0
    // and that the integrate/ai split wiring populated the accumulators without NaN/negatives.
    CHECK(std::isfinite(tb.total.mean));
    CHECK(tb.total.mean >= 0.0);
    CHECK(tb.tickHz >= 0.0);
    CHECK(std::isfinite(tb.tickHz));
    for (int i = 0; i < fl::kTickPhaseCount; ++i) {
        CHECK(std::isfinite(tb.phases[i].mean));
        CHECK(tb.phases[i].mean >= 0.0);
    }
    CHECK(tb.other.mean >= 0.0);
}

TEST_CASE("WorldBroadcaster: flight model resolver is consulted for a flightModelId, falls back on miss",
          "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);

    fl::EntityDef def = makeDebugDef();
    def.flightModelId = "models/x";
    registry.registerType(def);

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    std::string requestedId;
    broadcaster.setFlightModelResolver([&](const std::string& id) -> std::shared_ptr<const fl::FlightModelData> {
        requestedId = id;
        return nullptr; // unknown id -> WorldBroadcaster falls back to the builtin model
    });

    broadcaster.onConnect(0u);
    broadcaster.onTick(1.0 / 60.0, 1u);

    CHECK(requestedId == "models/x"); // resolver consulted with the entity's flightModelId
    CHECK(em.liveCount() == 1u);      // spawn still succeeded via the builtin fallback
}

TEST_CASE("WorldBroadcaster: flight model resolver is skipped when flightModelId is empty", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef()); // empty flightModelId

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    bool called = false;
    broadcaster.setFlightModelResolver([&](const std::string&) -> std::shared_ptr<const fl::FlightModelData> {
        called = true;
        return nullptr;
    });

    broadcaster.onConnect(0u);
    broadcaster.onTick(1.0 / 60.0, 1u);

    CHECK_FALSE(called); // empty id -> resolver never invoked
    CHECK(em.liveCount() == 1u);
}

TEST_CASE("WorldBroadcaster: onTick with no peers sends nothing", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    // No onConnect — per-peer loop iterates over an empty m_peerEntities; no snapshot is sent.
    broadcaster.onTick(1.0 / 60.0, 5u);

    CHECK(snapshotsFor(net, 0).empty());
}

TEST_CASE("WorldBroadcaster: onTick with connected peer and no extra entities sends peer-only snapshot",
          "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.onConnect(0u);
    broadcaster.onTick(1.0 / 60.0, 5u);

    REQUIRE(!snapshotsFor(net, 0).empty());
    const auto pkt = snapshotsFor(net, 0)[0];
    auto hdr = parseSnapshotHeader(pkt);
    CHECK(totalEntityCount(hdr) == 1u); // only the peer's own entity
    CHECK(hdr.tickIndex == 5u);

    // Packet = header + quantized bitstream (one full own-entity record) + SnapshotPeerCount TLV
    // (6 bytes). SnapshotPeerLatency TLV is absent (estimatedDelayTicks == 0; no heartbeat sent).
    CHECK(hdr.bitstreamBytes > 0u);
    const std::size_t extOffset = sizeof(fl::MsgWorldSnapshotHeader) + hdr.bitstreamBytes;
    REQUIRE(pkt.size() == extOffset + 6u);
    uint16_t pc{};
    CHECK(fl::readExtValue(pkt.data() + extOffset, 6u, static_cast<uint16_t>(fl::ExtTag::SnapshotPeerCount), pc));
    CHECK(pc == 1u); // 1 active peer
}

TEST_CASE("WorldBroadcaster: onConnect sends ConnectAck with registered types and spawns entity",
          "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);

    registry.registerType(makeDebugDef()); // required for peer-entity spawn
    registry.registerType(makeDebugDef("type:a"));
    registry.registerType(makeDebugDef("type:b"));

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.onConnect(0u);

    REQUIRE(net.sends.size() == 2u);
    CHECK(net.sendReliable);

    fl::MsgConnectAck ack = parseSendAck(net);
    CHECK(ack.msgId == static_cast<uint8_t>(fl::MsgId::ConnectAck));
    CHECK(ack.tickRateHz == 60);
    CHECK(ack.typeCount == 3u);
    CHECK(ack.assignedEntityGen != 0u); // generation != 0 means a valid entity was assigned

    // liveCount() is an atomic snapshot updated at the end of onTick() — drive one tick.
    broadcaster.onTick(1.0 / 60.0, 1u);
    CHECK(em.liveCount() == 1u);
}

TEST_CASE("WorldBroadcaster: peer entity spawns at terrain height plus 500 m AGL", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:5000";
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setGroundElevation(550.f);
    broadcaster.onConnect(0u);
    broadcaster.onTick(1.0 / 60.0, 1u);

    REQUIRE(!snapshotsFor(net, 0).empty());
    const auto pkt = snapshotsFor(net, 0).back();
    REQUIRE(parseSnapshotHeader(pkt).recordCount >= 1u);
    const auto _ents = decodeEntities(pkt);
    REQUIRE(!_ents.empty());
    const DecodedEntity& e = _ents[0];
    // 550 m terrain + 500 m AGL = 1050 m; allow ±10 m for one flight-integrator tick
    CHECK(e.pos[1] >= 1040.0);
    CHECK(e.pos[1] <= 1060.0);
}

TEST_CASE("WorldBroadcaster: peer entity spawns at 500 m AGL when ground elevation is zero", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:5000";
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    // setGroundElevation not called — default m_groundElevation = 0.f
    broadcaster.onConnect(0u);
    broadcaster.onTick(1.0 / 60.0, 1u);

    REQUIRE(!snapshotsFor(net, 0).empty());
    const auto pkt = snapshotsFor(net, 0).back();
    REQUIRE(parseSnapshotHeader(pkt).recordCount >= 1u);
    const auto _ents = decodeEntities(pkt);
    REQUIRE(!_ents.empty());
    const DecodedEntity& e = _ents[0];
    // 0 m terrain + 500 m AGL = 500 m; allow ±10 m for one flight-integrator tick
    CHECK(e.pos[1] >= 490.0);
    CHECK(e.pos[1] <= 510.0);
}

TEST_CASE("WorldBroadcaster: peer entity spawns at configured spawn point XYZ", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:5000";
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setSpawnPoints({std::array<double, 3>{1000.0, 750.0, -500.0}});
    broadcaster.onConnect(0u);
    broadcaster.onTick(1.0 / 60.0, 1u);

    REQUIRE(!snapshotsFor(net, 0).empty());
    const auto pkt = snapshotsFor(net, 0).back();
    REQUIRE(parseSnapshotHeader(pkt).recordCount >= 1u);
    const auto _ents = decodeEntities(pkt);
    REQUIRE(!_ents.empty());
    const DecodedEntity& e = _ents[0];
    // X and Z are set from the spawn point; Y allows ±10 m for one flight-integrator tick.
    CHECK(e.pos[0] >= 999.0);
    CHECK(e.pos[0] <= 1001.0);
    CHECK(e.pos[1] >= 740.0);
    CHECK(e.pos[1] <= 760.0);
    CHECK(e.pos[2] >= -501.0);
    CHECK(e.pos[2] <= -499.0);
}

TEST_CASE("WorldBroadcaster: spawn points assigned round-robin to peers", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:5000";
    net.peerAddresses[1] = "5.6.7.8:6000";
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setSpawnPoints({std::array<double, 3>{0.0, 200.0, 0.0}, std::array<double, 3>{1000.0, 300.0, 500.0}});
    broadcaster.onConnect(0u); // → point 0 (X=0)
    broadcaster.onConnect(1u); // → point 1 (X=1000)
    broadcaster.onTick(1.0 / 60.0, 1u);

    REQUIRE(!snapshotsFor(net, 0).empty());
    const auto pkt = snapshotsFor(net, 0).back();
    auto _ents = decodeEntities(pkt);
    REQUIRE(_ents.size() >= 2u);

    DecodedEntity e0 = _ents[0], e1 = _ents[1];
    // Sort by X so the check is order-independent.
    if (e0.pos[0] > e1.pos[0])
        std::swap(e0, e1);

    // e0 → point 0 (X≈0), e1 → point 1 (X≈1000)
    CHECK(e0.pos[0] >= -1.0);
    CHECK(e0.pos[0] <= 1.0);
    CHECK(e1.pos[0] >= 999.0);
    CHECK(e1.pos[0] <= 1001.0);
    CHECK(e1.pos[2] >= 499.0);
    CHECK(e1.pos[2] <= 501.0);
}

TEST_CASE("WorldBroadcaster: spawn point index wraps round-robin with three peers two points", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:5000";
    net.peerAddresses[1] = "5.6.7.8:6000";
    net.peerAddresses[2] = "9.10.11.12:7000";
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setSpawnPoints({std::array<double, 3>{0.0, 200.0, 0.0}, std::array<double, 3>{1000.0, 300.0, 0.0}});
    broadcaster.onConnect(0u); // → point 0 (X≈0)
    broadcaster.onConnect(1u); // → point 1 (X≈1000)
    broadcaster.onConnect(2u); // → point 0 again (wrap)
    broadcaster.onTick(1.0 / 60.0, 1u);

    REQUIRE(!snapshotsFor(net, 0).empty());
    const auto pkt = snapshotsFor(net, 0).back();
    const auto entries = decodeEntities(pkt);
    REQUIRE(entries.size() >= 3u);

    // Count how many entities landed near X=0 vs X=1000 (tolerance ±1 m).
    int nearPoint0 = 0;
    int nearPoint1 = 0;
    for (const auto& e : entries) {
        if (e.pos[0] >= -1.0 && e.pos[0] <= 1.0)
            ++nearPoint0;
        else if (e.pos[0] >= 999.0 && e.pos[0] <= 1001.0)
            ++nearPoint1;
    }
    CHECK(nearPoint0 == 2); // peers 0 and 2 → point 0
    CHECK(nearPoint1 == 1); // peer 1 → point 1
}

TEST_CASE("WorldBroadcaster: onConnect with empty registry sends typeCount=0 and assigns no entity",
          "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    broadcaster.onConnect(0u);

    REQUIRE(net.sends.size() == 2u);
    fl::MsgConnectAck ack = parseSendAck(net);
    CHECK(ack.typeCount == 0u);
    CHECK(ack.assignedEntityIdx == 0u); // spawn failed — type not registered
    CHECK(ack.assignedEntityGen == 0u);
    CHECK(em.liveCount() == 0u);
}

TEST_CASE("WorldBroadcaster: onConnect without builtin type registered assigns no entity", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);

    // Register a type but NOT "builtin:debug-entity"
    registry.registerType(makeDebugDef("other:type"));

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.onConnect(0u);

    fl::MsgConnectAck ack = parseSendAck(net);
    CHECK(ack.assignedEntityIdx == 0u); // "builtin:debug-entity" not found
    CHECK(ack.assignedEntityGen == 0u);
    CHECK(em.liveCount() == 0u);
}

TEST_CASE("WorldBroadcaster: onDisconnect after connect removes peer entity", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);

    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    broadcaster.onConnect(0u);
    // liveCount() is updated by onTick — drive a tick to confirm the spawn landed.
    broadcaster.onTick(1.0 / 60.0, 0u);
    REQUIRE(em.liveCount() == 1u);

    broadcaster.onDisconnect(0u);
    // Entity is marked dead and reaped on the next onTick.
    const std::size_t sendsBefore = net.sends.size(); // includes Hello + ConnectAck + snapshot from tick 0
    broadcaster.onTick(1.0 / 60.0, 1u);
    CHECK(em.liveCount() == 0u);
    CHECK(net.sends.size() == sendsBefore); // nothing extra after disconnect (no peer left)
}

TEST_CASE("WorldBroadcaster: onDisconnect does not crash and sends nothing", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    REQUIRE_NOTHROW(broadcaster.onDisconnect(0u));
    CHECK(net.sends.empty());
    CHECK(snapshotsFor(net, 0).empty());
}

TEST_CASE("WorldBroadcaster: onReceive is a no-op for unknown msgId", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    const uint8_t garbage[] = {0xDE, 0xAD, 0xBE, 0xEF};
    REQUIRE_NOTHROW(broadcaster.onReceive(0u, garbage, sizeof(garbage)));
    CHECK(net.sends.empty());
    CHECK(snapshotsFor(net, 0).empty());
}

TEST_CASE("WorldBroadcaster: onReceive empty packet is discarded", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);

    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.onConnect(0u);
    clearSnapshots(net);
    net.sends.clear();

    broadcaster.onReceive(0u, nullptr, 0u);
    broadcaster.onTick(1.0 / 60.0, 1u);

    // Entity should not have moved (no input applied).
    const auto pkt = snapshotsFor(net, 0)[0];
    fl::MsgWorldSnapshotHeader hdr;
    std::memcpy(&hdr, pkt.data(), sizeof(hdr));
    REQUIRE(totalEntityCount(hdr) >= 1u);
    const auto _ents = decodeEntities(pkt);
    REQUIRE(!_ents.empty());
    const DecodedEntity& e = _ents[0];
    // Empty packet discarded: entity is present and data is finite (craft still flies).
    CHECK(std::isfinite(e.vel[0]));
    CHECK(std::isfinite(e.vel[1]));
    CHECK(std::isfinite(e.vel[2]));
}

TEST_CASE("WorldBroadcaster: onReceive valid ClientInput moves entity on next tick", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);

    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.onConnect(0u);

    fl::MsgClientInput inp{};
    inp.msgId = static_cast<uint8_t>(fl::MsgId::ClientInput);
    inp.throttle = 1.f; // full throttle
    broadcaster.onReceive(0u, &inp, sizeof(inp));

    clearSnapshots(net);
    broadcaster.onTick(1.0 / 60.0, 1u);

    REQUIRE(!snapshotsFor(net, 0).empty());
    const auto pkt = snapshotsFor(net, 0)[0];
    fl::MsgWorldSnapshotHeader hdr;
    std::memcpy(&hdr, pkt.data(), sizeof(hdr));
    REQUIRE(totalEntityCount(hdr) >= 1u);
    const auto _ents = decodeEntities(pkt);
    REQUIRE(!_ents.empty());
    const DecodedEntity& e = _ents[0];

    // Entity starts with identity orientation (+X forward); throttle=1 should produce
    // non-zero velocity in the +X direction.
    CHECK(e.vel[0] > 0.f);
}

TEST_CASE("WorldBroadcaster: onReceive truncated ClientInput is discarded", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);

    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.onConnect(0u);
    clearSnapshots(net);

    // Only 10 bytes — less than sizeof(MsgClientInput) = 44.
    const uint8_t tiny[] = {static_cast<uint8_t>(fl::MsgId::ClientInput), 0, 0, 0, 0, 0, 0, 0, 0, 0};
    broadcaster.onReceive(0u, tiny, sizeof(tiny));
    broadcaster.onTick(1.0 / 60.0, 1u);

    const auto pkt = snapshotsFor(net, 0)[0];
    fl::MsgWorldSnapshotHeader hdr;
    std::memcpy(&hdr, pkt.data(), sizeof(hdr));
    REQUIRE(totalEntityCount(hdr) >= 1u);
    const auto _ents = decodeEntities(pkt);
    REQUIRE(!_ents.empty());
    const DecodedEntity& e = _ents[0];
    // Truncated packet discarded: entity is present and velocity is finite.
    CHECK(std::isfinite(e.vel[0]));
    CHECK(std::isfinite(e.vel[1]));
    CHECK(std::isfinite(e.vel[2]));
}

TEST_CASE("WorldBroadcaster: onReceive clamps out-of-range throttle", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);

    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.onConnect(0u);
    clearSnapshots(net);

    fl::MsgClientInput inp{};
    inp.msgId = static_cast<uint8_t>(fl::MsgId::ClientInput);
    inp.throttle = 5.f; // out-of-range; must be clamped to 1.0
    broadcaster.onReceive(0u, &inp, sizeof(inp));

    broadcaster.onTick(1.0 / 60.0, 1u);

    const auto pkt = snapshotsFor(net, 0)[0];
    fl::MsgWorldSnapshotHeader hdr;
    std::memcpy(&hdr, pkt.data(), sizeof(hdr));
    REQUIRE(totalEntityCount(hdr) >= 1u);
    const auto _ents = decodeEntities(pkt);
    REQUIRE(!_ents.empty());
    const DecodedEntity& e = _ents[0];

    // Throttle clamped to 1.0 (not 5.0): craft accelerates forward without exceeding physics limits.
    CHECK(e.vel[0] > 0.f);
    CHECK(std::isfinite(e.vel[1]));
}

TEST_CASE("WorldBroadcaster: onReceive zero viewAxis uses forward fallback", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);

    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.onConnect(0u);
    clearSnapshots(net);

    fl::MsgClientInput inp{};
    inp.msgId = static_cast<uint8_t>(fl::MsgId::ClientInput);
    inp.throttle = 1.f;
    inp.viewAxis[0] = 0.f; // degenerate — all zero
    inp.viewAxis[1] = 0.f;
    inp.viewAxis[2] = 0.f;
    broadcaster.onReceive(0u, &inp, sizeof(inp));

    broadcaster.onTick(1.0 / 60.0, 1u);

    // Entity should still move (fallback viewAxis {1,0,0} used for normalisation;
    // actual kinematics uses entity quaternion, not viewAxis directly, so entity moves).
    const auto pkt = snapshotsFor(net, 0)[0];
    fl::MsgWorldSnapshotHeader hdr;
    std::memcpy(&hdr, pkt.data(), sizeof(hdr));
    REQUIRE(totalEntityCount(hdr) >= 1u);
    const auto _ents = decodeEntities(pkt);
    REQUIRE(!_ents.empty());
    const DecodedEntity& e = _ents[0];
    CHECK(e.vel[0] > 0.f); // entity moves forward (+X) with identity orientation
}

TEST_CASE("WorldBroadcaster: onReceive after peer disconnects has no effect on tick", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);

    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    broadcaster.onConnect(0u);
    broadcaster.onDisconnect(0u); // entity killed; maps cleared

    // Client sends an input after disconnecting — server re-adds to m_peerInputs only.
    fl::MsgClientInput inp{};
    inp.msgId = static_cast<uint8_t>(fl::MsgId::ClientInput);
    inp.throttle = 1.f;
    broadcaster.onReceive(0u, &inp, sizeof(inp));

    clearSnapshots(net);
    // onTick: kinematics loop finds peerId 0 in m_peerInputs but NOT in m_peerEntities -> skip.
    REQUIRE_NOTHROW(broadcaster.onTick(1.0 / 60.0, 1u));
    // Entity was reaped; liveCount = 0.
    CHECK(em.liveCount() == 0u);
}

TEST_CASE("WorldBroadcaster: onTick skips kinematics for dead entity", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);

    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    broadcaster.onConnect(0u);

    fl::MsgClientInput inp{};
    inp.msgId = static_cast<uint8_t>(fl::MsgId::ClientInput);
    inp.throttle = 1.f;
    broadcaster.onReceive(0u, &inp, sizeof(inp));

    // Kill the entity externally — marks dead=true, queues reap.
    // Entity remains in m_peerEntities (disconnect hasn't happened).
    fl::MsgConnectAck ack = parseSendAck(net);
    fl::EntityId id;
    id.index = ack.assignedEntityIdx;
    id.generation = ack.assignedEntityGen;
    em.kill(id);

    // onTick: kinematics loop calls get(id) -> state->dead == true -> skip.
    clearSnapshots(net);
    REQUIRE_NOTHROW(broadcaster.onTick(1.0 / 60.0, 1u));
}

TEST_CASE("WorldBroadcaster: two peers each control independent entities", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);

    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    broadcaster.onConnect(0u);
    broadcaster.onConnect(1u);

    // Peer 0: full throttle forward; peer 1: no throttle.
    // Set inputs BEFORE the first tick. On tick 0 all entities are new to their peers, so they
    // appear as full records — this lets us verify velocity via parseFullEntries.
    fl::MsgClientInput inp0{};
    inp0.msgId = static_cast<uint8_t>(fl::MsgId::ClientInput);
    inp0.seqNum = 1u;
    inp0.throttle = 1.f;
    broadcaster.onReceive(0u, &inp0, sizeof(inp0));

    fl::MsgClientInput inp1{};
    inp1.msgId = static_cast<uint8_t>(fl::MsgId::ClientInput);
    inp1.seqNum = 1u;
    inp1.throttle = 0.f;
    broadcaster.onReceive(1u, &inp1, sizeof(inp1));

    broadcaster.onTick(1.0 / 60.0, 0u); // tick 0: all entities new → full entries
    REQUIRE(em.liveCount() == 2u);

    // Snapshot has both entities; find peer 0's and peer 1's entries by assigned idx.
    // onConnect sends MsgHello (even index) then ConnectAck (odd index) for each peer.
    fl::MsgConnectAck ack0, ack1;
    std::memcpy(&ack0, net.sends[1].data(), sizeof(ack0)); // peer 0: sends[0]=Hello, sends[1]=Ack
    std::memcpy(&ack1, net.sends[3].data(), sizeof(ack1)); // peer 1: sends[2]=Hello, sends[3]=Ack

    REQUIRE(!snapshotsFor(net, 0).empty());
    const auto pkt = snapshotsFor(net, 0)[0];
    fl::MsgWorldSnapshotHeader hdr;
    std::memcpy(&hdr, pkt.data(), sizeof(hdr));
    CHECK(totalEntityCount(hdr) == 2u);

    // Find entries for each peer's entity by index within peer 0's snapshot.
    // With default draw distance (200 km) both entities are visible to peer 0.
    DecodedEntity ePeer0{}, ePeer1{};
    for (const auto& e : parseFullEntries(pkt)) {
        if (e.entityIdx == ack0.assignedEntityIdx)
            ePeer0 = e;
        else if (e.entityIdx == ack1.assignedEntityIdx)
            ePeer1 = e;
    }

    // Peer 0 (throttle=1) accelerates via thrust; peer 1 (throttle=0) decelerates via drag.
    // After one tick from the same initial 40 m/s, peer 0 must be faster than peer 1.
    CHECK(ePeer0.vel[0] > 0.f);
    CHECK(ePeer0.vel[0] > ePeer1.vel[0]);
    CHECK(std::isfinite(ePeer1.vel[0]));
    CHECK(std::isfinite(ePeer1.vel[1]));
}

TEST_CASE("WorldBroadcaster: onConnect sends MsgHello as first reliable packet", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);

    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.onConnect(0u);

    REQUIRE(net.sends.size() == 2u);

    fl::MsgHello hello = parseSendHello(net);
    CHECK(hello.msgId == static_cast<uint8_t>(fl::MsgId::Hello));
    CHECK(hello.protocolVersion == fl::kProtocolVersion);

    fl::MsgConnectAck ack = parseSendAck(net);
    CHECK(ack.msgId == static_cast<uint8_t>(fl::MsgId::ConnectAck));
}

TEST_CASE("WorldBroadcaster: onReceive discards MsgClientInput with mismatched protocolVersion",
          "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);

    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.onConnect(0u);

    // Send a full-throttle input with the wrong protocol version.
    fl::MsgClientInput inp{};
    inp.msgId = static_cast<uint8_t>(fl::MsgId::ClientInput);
    inp.throttle = 1.f;
    inp.protocolVersion = 0xFFFFu; // deliberate mismatch
    broadcaster.onReceive(0u, &inp, sizeof(inp));

    clearSnapshots(net);
    broadcaster.onTick(1.0 / 60.0, 1u);

    REQUIRE(!snapshotsFor(net, 0).empty());
    const auto pkt = snapshotsFor(net, 0)[0];
    fl::MsgWorldSnapshotHeader hdr;
    std::memcpy(&hdr, pkt.data(), sizeof(hdr));
    REQUIRE(totalEntityCount(hdr) >= 1u);
    const auto _ents = decodeEntities(pkt);
    REQUIRE(!_ents.empty());
    const DecodedEntity& e = _ents[0];
    // Mismatched version discarded: default throttle=0 input used instead of throttle=1.
    // Full throttle from 40 m/s produces ~40.6 m/s in one tick; this must stay < 40.1 m/s.
    CHECK(e.vel[0] < 40.1f);
}

TEST_CASE("WorldBroadcaster: onTick snapshot carries correct protocolVersion", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.onConnect(0u); // peer required for per-peer unicast

    broadcaster.onTick(1.0 / 60.0, 1u);

    REQUIRE(!snapshotsFor(net, 0).empty());
    auto hdr = parseSnapshotHeader(snapshotsFor(net, 0)[0]);
    CHECK(hdr.protocolVersion == static_cast<uint8_t>(fl::kProtocolVersion));
}

// ---------------------------------------------------------------------------
// getPeerCount
// ---------------------------------------------------------------------------

TEST_CASE("WorldBroadcaster: getPeerCount is zero before any connections", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    CHECK(broadcaster.getPeerCount() == 0);
}

TEST_CASE("WorldBroadcaster: getPeerCount tracks connect and disconnect", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    CHECK(broadcaster.getPeerCount() == 0);

    broadcaster.onConnect(42);
    CHECK(broadcaster.getPeerCount() == 1);

    broadcaster.onDisconnect(42);
    CHECK(broadcaster.getPeerCount() == 0);
}

TEST_CASE("WorldBroadcaster: onTick populates throttle in WorldSnapshot from FlightState", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);

    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.onConnect(0u);

    // Send a client input with full throttle.
    fl::MsgClientInput inp{};
    inp.msgId = static_cast<uint8_t>(fl::MsgId::ClientInput);
    inp.protocolVersion = fl::kProtocolVersion;
    inp.throttle = 1.f;
    broadcaster.onReceive(0u, &inp, sizeof(inp));

    clearSnapshots(net);
    // Multiple ticks allow the spool-up lag to partially settle.
    for (int i = 0; i < 10; ++i)
        broadcaster.onTick(1.0 / 60.0, static_cast<uint64_t>(i + 1));

    REQUIRE(!snapshotsFor(net, 0).empty());
    const auto pkt = snapshotsFor(net, 0).back(); // last of 10 snapshots

    auto hdr = parseSnapshotHeader(pkt);
    REQUIRE(totalEntityCount(hdr) >= 1u);

    // throttle_actual spools up over time; after 10 ticks at 1.f input it should be > 0.
    // Quantized records (full on tick 1, delta on ticks 2-10) both carry throttle.
    const auto _ents = decodeEntities(pkt);
    REQUIRE(!_ents.empty());
    const uint8_t throttle = _ents[0].throttle;
    CHECK(throttle > 0u);
    CHECK(throttle <= 100u);
}

TEST_CASE("WorldBroadcaster: abEngaged is 0 in WorldSnapshot when model has no afterburner table",
          "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);

    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.onConnect(0u);

    fl::MsgClientInput inp{};
    inp.msgId = static_cast<uint8_t>(fl::MsgId::ClientInput);
    inp.protocolVersion = fl::kProtocolVersion;
    inp.buttons = 0x02u; // afterburner bit set
    inp.throttle = 1.f;
    broadcaster.onReceive(0u, &inp, sizeof(inp));

    clearSnapshots(net);
    broadcaster.onTick(1.0 / 60.0, 1u);

    REQUIRE(!snapshotsFor(net, 0).empty());
    const auto pkt = snapshotsFor(net, 0).back();
    REQUIRE(parseSnapshotHeader(pkt).recordCount >= 1u);

    fl::MsgWorldSnapshotHeader hdr;
    std::memcpy(&hdr, pkt.data(), sizeof(hdr));
    REQUIRE(totalEntityCount(hdr) >= 1u);

    const auto _ents = decodeEntities(pkt);
    REQUIRE(!_ents.empty());
    const DecodedEntity& e = _ents[0];

    // Builtin model has no ab_thrust table → FlightState::ab_engaged stays false → packed as 0.
    CHECK(e.abEngaged == 0u);
}

TEST_CASE("WorldBroadcaster: engineFailFlags has kEngineFailGeneric when entity damage is Heavy",
          "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);

    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.onConnect(0u);

    fl::MsgConnectAck ack = parseSendAck(net);
    fl::EntityId id;
    id.index = ack.assignedEntityIdx;
    id.generation = ack.assignedEntityGen;
    auto* state = em.get(id);
    REQUIRE(state != nullptr);
    state->damageLevel = fl::DamageLevel::Heavy; // >= 2 → kEngineFailGeneric OR'd in

    clearSnapshots(net);
    broadcaster.onTick(1.0 / 60.0, 1u);

    REQUIRE(!snapshotsFor(net, 0).empty());
    const auto pkt = snapshotsFor(net, 0).back();
    REQUIRE(parseSnapshotHeader(pkt).recordCount >= 1u);

    fl::MsgWorldSnapshotHeader hdr;
    std::memcpy(&hdr, pkt.data(), sizeof(hdr));
    REQUIRE(totalEntityCount(hdr) >= 1u);

    const auto _ents = decodeEntities(pkt);
    REQUIRE(!_ents.empty());
    const DecodedEntity& e = _ents[0];

    CHECK((e.engineFailFlags & fl::kEngineFailGeneric) != 0u);
}

// ---------------------------------------------------------------------------
// seqNum staleness guard and delay estimation
// ---------------------------------------------------------------------------

TEST_CASE("WorldBroadcaster: onReceive discards duplicate seqNum", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.onConnect(0u);

    // First packet (seqNum=5, throttle=0) accepted.
    fl::MsgClientInput inp{};
    inp.msgId = static_cast<uint8_t>(fl::MsgId::ClientInput);
    inp.protocolVersion = fl::kProtocolVersion;
    inp.seqNum = 5u;
    inp.throttle = 0.f;
    broadcaster.onReceive(0u, &inp, sizeof(inp));

    // Duplicate seqNum=5 with throttle=1 must be dropped; stored throttle stays 0.
    inp.throttle = 1.f;
    broadcaster.onReceive(0u, &inp, sizeof(inp));

    clearSnapshots(net);
    broadcaster.onTick(1.0 / 60.0, 1u);

    REQUIRE(!snapshotsFor(net, 0).empty());
    fl::MsgWorldSnapshotHeader hdr;
    std::memcpy(&hdr, snapshotsFor(net, 0)[0].data(), sizeof(hdr));
    REQUIRE(totalEntityCount(hdr) >= 1u);
    const auto _ents = decodeEntities(snapshotsFor(net, 0)[0]);
    REQUIRE(!_ents.empty());
    const DecodedEntity& e = _ents[0];
    // throttle=0 retained (idle thrust only) → vel stays below full-throttle level.
    CHECK(e.vel[0] < 0.2f);
}

TEST_CASE("WorldBroadcaster: onReceive discards stale seqNum (out-of-order)", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.onConnect(0u);

    // First packet: seqNum=5, throttle=1 accepted.
    fl::MsgClientInput inp{};
    inp.msgId = static_cast<uint8_t>(fl::MsgId::ClientInput);
    inp.protocolVersion = fl::kProtocolVersion;
    inp.seqNum = 5u;
    inp.throttle = 1.f;
    broadcaster.onReceive(0u, &inp, sizeof(inp));

    // Out-of-order: seqNum=3 (stale) with throttle=0 must be dropped.
    inp.seqNum = 3u;
    inp.throttle = 0.f;
    broadcaster.onReceive(0u, &inp, sizeof(inp));

    clearSnapshots(net);
    broadcaster.onTick(1.0 / 60.0, 1u);

    REQUIRE(!snapshotsFor(net, 0).empty());
    fl::MsgWorldSnapshotHeader hdr;
    std::memcpy(&hdr, snapshotsFor(net, 0)[0].data(), sizeof(hdr));
    REQUIRE(totalEntityCount(hdr) >= 1u);
    const auto _ents = decodeEntities(snapshotsFor(net, 0)[0]);
    REQUIRE(!_ents.empty());
    const DecodedEntity& e = _ents[0];
    // throttle=1 retained → full thrust; vel clearly above zero (idle) level.
    CHECK(e.vel[0] > 0.05f);
}

TEST_CASE("WorldBroadcaster: onReceive accepts seqNum wrap-around", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.onConnect(0u);

    fl::MsgClientInput inp{};
    inp.msgId = static_cast<uint8_t>(fl::MsgId::ClientInput);
    inp.protocolVersion = fl::kProtocolVersion;

    // Prime with UINT32_MAX; throttle=0 accepted (first packet, hasSeq=false).
    inp.seqNum = 0xFFFFFFFFu;
    inp.throttle = 0.f;
    broadcaster.onReceive(0u, &inp, sizeof(inp));

    // seqNum=0 wraps around: isNewerSeq(0, UINT32_MAX) must return true.
    inp.seqNum = 0u;
    inp.throttle = 1.f;
    broadcaster.onReceive(0u, &inp, sizeof(inp));

    // seqNum=UINT32_MAX-1 is now stale (older than 0 under half-window); must be dropped.
    inp.seqNum = 0xFFFFFFFEu;
    inp.throttle = 0.f;
    broadcaster.onReceive(0u, &inp, sizeof(inp));

    clearSnapshots(net);
    broadcaster.onTick(1.0 / 60.0, 1u);

    REQUIRE(!snapshotsFor(net, 0).empty());
    fl::MsgWorldSnapshotHeader hdr;
    std::memcpy(&hdr, snapshotsFor(net, 0)[0].data(), sizeof(hdr));
    REQUIRE(totalEntityCount(hdr) >= 1u);
    const auto _ents = decodeEntities(snapshotsFor(net, 0)[0]);
    REQUIRE(!_ents.empty());
    const DecodedEntity& e = _ents[0];
    // seqNum=0 throttle=1 retained (UINT32_MAX-1 dropped) → full thrust; vel above zero (idle) level.
    CHECK(e.vel[0] > 0.05f);
}

TEST_CASE("WorldBroadcaster: onReceive computes estimatedDelayTicks from tickIndex", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.onConnect(0u);

    // Advance to tick 10 so m_currentTick = 10.
    broadcaster.onTick(1.0 / 60.0, 10u);
    clearSnapshots(net);

    fl::MsgClientInput inp{};
    inp.msgId = static_cast<uint8_t>(fl::MsgId::ClientInput);
    inp.protocolVersion = fl::kProtocolVersion;
    inp.seqNum = 1u;
    inp.tickIndex = 5u; // client last saw tick 5; delay = 10 - 5 = 5 ticks
    broadcaster.onReceive(0u, &inp, sizeof(inp));

    uint32_t gotDelay = 0xFFFFFFFFu;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) { gotDelay = pi.delayTicks; });
    CHECK(gotDelay == 5u);
}

TEST_CASE("WorldBroadcaster: onReceive future tickIndex does not update estimatedDelayTicks", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.onConnect(0u);

    // Advance to tick 3 so m_currentTick = 3.
    broadcaster.onTick(1.0 / 60.0, 3u);
    clearSnapshots(net);

    // Client sends tickIndex=10 (in the future from the server's perspective).
    fl::MsgClientInput inp{};
    inp.msgId = static_cast<uint8_t>(fl::MsgId::ClientInput);
    inp.protocolVersion = fl::kProtocolVersion;
    inp.seqNum = 1u;
    inp.tickIndex = 10u; // tickIndex > m_currentTick: guard must prevent underflow
    broadcaster.onReceive(0u, &inp, sizeof(inp));

    uint32_t gotDelay = 0xFFFFFFFFu;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) { gotDelay = pi.delayTicks; });
    // estimatedDelayTicks stays at its initialized value of 0 (no underflow).
    CHECK(gotDelay == 0u);
}

// ---------------------------------------------------------------------------
// Weather integration tests
// ---------------------------------------------------------------------------

TEST_CASE("WorldBroadcaster: with WeatherController broadcasts MsgWeatherState 0x04", "[world_broadcaster][weather]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::WeatherController weather;
    fl::WorldBroadcaster broadcaster(em, registry, net, logger, &weather);

    // Run 10 ticks — MsgWeatherState broadcasts every 10 ticks
    for (int i = 0; i < 10; ++i)
        broadcaster.onTick(1.0 / 60.0, static_cast<uint64_t>(i + 1));

    // At least one broadcast should be MsgWeatherState (msgId == 0x04)
    bool foundWeather = false;
    for (const auto& pkt : net.broadcasts) {
        if (!pkt.empty() && pkt[0] == 0x04u) {
            foundWeather = true;
            // Verify minimum size
            CHECK(pkt.size() >= sizeof(fl::MsgWeatherState));
        }
    }
    CHECK(foundWeather);
}

TEST_CASE("WorldBroadcaster: without WeatherController does not broadcast 0x04", "[world_broadcaster][weather]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::WorldBroadcaster broadcaster(em, registry, net, logger, nullptr);

    for (int i = 0; i < 10; ++i)
        broadcaster.onTick(1.0 / 60.0, static_cast<uint64_t>(i + 1));

    for (const auto& pkt : net.broadcasts)
        if (!pkt.empty())
            CHECK(pkt[0] != 0x04u);
}

// ---------------------------------------------------------------------------
// Peer management: kick / ban / unban / forEachPeer
// ---------------------------------------------------------------------------

TEST_CASE("WorldBroadcaster: kickPeer calls disconnectPeer on network", "[world_broadcaster][admin]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    net.peerAddresses[0] = "1.2.3.4:5000";
    broadcaster.onConnect(0u);
    net.disconnectedPeers.clear();

    broadcaster.kickPeer(0u);

    REQUIRE(net.disconnectedPeers.size() == 1u);
    CHECK(net.disconnectedPeers[0] == 0u);
}

TEST_CASE("WorldBroadcaster: banAddress kicks connected peer with matching IPv4", "[world_broadcaster][admin]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    net.peerAddresses[0] = "1.2.3.4:5000";
    broadcaster.onConnect(0u);
    net.disconnectedPeers.clear();

    broadcaster.banAddress("1.2.3.4");

    REQUIRE(net.disconnectedPeers.size() == 1u);
    CHECK(net.disconnectedPeers[0] == 0u);
}

TEST_CASE("WorldBroadcaster: banAddress with no connected peers does not crash", "[world_broadcaster][admin]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    REQUIRE_NOTHROW(broadcaster.banAddress("10.0.0.1"));
    CHECK(net.disconnectedPeers.empty());
}

TEST_CASE("WorldBroadcaster: banAddress does not kick peer on different IP", "[world_broadcaster][admin]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    net.peerAddresses[0] = "5.5.5.5:5000";
    broadcaster.onConnect(0u);
    net.disconnectedPeers.clear();

    broadcaster.banAddress("1.2.3.4"); // different IP — peer 0 must not be kicked

    CHECK(net.disconnectedPeers.empty());
}

TEST_CASE("WorldBroadcaster: banned IPv4 peer is rejected on onConnect", "[world_broadcaster][admin]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    broadcaster.banAddress("1.2.3.4");
    net.peerAddresses[0] = "1.2.3.4:5000";
    broadcaster.onConnect(0u);

    // Peer was rejected: MsgConnectRefusal sent, then disconnectPeer called.
    REQUIRE(net.disconnectedPeers.size() == 1u);
    CHECK(net.disconnectedPeers[0] == 0u);
    auto ref = parseSendRefusal(net);
    CHECK(ref.msgId == static_cast<uint8_t>(fl::MsgId::ConnectRefusal));
    CHECK(std::string_view(ref.reason) == "You are banned from this server.");
}

TEST_CASE("WorldBroadcaster: IPv4-mapped IPv6 peer is rejected when IPv4 is banned", "[world_broadcaster][admin]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    broadcaster.banAddress("1.2.3.4");
    net.peerAddresses[0] = "[::ffff:1.2.3.4]:5000"; // dual-stack mapped form
    broadcaster.onConnect(0u);

    REQUIRE(net.disconnectedPeers.size() == 1u);
    auto ref = parseSendRefusal(net);
    CHECK(ref.msgId == static_cast<uint8_t>(fl::MsgId::ConnectRefusal));
    CHECK(std::string_view(ref.reason) == "You are banned from this server.");
}

TEST_CASE("WorldBroadcaster: peer on non-banned IP is allowed on onConnect", "[world_broadcaster][admin]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    broadcaster.banAddress("9.9.9.9");
    net.peerAddresses[0] = "1.2.3.4:5000"; // different IP — must be allowed
    broadcaster.onConnect(0u);

    CHECK(net.disconnectedPeers.empty());
    CHECK(net.sends.size() >= 2u); // MsgHello + ConnectAck
}

TEST_CASE("WorldBroadcaster: unbanAddress allows reconnect after ban", "[world_broadcaster][admin]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    broadcaster.banAddress("1.2.3.4");
    broadcaster.unbanAddress("1.2.3.4");

    net.peerAddresses[0] = "1.2.3.4:5000";
    broadcaster.onConnect(0u);

    CHECK(net.disconnectedPeers.empty());
    CHECK(net.sends.size() >= 2u);
}

TEST_CASE("WorldBroadcaster: forEachPeer calls fn for each connected peer", "[world_broadcaster][admin]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    net.peerAddresses[0] = "1.2.3.4:5000";
    net.peerAddresses[1] = "5.6.7.8:6000";
    broadcaster.onConnect(0u);
    broadcaster.onConnect(1u);

    int callCount = 0;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) {
        fl::EntityId eid = pi.eid;
        CHECK(eid.valid());
        ++callCount;
    });
    CHECK(callCount == 2);
}

TEST_CASE("WorldBroadcaster: forEachPeer with no connected peers does not call fn", "[world_broadcaster][admin]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    int callCount = 0;
    broadcaster.forEachPeer([&](const fl::PeerInfo&) { ++callCount; });
    CHECK(callCount == 0);
}

// ---------------------------------------------------------------------------
// Security: rate limiting
// ---------------------------------------------------------------------------

static std::vector<uint8_t> makeClientInputPacket() {
    fl::MsgClientInput inp{};
    inp.msgId = static_cast<uint8_t>(fl::MsgId::ClientInput);
    inp.protocolVersion = fl::kProtocolVersion;
    inp.viewAxis[0] = 1.0f;
    std::vector<uint8_t> buf(sizeof(inp));
    std::memcpy(buf.data(), &inp, sizeof(inp));
    return buf;
}

TEST_CASE("WorldBroadcaster: IP under rate limit is not disconnected", "[world_broadcaster][security]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    fl::ManualClock t;
    broadcaster.setClock(t);
    broadcaster.setRateLimitParams(5, 10, 3);

    net.peerAddresses[0] = "1.2.3.4:1000";
    for (int i = 0; i < 4; ++i) {
        broadcaster.onConnect(0u);
        broadcaster.onDisconnect(0u);
        net.disconnectedPeers.clear();
        net.sends.clear();
    }
    // 5th connect (at limit, size == limit, not strictly over): should be allowed
    broadcaster.onConnect(0u);
    CHECK(net.disconnectedPeers.empty());
}

TEST_CASE("WorldBroadcaster: IP exceeding rate limit is disconnected", "[world_broadcaster][security]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    fl::ManualClock t;
    broadcaster.setClock(t);
    broadcaster.setRateLimitParams(3, 10, 3);

    net.peerAddresses[0] = "1.2.3.4:1000";
    // 3 connects fill the window
    for (int i = 0; i < 3; ++i) {
        broadcaster.onConnect(0u);
        broadcaster.onDisconnect(0u);
        net.disconnectedPeers.clear();
        net.sends.clear();
    }
    // 4th connect (over limit) must be rejected
    broadcaster.onConnect(0u);
    REQUIRE(net.disconnectedPeers.size() == 1u);
    CHECK(net.disconnectedPeers[0] == 0u);
    auto ref = parseSendRefusal(net);
    CHECK(ref.msgId == static_cast<uint8_t>(fl::MsgId::ConnectRefusal));
    CHECK(std::string_view(ref.reason) == "Connection rate limit exceeded. Try again later.");
}

TEST_CASE("WorldBroadcaster: rate limit resets after window expires", "[world_broadcaster][security]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    fl::ManualClock t;
    broadcaster.setClock(t);
    broadcaster.setRateLimitParams(2, 5, 3);

    net.peerAddresses[0] = "1.2.3.4:1000";
    // Fill window: 2 connects
    broadcaster.onConnect(0u);
    broadcaster.onDisconnect(0u);
    net.disconnectedPeers.clear();
    net.sends.clear();
    broadcaster.onConnect(0u);
    broadcaster.onDisconnect(0u);
    net.disconnectedPeers.clear();
    net.sends.clear();

    // Advance clock past the window
    t.advance(std::chrono::seconds(6));

    // Should be allowed again
    broadcaster.onConnect(0u);
    CHECK(net.disconnectedPeers.empty());
}

TEST_CASE("WorldBroadcaster: rate limit tracks different IPs independently", "[world_broadcaster][security]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    fl::ManualClock t;
    broadcaster.setClock(t);
    broadcaster.setRateLimitParams(2, 10, 3);

    // IP-A fills limit
    net.peerAddresses[0] = "1.1.1.1:1000";
    broadcaster.onConnect(0u);
    broadcaster.onDisconnect(0u);
    net.disconnectedPeers.clear();
    net.sends.clear();
    broadcaster.onConnect(0u);
    broadcaster.onDisconnect(0u);
    net.disconnectedPeers.clear();
    net.sends.clear();
    broadcaster.onConnect(0u); // 3rd from IP-A: rejected
    REQUIRE(net.disconnectedPeers.size() == 1u);
    net.disconnectedPeers.clear();

    // IP-B (different IP) is unaffected
    net.peerAddresses[1] = "2.2.2.2:2000";
    broadcaster.onConnect(1u);
    CHECK(net.disconnectedPeers.empty());
}

TEST_CASE("WorldBroadcaster: null getPeerAddress does not crash rate limit", "[world_broadcaster][security]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setRateLimitParams(2, 10, 3);

    // peer 0 has no address entry — getPeerAddress returns nullptr
    broadcaster.onConnect(0u); // must not crash
    // peer was not disconnected (unknown IP skips rate-limit and allowlist checks)
    CHECK(net.disconnectedPeers.empty());
}

// ---------------------------------------------------------------------------
// Security: allowlist
// ---------------------------------------------------------------------------

TEST_CASE("WorldBroadcaster: empty allowlist allows all IPs", "[world_broadcaster][security]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    net.peerAddresses[0] = "9.9.9.9:1000";
    broadcaster.onConnect(0u);
    CHECK(net.disconnectedPeers.empty());
}

TEST_CASE("WorldBroadcaster: IP on allowlist is permitted", "[world_broadcaster][security]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setAllowedAddresses({"1.2.3.4"});

    net.peerAddresses[0] = "1.2.3.4:1000";
    broadcaster.onConnect(0u);
    CHECK(net.disconnectedPeers.empty());
}

TEST_CASE("WorldBroadcaster: IP not on allowlist is rejected", "[world_broadcaster][security]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setAllowedAddresses({"9.9.9.9"});

    net.peerAddresses[0] = "1.2.3.4:1000";
    broadcaster.onConnect(0u);
    REQUIRE(net.disconnectedPeers.size() == 1u);
    CHECK(net.disconnectedPeers[0] == 0u);
    auto ref = parseSendRefusal(net);
    CHECK(ref.msgId == static_cast<uint8_t>(fl::MsgId::ConnectRefusal));
    CHECK(std::string_view(ref.reason) == "Access denied.");
}

TEST_CASE("WorldBroadcaster: setting empty allowlist re-enables all IPs", "[world_broadcaster][security]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setAllowedAddresses({"9.9.9.9"});
    // Clear allowlist
    broadcaster.setAllowedAddresses({});

    net.peerAddresses[0] = "1.2.3.4:1000";
    broadcaster.onConnect(0u);
    CHECK(net.disconnectedPeers.empty());
}

TEST_CASE("WorldBroadcaster: banned IP rejected even if on allowlist", "[world_broadcaster][security]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.banAddress("1.2.3.4");
    broadcaster.setAllowedAddresses({"1.2.3.4"});

    net.peerAddresses[0] = "1.2.3.4:1000";
    broadcaster.onConnect(0u);
    REQUIRE(net.disconnectedPeers.size() == 1u);
}

// ---------------------------------------------------------------------------
// Security: per-IP concurrent connection limit
// ---------------------------------------------------------------------------

TEST_CASE("WorldBroadcaster: per-IP limit of zero allows unlimited connections", "[world_broadcaster][security]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    // limit=0 = unlimited; connect three peers from the same IP

    net.peerAddresses[0] = "1.2.3.4:1001";
    net.peerAddresses[1] = "1.2.3.4:1002";
    net.peerAddresses[2] = "1.2.3.4:1003";
    broadcaster.onConnect(0u);
    broadcaster.onConnect(1u);
    broadcaster.onConnect(2u);
    CHECK(net.disconnectedPeers.empty());
}

TEST_CASE("WorldBroadcaster: per-IP limit allows last connection at limit", "[world_broadcaster][security]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setMaxConnectionsPerIp(2);

    net.peerAddresses[0] = "1.2.3.4:1001";
    net.peerAddresses[1] = "1.2.3.4:1002";
    broadcaster.onConnect(0u);
    net.disconnectedPeers.clear();
    net.sends.clear();
    broadcaster.onConnect(1u); // count was 1, limit is 2 — allowed
    CHECK(net.disconnectedPeers.empty());
}

TEST_CASE("WorldBroadcaster: per-IP limit rejects connection over limit", "[world_broadcaster][security]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setMaxConnectionsPerIp(2);

    net.peerAddresses[0] = "1.2.3.4:1001";
    net.peerAddresses[1] = "1.2.3.4:1002";
    net.peerAddresses[2] = "1.2.3.4:1003";
    broadcaster.onConnect(0u);
    broadcaster.onConnect(1u);
    net.disconnectedPeers.clear();
    net.sends.clear();
    broadcaster.onConnect(2u); // count is 2, limit is 2 — rejected
    REQUIRE(net.disconnectedPeers.size() == 1u);
    CHECK(net.disconnectedPeers[0] == 2u);
    auto ref = parseSendRefusal(net);
    CHECK(ref.msgId == static_cast<uint8_t>(fl::MsgId::ConnectRefusal));
    CHECK(std::string_view(ref.reason) == "Too many connections from your address.");
}

TEST_CASE("WorldBroadcaster: per-IP limit counts only matching-IP peers", "[world_broadcaster][security]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setMaxConnectionsPerIp(2);

    net.peerAddresses[0] = "1.2.3.4:1001";
    net.peerAddresses[1] = "1.2.3.4:1002";
    net.peerAddresses[2] = "5.5.5.5:1001";
    net.peerAddresses[3] = "5.5.5.5:1002";
    broadcaster.onConnect(0u);
    broadcaster.onConnect(1u);
    broadcaster.onConnect(2u);
    broadcaster.onConnect(3u);
    CHECK(net.disconnectedPeers.empty());
}

TEST_CASE("WorldBroadcaster: null getPeerAddress does not crash per-IP limit check", "[world_broadcaster][security]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setMaxConnectionsPerIp(1);

    // peer 0 has no address entry → getPeerAddress returns nullptr
    broadcaster.onConnect(0u);
    CHECK(net.disconnectedPeers.empty());
}

TEST_CASE("WorldBroadcaster: per-IP limit slot freed after disconnect allows reconnect",
          "[world_broadcaster][security]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setMaxConnectionsPerIp(1);

    net.peerAddresses[0] = "1.2.3.4:1001";
    net.peerAddresses[1] = "1.2.3.4:1002";
    broadcaster.onConnect(0u);
    broadcaster.onDisconnect(0u); // frees the slot
    net.disconnectedPeers.clear();
    net.sends.clear();
    broadcaster.onConnect(1u); // count is now 0 — should be allowed
    CHECK(net.disconnectedPeers.empty());
}

TEST_CASE("WorldBroadcaster: per-IP limit counts IPv4-mapped IPv6 as same address", "[world_broadcaster][security]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setMaxConnectionsPerIp(1);

    net.peerAddresses[0] = "1.2.3.4:1001";
    net.peerAddresses[1] = "[::ffff:1.2.3.4]:1002"; // IPv4-mapped IPv6 — same host
    broadcaster.onConnect(0u);
    net.disconnectedPeers.clear();
    net.sends.clear();
    broadcaster.onConnect(1u); // normalizeIp maps ::ffff:1.2.3.4 → 1.2.3.4 → rejected
    REQUIRE(net.disconnectedPeers.size() == 1u);
    CHECK(net.disconnectedPeers[0] == 1u);
    auto ref = parseSendRefusal(net);
    CHECK(ref.msgId == static_cast<uint8_t>(fl::MsgId::ConnectRefusal));
    CHECK(std::string_view(ref.reason) == "Too many connections from your address.");
}

// ---------------------------------------------------------------------------
// Security: flood detection
// ---------------------------------------------------------------------------

TEST_CASE("WorldBroadcaster: peer within flood limit is not disconnected", "[world_broadcaster][security]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setRateLimitParams(100, 10, 2); // threshold = 120 packets/s

    fl::ManualClock t;
    broadcaster.setClock(t);

    net.peerAddresses[0] = "1.2.3.4:1000";
    broadcaster.onConnect(0u);

    auto pkt = makeClientInputPacket();
    for (int i = 0; i < 120; ++i) // exactly at threshold: not over
        broadcaster.onReceive(0u, pkt.data(), pkt.size());

    CHECK(net.disconnectedPeers.empty());
}

TEST_CASE("WorldBroadcaster: peer exceeding flood limit is disconnected", "[world_broadcaster][security]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setRateLimitParams(100, 10, 2); // threshold = 120 packets/s

    fl::ManualClock t;
    broadcaster.setClock(t);

    net.peerAddresses[0] = "1.2.3.4:1000";
    broadcaster.onConnect(0u);

    auto pkt = makeClientInputPacket();
    for (int i = 0; i < 121; ++i) // 121 > 120: over threshold
        broadcaster.onReceive(0u, pkt.data(), pkt.size());

    REQUIRE(net.disconnectedPeers.size() >= 1u);
    CHECK(net.disconnectedPeers[0] == 0u);
}

TEST_CASE("WorldBroadcaster: flood counter resets after 1s window", "[world_broadcaster][security]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setRateLimitParams(100, 10, 2); // threshold = 120

    fl::ManualClock t;
    broadcaster.setClock(t);

    net.peerAddresses[0] = "1.2.3.4:1000";
    broadcaster.onConnect(0u);

    auto pkt = makeClientInputPacket();
    // Send 120 (at limit)
    for (int i = 0; i < 120; ++i)
        broadcaster.onReceive(0u, pkt.data(), pkt.size());
    CHECK(net.disconnectedPeers.empty());

    // Advance past the 1s window, counter resets
    t.advance(std::chrono::seconds(2));
    // Send 120 more — still at limit in the new window
    for (int i = 0; i < 120; ++i)
        broadcaster.onReceive(0u, pkt.data(), pkt.size());
    CHECK(net.disconnectedPeers.empty());
}

TEST_CASE("WorldBroadcaster: non-ClientInput packets do not count toward flood", "[world_broadcaster][security]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setRateLimitParams(100, 10, 2); // threshold = 120

    fl::ManualClock t;
    broadcaster.setClock(t);

    net.peerAddresses[0] = "1.2.3.4:1000";
    broadcaster.onConnect(0u);

    // Send 500 packets with an unknown msgId — should not trigger flood
    uint8_t unknownMsg = 0xFF;
    for (int i = 0; i < 500; ++i)
        broadcaster.onReceive(0u, &unknownMsg, 1u);

    CHECK(net.disconnectedPeers.empty());
}

TEST_CASE("WorldBroadcaster: onDisconnect clears flood state", "[world_broadcaster][security]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setRateLimitParams(100, 10, 1); // threshold = 60

    fl::ManualClock t;
    broadcaster.setClock(t);

    net.peerAddresses[0] = "1.2.3.4:1000";
    broadcaster.onConnect(0u);

    // Fill flood state to 59 packets (just under threshold)
    auto pkt = makeClientInputPacket();
    for (int i = 0; i < 59; ++i)
        broadcaster.onReceive(0u, pkt.data(), pkt.size());

    // Disconnect + reconnect — flood state should be cleared
    broadcaster.onDisconnect(0u);
    net.sends.clear();
    broadcaster.onConnect(0u);

    // Should be able to send 60 packets in the new window without triggering flood
    for (int i = 0; i < 60; ++i)
        broadcaster.onReceive(0u, pkt.data(), pkt.size());
    CHECK(net.disconnectedPeers.empty());
}

// ---------------------------------------------------------------------------
// Security: ban set management
// ---------------------------------------------------------------------------

TEST_CASE("WorldBroadcaster: setBannedAddresses replaces existing ban set", "[world_broadcaster][security]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    broadcaster.banAddress("1.1.1.1");
    // Replace with a different set
    broadcaster.setBannedAddresses({"2.2.2.2"});

    // Old ban is gone — 1.1.1.1 can connect
    net.peerAddresses[0] = "1.1.1.1:1000";
    broadcaster.onConnect(0u);
    CHECK(net.disconnectedPeers.empty());

    // New ban is active — 2.2.2.2 is rejected
    net.peerAddresses[1] = "2.2.2.2:2000";
    broadcaster.onConnect(1u);
    REQUIRE(net.disconnectedPeers.size() == 1u);
}

TEST_CASE("WorldBroadcaster: getBannedAddresses returns current set", "[world_broadcaster][security]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    broadcaster.banAddress("1.2.3.4");
    broadcaster.banAddress("5.6.7.8");
    auto banned = broadcaster.getBannedAddresses();
    CHECK(banned.count("1.2.3.4") == 1u);
    CHECK(banned.count("5.6.7.8") == 1u);
    CHECK(banned.size() == 2u);
}

// ---------------------------------------------------------------------------
// Security: edge cases for branch coverage
// ---------------------------------------------------------------------------

TEST_CASE("WorldBroadcaster: onConnect null getPeerAddress skips allowlist and rate limit",
          "[world_broadcaster][security]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setAllowedAddresses({"9.9.9.9"});
    broadcaster.setRateLimitParams(1, 10, 3);

    // peer 0 has no address — getPeerAddress returns nullptr
    // With a non-empty allowlist, a known IP would be rejected.
    // But empty IP bypasses both allowlist and rate limit — no disconnect.
    broadcaster.onConnect(0u);
    CHECK(net.disconnectedPeers.empty());
}

TEST_CASE("WorldBroadcaster: rate limit prune preserves entries with unexpired timestamps",
          "[world_broadcaster][security]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setRateLimitParams(10, 5, 3);

    fl::ManualClock t;
    broadcaster.setClock(t);

    net.peerAddresses[0] = "1.2.3.4:1000";
    // One recent connect
    broadcaster.onConnect(0u);
    broadcaster.onDisconnect(0u);
    net.disconnectedPeers.clear();
    net.sends.clear();

    // Trigger prune by running 600 ticks (but clock hasn't advanced past window)
    for (int i = 0; i < 600; ++i)
        broadcaster.onTick(1.0 / 60.0, static_cast<uint64_t>(i));

    // Entry was NOT pruned (timestamp still in window) — reconnect should increment counter
    broadcaster.onConnect(0u); // 2nd connect — should succeed (limit is 10)
    CHECK(net.disconnectedPeers.empty());
}

TEST_CASE("WorldBroadcaster: rate limit prune removes fully expired entries", "[world_broadcaster][security]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setRateLimitParams(1, 5, 3);

    fl::ManualClock t;
    broadcaster.setClock(t);

    net.peerAddresses[0] = "1.2.3.4:1000";
    // One connect fills limit (limit=1)
    broadcaster.onConnect(0u);
    broadcaster.onDisconnect(0u);
    net.disconnectedPeers.clear();
    net.sends.clear();

    // Advance clock past window
    t.advance(std::chrono::seconds(6));

    // Run 600 ticks to trigger prune (timestamps now all expired)
    for (int i = 0; i < 600; ++i)
        broadcaster.onTick(1.0 / 60.0, static_cast<uint64_t>(i));

    // After prune, IP-A can connect again (counter reset by prune)
    broadcaster.onConnect(0u);
    CHECK(net.disconnectedPeers.empty());
}

// ---------------------------------------------------------------------------
// Shutdown countdown tests
// ---------------------------------------------------------------------------

// Find the first MsgServerNotice in broadcasts from index `from`.
static bool findNotice(const std::vector<std::vector<uint8_t>>& broadcasts, std::size_t from,
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

TEST_CASE("WorldBroadcaster: initiateShutdown broadcasts first notice on next tick", "[world_broadcaster][shutdown]") {
    MockNetwork net;
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    fl::ManualClock t;
    broadcaster.setClock(t);
    broadcaster.initiateShutdown(30, 5);

    CHECK(broadcaster.isShuttingDown());
    CHECK(broadcaster.secondsUntilShutdown() <= 30u);

    broadcaster.onTick(1.0 / 60.0, 0u);

    fl::MsgServerNotice notice{};
    REQUIRE(findNotice(net.broadcasts, 0, notice));
    CHECK(notice.secondsRemaining > 0u);
    CHECK(notice.secondsRemaining <= 30u);
    CHECK(notice.text[0] != '\0');
}

TEST_CASE("WorldBroadcaster: no notice broadcast when interval not reached", "[world_broadcaster][shutdown]") {
    MockNetwork net;
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    fl::ManualClock t;
    broadcaster.setClock(t);
    broadcaster.initiateShutdown(60, 30); // 30s interval

    // First tick fires first notice.
    broadcaster.onTick(1.0 / 60.0, 0u);
    std::size_t noticesAfterFirst = 0;
    for (const auto& pkt : net.broadcasts)
        if (pkt.size() == sizeof(fl::MsgServerNotice) && pkt[0] == static_cast<uint8_t>(fl::MsgId::ServerNotice))
            ++noticesAfterFirst;
    REQUIRE(noticesAfterFirst == 1u);

    // Advance only 10s (halfway through 30s interval) — no new notice expected.
    t.advance(std::chrono::seconds(10));
    net.broadcasts.clear();
    broadcaster.onTick(1.0 / 60.0, 1u);

    std::size_t newNotices = 0;
    for (const auto& pkt : net.broadcasts)
        if (pkt.size() == sizeof(fl::MsgServerNotice) && pkt[0] == static_cast<uint8_t>(fl::MsgId::ServerNotice))
            ++newNotices;
    CHECK(newNotices == 0u);
}

TEST_CASE("WorldBroadcaster: cancelShutdown stops countdown", "[world_broadcaster][shutdown]") {
    MockNetwork net;
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    fl::ManualClock t;
    broadcaster.setClock(t);
    broadcaster.initiateShutdown(60, 30);
    broadcaster.onTick(1.0 / 60.0, 0u);
    broadcaster.cancelShutdown();
    CHECK(!broadcaster.isShuttingDown());

    net.broadcasts.clear();
    t.advance(std::chrono::seconds(70)); // past original shutdown time
    broadcaster.onTick(1.0 / 60.0, 1u);

    fl::MsgServerNotice notice{};
    CHECK(!findNotice(net.broadcasts, 0, notice));
}

TEST_CASE("WorldBroadcaster: extendShutdown pushes back and fires immediate notice", "[world_broadcaster][shutdown]") {
    MockNetwork net;
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    fl::ManualClock t;
    broadcaster.setClock(t);
    broadcaster.initiateShutdown(30, 5);
    broadcaster.onTick(1.0 / 60.0, 0u);

    t.advance(std::chrono::seconds(20));
    net.broadcasts.clear();

    REQUIRE(broadcaster.extendShutdown(60u));

    broadcaster.onTick(1.0 / 60.0, 1u);
    fl::MsgServerNotice notice{};
    REQUIRE(findNotice(net.broadcasts, 0, notice));
    CHECK(notice.secondsRemaining > 50u); // ~70s remaining after extension
}

TEST_CASE("WorldBroadcaster: extendShutdown returns false when not shutting down", "[world_broadcaster][shutdown]") {
    MockNetwork net;
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    CHECK(!broadcaster.extendShutdown(60u));
}

TEST_CASE("WorldBroadcaster: shutdown callback fires at T=0", "[world_broadcaster][shutdown]") {
    MockNetwork net;
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    bool called = false;
    broadcaster.setShutdownCallback([&called]() { called = true; });

    fl::ManualClock t;
    broadcaster.setClock(t);
    broadcaster.initiateShutdown(10, 5);

    t.advance(std::chrono::seconds(11));
    broadcaster.onTick(1.0 / 60.0, 0u);

    CHECK(called);
    CHECK(!broadcaster.isShuttingDown());
    CHECK(broadcaster.secondsUntilShutdown() == 0u);
}

TEST_CASE("WorldBroadcaster: T=0 fires without crash when no callback set", "[world_broadcaster][shutdown]") {
    MockNetwork net;
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    fl::ManualClock t;
    broadcaster.setClock(t);
    broadcaster.initiateShutdown(0, 0);

    broadcaster.onTick(1.0 / 60.0, 0u);

    fl::MsgServerNotice notice{};
    REQUIRE(findNotice(net.broadcasts, 0, notice));
    CHECK(notice.secondsRemaining == 0u);
    CHECK(!broadcaster.isShuttingDown());
}

TEST_CASE("WorldBroadcaster: initiateShutdown delay=0 fires on very next tick", "[world_broadcaster][shutdown]") {
    MockNetwork net;
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    bool called = false;
    broadcaster.setShutdownCallback([&called]() { called = true; });

    fl::ManualClock t;
    broadcaster.setClock(t);
    broadcaster.initiateShutdown(0, 0);

    broadcaster.onTick(1.0 / 60.0, 0u);

    fl::MsgServerNotice notice{};
    REQUIRE(findNotice(net.broadcasts, 0, notice));
    CHECK(notice.secondsRemaining == 0u);
    CHECK(called);
}

TEST_CASE("WorldBroadcaster: T-60 notice always fires with 5-min interval", "[world_broadcaster][shutdown]") {
    MockNetwork net;
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    fl::ManualClock t;
    broadcaster.setClock(t);
    broadcaster.initiateShutdown(600, 300); // 10 min, 5-min interval

    broadcaster.onTick(1.0 / 60.0, 0u); // first notice at T-600s

    // Advance to T-61s (would skip T-60s without the clamp logic).
    t.advance(std::chrono::seconds(539));
    net.broadcasts.clear();
    broadcaster.onTick(1.0 / 60.0, 1u);

    fl::MsgServerNotice notice{};
    REQUIRE(findNotice(net.broadcasts, 0, notice));
    CHECK(notice.secondsRemaining <= 62u);
    CHECK(notice.secondsRemaining >= 58u);
}

TEST_CASE("WorldBroadcaster: notice text contains hours for delays >= 3600s", "[world_broadcaster][shutdown]") {
    MockNetwork net;
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    fl::ManualClock t;
    broadcaster.setClock(t);
    broadcaster.initiateShutdown(7200, 3600);

    broadcaster.onTick(1.0 / 60.0, 0u);

    fl::MsgServerNotice notice{};
    REQUIRE(findNotice(net.broadcasts, 0, notice));
    CHECK(std::string(notice.text).find("hour") != std::string::npos);
}

TEST_CASE("WorldBroadcaster: notice text contains minutes for delays 61-3599s", "[world_broadcaster][shutdown]") {
    MockNetwork net;
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    fl::ManualClock t;
    broadcaster.setClock(t);
    broadcaster.initiateShutdown(300, 60);

    broadcaster.onTick(1.0 / 60.0, 0u);

    fl::MsgServerNotice notice{};
    REQUIRE(findNotice(net.broadcasts, 0, notice));
    CHECK(std::string(notice.text).find("minute") != std::string::npos);
}

TEST_CASE("WorldBroadcaster: notice text uses final-minute wording at T-60s", "[world_broadcaster][shutdown]") {
    MockNetwork net;
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    fl::ManualClock t;
    broadcaster.setClock(t);
    broadcaster.initiateShutdown(300, 60);

    t.advance(std::chrono::seconds(245)); // T-55s remaining
    broadcaster.onTick(1.0 / 60.0, 0u);

    fl::MsgServerNotice notice{};
    REQUIRE(findNotice(net.broadcasts, 0, notice));
    CHECK(std::string(notice.text).find("1 minute") != std::string::npos);
}

TEST_CASE("WorldBroadcaster: initiateShutdown with reason includes reason in countdown notice",
          "[world_broadcaster][shutdown]") {
    MockNetwork net;
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    fl::ManualClock t;
    broadcaster.setClock(t);
    broadcaster.initiateShutdown(300, 5, "Server restarting");

    broadcaster.onTick(1.0 / 60.0, 0u);

    fl::MsgServerNotice notice{};
    REQUIRE(findNotice(net.broadcasts, 0, notice));
    std::string text(notice.text);
    CHECK(text.find("Server restarting") != std::string::npos);
    CHECK(text.find("minutes") != std::string::npos);
}

TEST_CASE("WorldBroadcaster: initiateShutdown with reason uses short format for secsLeft at most 60",
          "[world_broadcaster][shutdown]") {
    MockNetwork net;
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    fl::ManualClock t;
    broadcaster.setClock(t);
    broadcaster.initiateShutdown(30, 5, "Server restarting");

    broadcaster.onTick(1.0 / 60.0, 0u);

    fl::MsgServerNotice notice{};
    REQUIRE(findNotice(net.broadcasts, 0, notice));
    std::string text(notice.text);
    CHECK(text.find("Server restarting") != std::string::npos);
    CHECK(text.find("1 minute") != std::string::npos);
    CHECK(text.find("save your progress") == std::string::npos);
}

TEST_CASE("WorldBroadcaster: initiateShutdown with reason includes reason in T=0 notice",
          "[world_broadcaster][shutdown]") {
    MockNetwork net;
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    fl::ManualClock t;
    broadcaster.setClock(t);
    broadcaster.initiateShutdown(0, 5, "Server restarting");

    broadcaster.onTick(1.0 / 60.0, 0u);

    fl::MsgServerNotice notice{};
    REQUIRE(findNotice(net.broadcasts, 0, notice));
    CHECK(notice.secondsRemaining == 0u);
    CHECK(std::string(notice.text).find("Server restarting") != std::string::npos);
}

TEST_CASE("WorldBroadcaster: long reason is safely truncated to fit MsgServerNotice text",
          "[world_broadcaster][shutdown]") {
    MockNetwork net;
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    fl::ManualClock t;
    broadcaster.setClock(t);
    std::string longReason(60, 'X');
    broadcaster.initiateShutdown(300, 5, longReason);

    broadcaster.onTick(1.0 / 60.0, 0u);

    fl::MsgServerNotice notice{};
    REQUIRE(findNotice(net.broadcasts, 0, notice));
    CHECK(std::strlen(notice.text) < sizeof(notice.text));
}

TEST_CASE("WorldBroadcaster: cancelShutdown clears reason so subsequent shutdown uses default text",
          "[world_broadcaster][shutdown]") {
    MockNetwork net;
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    fl::ManualClock t;
    broadcaster.setClock(t);
    broadcaster.initiateShutdown(30, 5, "reason text");
    broadcaster.cancelShutdown();
    broadcaster.initiateShutdown(30, 5);

    broadcaster.onTick(1.0 / 60.0, 0u);

    fl::MsgServerNotice notice{};
    REQUIRE(findNotice(net.broadcasts, 0, notice));
    std::string text(notice.text);
    CHECK(text.find("reason text") == std::string::npos);
    CHECK(text.find("Server shutting down") != std::string::npos);
}

TEST_CASE("WorldBroadcaster: extendShutdown preserves reason in subsequent notices", "[world_broadcaster][shutdown]") {
    MockNetwork net;
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    fl::ManualClock t;
    broadcaster.setClock(t);
    broadcaster.initiateShutdown(120, 60, "Server restarting");

    // First tick fires first notice.
    broadcaster.onTick(1.0 / 60.0, 0u);
    std::size_t firstNoticeIdx = net.broadcasts.size() - 1;

    // Advance 60s to reach next notice interval then extend.
    t.advance(std::chrono::seconds(60));
    broadcaster.extendShutdown(60);
    net.broadcasts.clear();
    broadcaster.onTick(1.0 / 60.0, 1u);

    fl::MsgServerNotice notice{};
    REQUIRE(findNotice(net.broadcasts, 0, notice));
    CHECK(std::string(notice.text).find("Server restarting") != std::string::npos);
    (void)firstNoticeIdx;
}

// ---------------------------------------------------------------------------
// MsgAdminCommand / MsgAdminResponse tests
// ---------------------------------------------------------------------------

namespace {

// Build a MsgAdminCommand packet with the given token and command strings.
static std::vector<uint8_t> makeAdminCmd(const char* token, const char* command, uint16_t reqId = 0x0042u) {
    fl::MsgAdminCommand msg{};
    msg.msgId = static_cast<uint8_t>(fl::MsgId::AdminCommand);
    msg.reqId = reqId;
    std::snprintf(msg.token, sizeof(msg.token), "%s", token);
    std::snprintf(msg.command, sizeof(msg.command), "%s", command);
    return {reinterpret_cast<const uint8_t*>(&msg), reinterpret_cast<const uint8_t*>(&msg) + sizeof(msg)};
}

// Minimal mock for CommandShell mark/drainSince injection. Pre-load `lines` before
// advancing the tick; drainSince returns whatever is in `lines` regardless of the mark.
struct ShellDrainMock {
    std::vector<std::string> lines;
    int mark() const {
        return 0;
    }
    std::vector<std::string> drainSince(int /*mark*/) const {
        return lines;
    }
};

// Collect all MsgAdminResponse and MsgAdminResponseChunk packets from net.sends that
// arrive at or after index `afterIdx`, filtered by msgId. Used to isolate deferred drain
// output from the synchronous ack and WorldSnapshot sends that onTick also emits.
static std::vector<std::vector<uint8_t>> drainSends(const MockNetwork& net, std::size_t afterIdx) {
    std::vector<std::vector<uint8_t>> result;
    for (std::size_t i = afterIdx; i < net.sends.size(); ++i) {
        const auto& pkt = net.sends[i];
        if (pkt.empty())
            continue;
        uint8_t id = pkt[0];
        if (id == static_cast<uint8_t>(fl::MsgId::AdminResponse) ||
            id == static_cast<uint8_t>(fl::MsgId::AdminResponseChunk))
            result.push_back(pkt);
    }
    return result;
}

// Return true if the last entry in net.sends is a MsgAdminResponseChunk; populate chunk.
static bool parseLastChunk(const MockNetwork& net, fl::MsgAdminResponseChunk& chunk) {
    if (net.sends.empty())
        return false;
    const auto& last = net.sends.back();
    if (last.size() != sizeof(fl::MsgAdminResponseChunk))
        return false;
    std::memcpy(&chunk, last.data(), sizeof(chunk));
    return chunk.msgId == static_cast<uint8_t>(fl::MsgId::AdminResponseChunk);
}

// ---------------------------------------------------------------------------
// MOTD helpers
// ---------------------------------------------------------------------------

static std::string parseMotdText(const std::vector<uint8_t>& pkt) {
    if (pkt.size() < sizeof(fl::MsgMotdHeader) + 1u || pkt[0] != static_cast<uint8_t>(fl::MsgId::Motd))
        return {};
    // exclude the 4-byte MsgMotdHeader and the trailing NUL.
    return std::string(reinterpret_cast<const char*>(pkt.data() + sizeof(fl::MsgMotdHeader)),
                       pkt.size() - sizeof(fl::MsgMotdHeader) - 1u);
}

TEST_CASE("WorldBroadcaster: no MOTD sent by default", "[world_broadcaster][motd]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    // setMotd NOT called

    broadcaster.onConnect(0u);

    // Hello + ConnectAck; no MOTD packet
    CHECK(net.sends.size() == 2u);
    for (const auto& pkt : net.sends)
        CHECK(pkt[0] != static_cast<uint8_t>(fl::MsgId::Motd));
}

TEST_CASE("WorldBroadcaster: MOTD sent as third send when non-empty", "[world_broadcaster][motd]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    broadcaster.setMotd("Welcome!");

    broadcaster.onConnect(0u);

    REQUIRE(net.sends.size() == 3u);
    CHECK(net.sends[2][0] == static_cast<uint8_t>(fl::MsgId::Motd));
    CHECK(parseMotdText(net.sends[2]) == "Welcome!");
}

TEST_CASE("WorldBroadcaster: oversized MOTD capped at kMaxMotdBytes", "[world_broadcaster][motd]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    broadcaster.setMotd(std::string(fl::kMaxMotdBytes + 500, 'A'));

    broadcaster.onConnect(0u);

    REQUIRE(net.sends.size() == 3u);
    // sizeof(MsgMotdHeader) (4) + kMaxMotdBytes (text) + 1 (NUL)
    CHECK(net.sends[2].size() == sizeof(fl::MsgMotdHeader) + fl::kMaxMotdBytes + 1u);
    CHECK(net.sends[2].back() == 0u); // NUL terminator
}

TEST_CASE("WorldBroadcaster: setMotd with empty string suppresses MOTD send", "[world_broadcaster][motd]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    broadcaster.setMotd("hello");
    broadcaster.setMotd(""); // cleared

    broadcaster.onConnect(0u);

    CHECK(net.sends.size() == 2u);
    for (const auto& pkt : net.sends)
        CHECK(pkt[0] != static_cast<uint8_t>(fl::MsgId::Motd));
}

TEST_CASE("WorldBroadcaster: MOTD displaySeconds is 0 by default", "[world_broadcaster][motd]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    broadcaster.setMotd("Welcome!");

    broadcaster.onConnect(0u);

    REQUIRE(net.sends.size() == 3u);
    REQUIRE(net.sends[2].size() >= sizeof(fl::MsgMotdHeader));
    uint16_t secs = 0;
    std::memcpy(&secs, net.sends[2].data() + offsetof(fl::MsgMotdHeader, displaySeconds), sizeof(secs));
    CHECK(secs == 0u);
}

TEST_CASE("WorldBroadcaster: MOTD packet displaySeconds matches setMotdDisplaySeconds", "[world_broadcaster][motd]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    broadcaster.setMotd("Welcome!");
    broadcaster.setMotdDisplaySeconds(45u);

    broadcaster.onConnect(0u);

    REQUIRE(net.sends.size() == 3u);
    REQUIRE(net.sends[2].size() >= sizeof(fl::MsgMotdHeader));
    uint16_t secs = 0;
    std::memcpy(&secs, net.sends[2].data() + offsetof(fl::MsgMotdHeader, displaySeconds), sizeof(secs));
    CHECK(secs == 45u);
}

TEST_CASE("WorldBroadcaster: applyConfig wires MOTD and display seconds in one call", "[world_broadcaster][motd]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);

    fl::WorldBroadcasterConfig cfg;
    cfg.motd = "Welcome via config!";
    cfg.motdDisplaySeconds = 30u;
    broadcaster.applyConfig(cfg);

    broadcaster.onConnect(0u);

    REQUIRE(net.sends.size() == 3u);
    CHECK(net.sends[2][0] == static_cast<uint8_t>(fl::MsgId::Motd));
    CHECK(parseMotdText(net.sends[2]) == "Welcome via config!");
    uint16_t secs = 0;
    std::memcpy(&secs, net.sends[2].data() + offsetof(fl::MsgMotdHeader, displaySeconds), sizeof(secs));
    CHECK(secs == 30u);
}

// ---------------------------------------------------------------------------
// Admin command helpers (existing)
// ---------------------------------------------------------------------------

// Return true if the last entry in net.sends is a MsgAdminResponse; populate resp.
static bool parseLastAdminResponse(const MockNetwork& net, fl::MsgAdminResponse& resp) {
    if (net.sends.empty())
        return false;
    const auto& last = net.sends.back();
    if (last.size() != sizeof(fl::MsgAdminResponse))
        return false;
    std::memcpy(&resp, last.data(), sizeof(resp));
    return resp.msgId == static_cast<uint8_t>(fl::MsgId::AdminResponse);
}

} // namespace

TEST_CASE("WorldBroadcaster: MsgAdminCommand discarded when no dispatcher set", "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    broadcaster.setOperatorPassword("secret"); // dispatcher NOT set

    broadcaster.onConnect(0u);
    net.sends.clear();

    auto pkt = makeAdminCmd("secret", "status");
    broadcaster.onReceive(0u, pkt.data(), pkt.size());

    CHECK(net.sends.empty());
}

TEST_CASE("WorldBroadcaster: MsgAdminCommand discarded when no password configured",
          "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    broadcaster.setAdminDispatch([](std::string_view) -> std::string { return "pong"; }); // password NOT set

    broadcaster.onConnect(0u);
    net.sends.clear();

    auto pkt = makeAdminCmd("", "status");
    broadcaster.onReceive(0u, pkt.data(), pkt.size());

    CHECK(net.sends.empty());
}

TEST_CASE("WorldBroadcaster: MsgAdminCommand discarded on wrong token", "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    broadcaster.setOperatorPassword("secret");
    broadcaster.setAdminDispatch([](std::string_view) -> std::string { return "pong"; });

    broadcaster.onConnect(0u);
    net.sends.clear();

    auto pkt = makeAdminCmd("wrongpass", "status");
    broadcaster.onReceive(0u, pkt.data(), pkt.size());

    CHECK(net.sends.empty());
}

TEST_CASE("WorldBroadcaster: MsgAdminCommand dispatches on correct token and sends MsgAdminResponse",
          "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    broadcaster.setOperatorPassword("secret");
    broadcaster.setAdminDispatch([](std::string_view cmd) -> std::string {
        if (cmd == "ping")
            return "pong";
        return "";
    });

    broadcaster.onConnect(0u);
    net.sends.clear();

    auto pkt = makeAdminCmd("secret", "ping");
    broadcaster.onReceive(0u, pkt.data(), pkt.size());

    REQUIRE(net.sends.size() == 1u);
    fl::MsgAdminResponse resp{};
    REQUIRE(parseLastAdminResponse(net, resp));
    CHECK(std::string(resp.text) == "pong");
    CHECK(net.sendReliable);
}

TEST_CASE("WorldBroadcaster: MsgAdminCommand discarded if packet too small", "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    broadcaster.setOperatorPassword("secret");
    broadcaster.setAdminDispatch([](std::string_view) -> std::string { return "pong"; });

    broadcaster.onConnect(0u);
    net.sends.clear();

    // Send only the msgId byte + 3 padding — well under sizeof(MsgAdminCommand).
    uint8_t tiny[4] = {static_cast<uint8_t>(fl::MsgId::AdminCommand), 0, 0, 0};
    broadcaster.onReceive(0u, tiny, sizeof(tiny));

    CHECK(net.sends.empty());
}

TEST_CASE("WorldBroadcaster: MsgAdminCommand token without null terminator fails auth",
          "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    broadcaster.setOperatorPassword("secret");
    broadcaster.setAdminDispatch([](std::string_view) -> std::string { return "pong"; });

    broadcaster.onConnect(0u);
    net.sends.clear();

    // Fill entire token field with 'x' (no null byte) — auth must fail, no crash.
    fl::MsgAdminCommand msg{};
    msg.msgId = static_cast<uint8_t>(fl::MsgId::AdminCommand);
    std::memset(msg.token, 'x', sizeof(msg.token));
    std::snprintf(msg.command, sizeof(msg.command), "ping");
    auto pkt = std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(&msg),
                                    reinterpret_cast<const uint8_t*>(&msg) + sizeof(msg));
    broadcaster.onReceive(0u, pkt.data(), pkt.size());

    CHECK(net.sends.empty());
}

TEST_CASE("WorldBroadcaster: MsgAdminCommand with empty command string is discarded",
          "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    broadcaster.setOperatorPassword("secret");
    broadcaster.setAdminDispatch([](std::string_view) -> std::string { return "should not be called"; });

    broadcaster.onConnect(0u);
    net.sends.clear();

    // Valid token, but command field is all zeros (empty after null-term).
    fl::MsgAdminCommand msg{};
    msg.msgId = static_cast<uint8_t>(fl::MsgId::AdminCommand);
    std::snprintf(msg.token, sizeof(msg.token), "secret");
    // command left zero-initialized — empty string
    auto pkt = std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(&msg),
                                    reinterpret_cast<const uint8_t*>(&msg) + sizeof(msg));
    broadcaster.onReceive(0u, pkt.data(), pkt.size());

    CHECK(net.sends.empty());
}

TEST_CASE("WorldBroadcaster: MsgAdminCommand empty dispatcher result still sends MsgAdminResponse",
          "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    broadcaster.setOperatorPassword("secret");
    // Dispatcher returns empty string (e.g. fire-and-forget command).
    broadcaster.setAdminDispatch([](std::string_view) -> std::string { return ""; });

    broadcaster.onConnect(0u);
    net.sends.clear();

    auto pkt = makeAdminCmd("secret", "spawn");
    broadcaster.onReceive(0u, pkt.data(), pkt.size());

    // Server always sends a response; client-side filters empty text before printing.
    REQUIRE(net.sends.size() == 1u);
    fl::MsgAdminResponse resp{};
    REQUIRE(parseLastAdminResponse(net, resp));
    CHECK(resp.text[0] == '\0');
}

TEST_CASE("WorldBroadcaster: MsgAdminCommand result >123 chars streams as MsgAdminResponseChunk",
          "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    broadcaster.setOperatorPassword("secret");
    broadcaster.setAdminDispatch([](std::string_view) -> std::string { return std::string(200, 'x'); });

    broadcaster.onConnect(0u);
    net.sends.clear();

    auto pkt = makeAdminCmd("secret", "peers");
    broadcaster.onReceive(0u, pkt.data(), pkt.size());

    REQUIRE(net.sends.size() == 1u);
    fl::MsgAdminResponseChunk chunk{};
    REQUIRE(parseLastChunk(net, chunk));
    CHECK((chunk.flags & fl::kChunkFlagEnd) != 0u);
    CHECK(std::strlen(chunk.body) == 200u);
}

TEST_CASE("WorldBroadcaster: sendAdminResponse fast-path for result <=123 chars",
          "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    broadcaster.setOperatorPassword("secret");
    broadcaster.setAdminDispatch([](std::string_view) -> std::string { return std::string(50, 'a'); });

    broadcaster.onConnect(0u);
    net.sends.clear();

    auto pkt = makeAdminCmd("secret", "status");
    broadcaster.onReceive(0u, pkt.data(), pkt.size());

    REQUIRE(net.sends.size() == 1u);
    fl::MsgAdminResponse resp{};
    REQUIRE(parseLastAdminResponse(net, resp));
    CHECK(std::strlen(resp.text) == 50u);
}

TEST_CASE("WorldBroadcaster: sendAdminResponse fast-path at exactly 123 chars", "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    broadcaster.setOperatorPassword("secret");
    broadcaster.setAdminDispatch([](std::string_view) -> std::string { return std::string(123, 'x'); });

    broadcaster.onConnect(0u);
    net.sends.clear();

    auto pkt = makeAdminCmd("secret", "help");
    broadcaster.onReceive(0u, pkt.data(), pkt.size());

    REQUIRE(net.sends.size() == 1u);
    fl::MsgAdminResponse resp{};
    REQUIRE(parseLastAdminResponse(net, resp));
    CHECK(std::strlen(resp.text) == 123u);
}

TEST_CASE("WorldBroadcaster: sendAdminResponse echoes reqId in MsgAdminResponse",
          "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    broadcaster.setOperatorPassword("secret");
    broadcaster.setAdminDispatch([](std::string_view) -> std::string { return "ok"; });

    broadcaster.onConnect(0u);
    net.sends.clear();

    auto pkt = makeAdminCmd("secret", "ping", 0xBEEFu);
    broadcaster.onReceive(0u, pkt.data(), pkt.size());

    REQUIRE(net.sends.size() == 1u);
    fl::MsgAdminResponse resp{};
    REQUIRE(parseLastAdminResponse(net, resp));
    CHECK(resp.reqId == 0xBEEFu);
}

TEST_CASE("WorldBroadcaster: sendAdminResponse 124-char result sends one chunk with kChunkFlagEnd",
          "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    broadcaster.setOperatorPassword("secret");
    broadcaster.setAdminDispatch([](std::string_view) -> std::string { return std::string(124, 'y'); });

    broadcaster.onConnect(0u);
    net.sends.clear();

    auto pkt = makeAdminCmd("secret", "help");
    broadcaster.onReceive(0u, pkt.data(), pkt.size());

    REQUIRE(net.sends.size() == 1u);
    fl::MsgAdminResponseChunk chunk{};
    REQUIRE(parseLastChunk(net, chunk));
    CHECK(chunk.seqNum == 0u);
    CHECK((chunk.flags & fl::kChunkFlagEnd) != 0u);
    CHECK(std::strlen(chunk.body) == 124u);
}

TEST_CASE("WorldBroadcaster: sendAdminResponse >505 chars sends two chunks", "[world_broadcaster][admin_command]") {
    const std::string longResult(506, 'z');

    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    broadcaster.setOperatorPassword("secret");
    broadcaster.setAdminDispatch([&](std::string_view) -> std::string { return longResult; });

    broadcaster.onConnect(0u);
    net.sends.clear();

    auto pkt = makeAdminCmd("secret", "peers");
    broadcaster.onReceive(0u, pkt.data(), pkt.size());

    REQUIRE(net.sends.size() == 2u);

    fl::MsgAdminResponseChunk c0{}, c1{};
    std::memcpy(&c0, net.sends[0].data(), sizeof(c0));
    std::memcpy(&c1, net.sends[1].data(), sizeof(c1));

    CHECK(c0.msgId == static_cast<uint8_t>(fl::MsgId::AdminResponseChunk));
    CHECK(c0.seqNum == 0u);
    CHECK((c0.flags & fl::kChunkFlagEnd) == 0u); // not the final chunk
    CHECK(c1.seqNum == 1u);
    CHECK((c1.flags & fl::kChunkFlagEnd) != 0u); // final chunk

    std::string assembled = std::string(c0.body) + std::string(c1.body);
    CHECK(assembled == longResult);
}

TEST_CASE("WorldBroadcaster: sendAdminResponse echoes reqId in every MsgAdminResponseChunk",
          "[world_broadcaster][admin_command]") {
    const std::string longResult(506, 'q');

    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    broadcaster.setOperatorPassword("secret");
    broadcaster.setAdminDispatch([&](std::string_view) -> std::string { return longResult; });

    broadcaster.onConnect(0u);
    net.sends.clear();

    auto pkt = makeAdminCmd("secret", "peers", 0x1111u);
    broadcaster.onReceive(0u, pkt.data(), pkt.size());

    REQUIRE(net.sends.size() == 2u);
    for (const auto& send : net.sends) {
        REQUIRE(send.size() == sizeof(fl::MsgAdminResponseChunk));
        fl::MsgAdminResponseChunk chunk{};
        std::memcpy(&chunk, send.data(), sizeof(chunk));
        CHECK(chunk.reqId == 0x1111u);
    }
}

// ---------------------------------------------------------------------------
// Admin auth lockout tests
// ---------------------------------------------------------------------------

// Shared setup helper: broadcaster with password "pw", dispatch noop, 3-failure
// threshold, 60 s lockout, injectable clock, and peer 0 connected from 1.2.3.4.
static void setupAuthFixture(fl::WorldBroadcaster& broadcaster, MockNetwork& net, fl::ManualClock& now) {
    broadcaster.setOperatorPassword("pw");
    broadcaster.setAdminDispatch([](std::string_view) -> std::string { return "ok"; });
    broadcaster.setAdminAuthParams(3, 60);
    broadcaster.setClock(now);
    broadcaster.onConnect(0u);
    net.sends.clear();
    net.disconnectedPeers.clear();
}

TEST_CASE("WorldBroadcaster: admin auth no lockout before threshold", "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    fl::ManualClock now;
    setupAuthFixture(broadcaster, net, now);

    // N-1 = 2 failures; peer must remain connected after each
    for (int i = 0; i < 2; ++i) {
        auto pkt = makeAdminCmd("wrongpass", "status");
        broadcaster.onReceive(0u, pkt.data(), pkt.size());
        CHECK(net.disconnectedPeers.empty());
    }
}

TEST_CASE("WorldBroadcaster: admin auth lockout triggered on Nth failure -- peer kicked",
          "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    fl::ManualClock now;
    setupAuthFixture(broadcaster, net, now);

    // 2 failures — no kick
    for (int i = 0; i < 2; ++i) {
        auto pkt = makeAdminCmd("wrongpass", "status");
        broadcaster.onReceive(0u, pkt.data(), pkt.size());
    }
    CHECK(net.disconnectedPeers.empty());

    // 3rd failure — lockout: peer kicked
    auto pkt = makeAdminCmd("wrongpass", "status");
    broadcaster.onReceive(0u, pkt.data(), pkt.size());
    REQUIRE(net.disconnectedPeers.size() == 1u);
    CHECK(net.disconnectedPeers[0] == 0u);
}

TEST_CASE("WorldBroadcaster: admin auth onConnect refused while locked", "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    net.peerAddresses[1] = "1.2.3.4:5678"; // same IP, new port (reconnect)
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    fl::ManualClock now;
    setupAuthFixture(broadcaster, net, now);

    // Trigger lockout on peer 0
    for (int i = 0; i < 3; ++i) {
        auto pkt = makeAdminCmd("wrongpass", "status");
        broadcaster.onReceive(0u, pkt.data(), pkt.size());
    }
    net.disconnectedPeers.clear();
    net.sends.clear();

    // Reconnect attempt from same IP — must be refused
    broadcaster.onConnect(1u);
    REQUIRE(net.disconnectedPeers.size() == 1u);
    CHECK(net.disconnectedPeers[0] == 1u);
    auto ref = parseSendRefusal(net);
    CHECK(ref.msgId == static_cast<uint8_t>(fl::MsgId::ConnectRefusal));
    CHECK(std::string_view(ref.reason) == "Access denied.");
}

TEST_CASE("WorldBroadcaster: admin auth lockout expires after TTL", "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    net.peerAddresses[1] = "1.2.3.4:5678";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    fl::ManualClock now;
    setupAuthFixture(broadcaster, net, now);

    // Trigger lockout
    for (int i = 0; i < 3; ++i) {
        auto pkt = makeAdminCmd("wrongpass", "status");
        broadcaster.onReceive(0u, pkt.data(), pkt.size());
    }
    net.disconnectedPeers.clear();
    net.sends.clear();

    // Advance clock past 60 s TTL
    now.advance(std::chrono::seconds(61));

    // Reconnect — should succeed (MsgHello sent, not in disconnectedPeers)
    broadcaster.onConnect(1u);
    CHECK(net.disconnectedPeers.empty());
    REQUIRE(!net.sends.empty());
    fl::MsgHello hello{};
    std::memcpy(&hello, net.sends.front().data(), sizeof(hello));
    CHECK(hello.msgId == static_cast<uint8_t>(fl::MsgId::Hello));
}

TEST_CASE("WorldBroadcaster: admin auth per-IP isolation", "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234"; // IP A
    net.peerAddresses[1] = "5.6.7.8:2222"; // IP B — different
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    fl::ManualClock now;
    setupAuthFixture(broadcaster, net, now);

    // Connect peer 1 from IP B
    broadcaster.onConnect(1u);
    net.sends.clear();
    net.disconnectedPeers.clear();

    // Lock out IP A (3 wrong tokens on peer 0)
    for (int i = 0; i < 3; ++i) {
        auto pkt = makeAdminCmd("wrongpass", "status");
        broadcaster.onReceive(0u, pkt.data(), pkt.size());
    }
    net.disconnectedPeers.clear();
    net.sends.clear();

    // IP B can still dispatch successfully
    auto pkt = makeAdminCmd("pw", "status");
    broadcaster.onReceive(1u, pkt.data(), pkt.size());
    CHECK(net.disconnectedPeers.empty());
    REQUIRE(net.sends.size() == 1u);
    fl::MsgAdminResponse resp{};
    REQUIRE(parseLastAdminResponse(net, resp));
    CHECK(std::string(resp.text) == "ok");
}

TEST_CASE("WorldBroadcaster: admin auth failure counter persists across disconnect-reconnect",
          "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    net.peerAddresses[1] = "1.2.3.4:5678"; // same normalized IP, new peerId
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    fl::ManualClock now;
    setupAuthFixture(broadcaster, net, now);

    // 2 failures on peer 0 — below threshold
    for (int i = 0; i < 2; ++i) {
        auto pkt = makeAdminCmd("wrongpass", "status");
        broadcaster.onReceive(0u, pkt.data(), pkt.size());
    }
    CHECK(net.disconnectedPeers.empty());

    // Peer 0 disconnects
    broadcaster.onDisconnect(0u);
    net.disconnectedPeers.clear();
    net.sends.clear();

    // Peer 1 reconnects from same IP; one more failure should trigger lockout (counter IP-keyed)
    broadcaster.onConnect(1u);
    net.sends.clear();
    net.disconnectedPeers.clear();

    auto pkt = makeAdminCmd("wrongpass", "status");
    broadcaster.onReceive(1u, pkt.data(), pkt.size());
    REQUIRE(net.disconnectedPeers.size() == 1u);
    CHECK(net.disconnectedPeers[0] == 1u);
}

TEST_CASE("WorldBroadcaster: admin auth correct token resets failure counter", "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    fl::ManualClock now;
    setupAuthFixture(broadcaster, net, now);

    // N-1 = 2 failures
    for (int i = 0; i < 2; ++i) {
        auto pkt = makeAdminCmd("wrongpass", "status");
        broadcaster.onReceive(0u, pkt.data(), pkt.size());
    }
    CHECK(net.disconnectedPeers.empty());

    // Successful auth — clears the counter
    auto good = makeAdminCmd("pw", "status");
    broadcaster.onReceive(0u, good.data(), good.size());
    CHECK(net.disconnectedPeers.empty());

    // 2 more failures — should NOT trigger lockout (counter was reset)
    for (int i = 0; i < 2; ++i) {
        auto pkt = makeAdminCmd("wrongpass", "status");
        broadcaster.onReceive(0u, pkt.data(), pkt.size());
    }
    CHECK(net.disconnectedPeers.empty());
}

TEST_CASE("WorldBroadcaster: admin auth wrong tokens when operator_password unset do not record failures",
          "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    net.peerAddresses[1] = "1.2.3.4:5678";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    fl::ManualClock now;
    broadcaster.setAdminAuthParams(3, 60);
    broadcaster.setClock(now);
    // Note: operator_password intentionally NOT set — admin channel disabled
    broadcaster.onConnect(0u);
    net.sends.clear();
    net.disconnectedPeers.clear();

    // Send N+5 = 8 wrong-token packets — admin channel is disabled so none are processed
    for (int i = 0; i < 8; ++i) {
        auto pkt = makeAdminCmd("wrongpass", "status");
        broadcaster.onReceive(0u, pkt.data(), pkt.size());
    }
    CHECK(net.disconnectedPeers.empty());

    // Reconnect from same IP — must not be blocked (no failures recorded)
    broadcaster.onConnect(1u);
    CHECK(net.disconnectedPeers.empty());
}

TEST_CASE("WorldBroadcaster: admin auth pruneExpired fires after 600 onTick calls",
          "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    net.peerAddresses[1] = "1.2.3.4:5678";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    fl::ManualClock now;
    setupAuthFixture(broadcaster, net, now);

    // Trigger lockout
    for (int i = 0; i < 3; ++i) {
        auto pkt = makeAdminCmd("wrongpass", "status");
        broadcaster.onReceive(0u, pkt.data(), pkt.size());
    }
    net.disconnectedPeers.clear();
    net.sends.clear();

    // Advance past TTL — lockout entry is now expired but not yet pruned
    now.advance(std::chrono::seconds(61));

    // Drive 600 onTick calls to trigger the prune cycle
    for (int i = 0; i < 600; ++i)
        broadcaster.onTick(1.0 / 60.0, static_cast<uint64_t>(i + 1));

    // Clear sends accumulated during 600 ticks (per-peer unicast snapshots from any still-connected
    // peers), then reconnect and verify MsgHello is the first new send.
    net.sends.clear();
    net.perPeerSends.clear();

    // After prune + TTL expiry, reconnect from same IP must succeed
    broadcaster.onConnect(1u);
    CHECK(net.disconnectedPeers.empty());
    REQUIRE(!net.sends.empty());
    fl::MsgHello hello{};
    std::memcpy(&hello, net.sends.front().data(), sizeof(hello));
    CHECK(hello.msgId == static_cast<uint8_t>(fl::MsgId::Hello));
}

TEST_CASE("WorldBroadcaster: admin_unlock clears lockout -- onConnect succeeds", "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    net.peerAddresses[1] = "1.2.3.4:5678";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    fl::ManualClock now;
    setupAuthFixture(broadcaster, net, now);

    // Trigger lockout on peer 0
    for (int i = 0; i < 3; ++i) {
        auto pkt = makeAdminCmd("wrongpass", "status");
        broadcaster.onReceive(0u, pkt.data(), pkt.size());
    }
    net.disconnectedPeers.clear();
    net.sends.clear();

    // Unlock: should report that the lockout was active
    CHECK(broadcaster.unlockAdminAuth("1.2.3.4"));

    // Reconnect from same IP must now succeed
    broadcaster.onConnect(1u);
    CHECK(net.disconnectedPeers.empty());
    REQUIRE(!net.sends.empty());
    fl::MsgHello hello{};
    std::memcpy(&hello, net.sends.front().data(), sizeof(hello));
    CHECK(hello.msgId == static_cast<uint8_t>(fl::MsgId::Hello));
}

TEST_CASE("WorldBroadcaster: admin_unlock is a no-op when IP is not locked", "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    net.peerAddresses[1] = "1.2.3.4:5678";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    fl::ManualClock now;
    setupAuthFixture(broadcaster, net, now);

    // No failures — unlockAdminAuth must report IP was not locked
    CHECK_FALSE(broadcaster.unlockAdminAuth("1.2.3.4"));

    // Connect peer 1 from same IP: must not be refused
    broadcaster.onConnect(1u);
    CHECK(net.disconnectedPeers.empty());
}

// ---------------------------------------------------------------------------
// Admin shell drain tests
// ---------------------------------------------------------------------------

TEST_CASE("WorldBroadcaster: admin shell drain sends no follow-on when shell not configured",
          "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    fl::ManualClock t;
    broadcaster.setClock(t);
    broadcaster.setOperatorPassword("secret");
    broadcaster.setAdminDispatch([](std::string_view) -> std::string { return "queued"; });
    // setAdminShell NOT called

    broadcaster.onConnect(0u);
    net.sends.clear();

    auto cmd = makeAdminCmd("secret", "spawn");
    broadcaster.onReceive(0u, cmd.data(), cmd.size());
    std::size_t sendsAfterRecv = net.sends.size(); // 1 sync ack

    t.advance(std::chrono::milliseconds(20));
    broadcaster.onTick(1.0 / 60.0, 1u);

    CHECK(drainSends(net, sendsAfterRecv).empty());
}

TEST_CASE("WorldBroadcaster: admin shell drain does not fire before wall-clock deadline",
          "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    fl::ManualClock t;
    broadcaster.setClock(t);
    broadcaster.setOperatorPassword("secret");
    broadcaster.setAdminDispatch([](std::string_view) -> std::string { return "queued"; });

    ShellDrainMock shell;
    broadcaster.setAdminShell([&shell]() { return shell.mark(); }, [&shell](int m) { return shell.drainSince(m); });

    broadcaster.onConnect(0u);
    net.sends.clear();

    auto cmd = makeAdminCmd("secret", "spawn");
    broadcaster.onReceive(0u, cmd.data(), cmd.size());
    std::size_t sendsAfterRecv = net.sends.size(); // 1 sync ack only

    shell.lines.push_back("[admin] spawned entity=1/1");

    // Drain does not fire before the 20 ms deadline (clock not yet advanced).
    broadcaster.onTick(1.0 / 60.0, 1u);
    CHECK(drainSends(net, sendsAfterRecv).empty());
}

TEST_CASE("WorldBroadcaster: admin shell drain fires after wall-clock deadline and forwards shell lines",
          "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    fl::ManualClock t;
    broadcaster.setClock(t);
    broadcaster.setOperatorPassword("secret");
    broadcaster.setAdminDispatch([](std::string_view) -> std::string { return "queued"; });

    ShellDrainMock shell;
    broadcaster.setAdminShell([&shell]() { return shell.mark(); }, [&shell](int m) { return shell.drainSince(m); });

    broadcaster.onConnect(0u);
    net.sends.clear();

    auto cmd = makeAdminCmd("secret", "spawn", 0x0001u);
    broadcaster.onReceive(0u, cmd.data(), cmd.size());
    std::size_t sendsAfterRecv = net.sends.size();

    // Simulate callback output becoming available.
    shell.lines.push_back("[admin] spawned builtin:debug-entity entity=1/1");

    // Advance clock past 20 ms deadline — drain fires.
    t.advance(std::chrono::milliseconds(20));
    broadcaster.onTick(1.0 / 60.0, 1u);

    auto drain = drainSends(net, sendsAfterRecv);
    REQUIRE(drain.size() == 1u);
    fl::MsgAdminResponse resp{};
    REQUIRE(drain[0].size() == sizeof(fl::MsgAdminResponse));
    std::memcpy(&resp, drain[0].data(), sizeof(resp));
    CHECK(std::string(resp.text) == "[admin] spawned builtin:debug-entity entity=1/1");
}

TEST_CASE("WorldBroadcaster: admin shell drain sends nothing when drain returns empty vector",
          "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    fl::ManualClock t;
    broadcaster.setClock(t);
    broadcaster.setOperatorPassword("secret");
    broadcaster.setAdminDispatch([](std::string_view) -> std::string { return "queued"; });

    ShellDrainMock shell; // lines stays empty
    broadcaster.setAdminShell([&shell]() { return shell.mark(); }, [&shell](int m) { return shell.drainSince(m); });

    broadcaster.onConnect(0u);
    net.sends.clear();

    auto cmd = makeAdminCmd("secret", "set_weather");
    broadcaster.onReceive(0u, cmd.data(), cmd.size());
    std::size_t sendsAfterRecv = net.sends.size();

    t.advance(std::chrono::milliseconds(20));
    broadcaster.onTick(1.0 / 60.0, 1u);

    CHECK(drainSends(net, sendsAfterRecv).empty());
}

TEST_CASE("WorldBroadcaster: admin shell drain skips disconnected peer", "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    fl::ManualClock t;
    broadcaster.setClock(t);
    broadcaster.setOperatorPassword("secret");
    broadcaster.setAdminDispatch([](std::string_view) -> std::string { return "queued"; });

    ShellDrainMock shell;
    broadcaster.setAdminShell([&shell]() { return shell.mark(); }, [&shell](int m) { return shell.drainSince(m); });

    broadcaster.onConnect(0u);
    net.sends.clear();

    auto cmd = makeAdminCmd("secret", "spawn");
    broadcaster.onReceive(0u, cmd.data(), cmd.size());
    std::size_t sendsAfterRecv = net.sends.size();

    shell.lines.push_back("[admin] spawned entity=1/1");

    // Peer disconnects before the drain deadline.
    broadcaster.onDisconnect(0u);

    t.advance(std::chrono::milliseconds(20));
    broadcaster.onTick(1.0 / 60.0, 1u);

    CHECK(drainSends(net, sendsAfterRecv).empty());
}

TEST_CASE("WorldBroadcaster: admin shell drain echoes correct reqId in follow-on response",
          "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    fl::ManualClock t;
    broadcaster.setClock(t);
    broadcaster.setOperatorPassword("secret");
    broadcaster.setAdminDispatch([](std::string_view) -> std::string { return "queued"; });

    ShellDrainMock shell;
    broadcaster.setAdminShell([&shell]() { return shell.mark(); }, [&shell](int m) { return shell.drainSince(m); });

    broadcaster.onConnect(0u);
    net.sends.clear();

    auto cmd = makeAdminCmd("secret", "spawn", 0xABCDu);
    broadcaster.onReceive(0u, cmd.data(), cmd.size());
    std::size_t sendsAfterRecv = net.sends.size();

    shell.lines.push_back("[admin] spawned entity=1/1");
    t.advance(std::chrono::milliseconds(20));
    broadcaster.onTick(1.0 / 60.0, 1u);

    auto drain = drainSends(net, sendsAfterRecv);
    REQUIRE(drain.size() == 1u);
    fl::MsgAdminResponse resp{};
    REQUIRE(drain[0].size() == sizeof(fl::MsgAdminResponse));
    std::memcpy(&resp, drain[0].data(), sizeof(resp));
    CHECK(resp.reqId == 0xABCDu);
}

TEST_CASE("WorldBroadcaster: two admin commands queue independent drains with separate reqIds",
          "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    fl::ManualClock t;
    broadcaster.setClock(t);
    broadcaster.setOperatorPassword("secret");
    broadcaster.setAdminDispatch([](std::string_view) -> std::string { return "queued"; });

    ShellDrainMock shell;
    broadcaster.setAdminShell([&shell]() { return shell.mark(); }, [&shell](int m) { return shell.drainSince(m); });

    broadcaster.onConnect(0u);
    net.sends.clear();

    auto cmd1 = makeAdminCmd("secret", "spawn", 0x0001u);
    auto cmd2 = makeAdminCmd("secret", "kill", 0x0002u);
    broadcaster.onReceive(0u, cmd1.data(), cmd1.size());
    broadcaster.onReceive(0u, cmd2.data(), cmd2.size());
    std::size_t sendsAfterRecv = net.sends.size(); // 2 sync acks

    // Both drains return the same lines (mark/drainSince mock ignores the mark value).
    shell.lines.push_back("[admin] result line");

    t.advance(std::chrono::milliseconds(20));
    broadcaster.onTick(1.0 / 60.0, 1u);

    // Two deferred responses, one per pending drain.
    auto drain = drainSends(net, sendsAfterRecv);
    REQUIRE(drain.size() == 2u);

    // Extract reqIds from both responses.
    fl::MsgAdminResponse r0{}, r1{};
    REQUIRE(drain[0].size() == sizeof(fl::MsgAdminResponse));
    REQUIRE(drain[1].size() == sizeof(fl::MsgAdminResponse));
    std::memcpy(&r0, drain[0].data(), sizeof(r0));
    std::memcpy(&r1, drain[1].data(), sizeof(r1));

    std::vector<uint16_t> reqIds{r0.reqId, r1.reqId};
    std::sort(reqIds.begin(), reqIds.end());
    CHECK(reqIds[0] == 0x0001u);
    CHECK(reqIds[1] == 0x0002u);
}

TEST_CASE("WorldBroadcaster: admin shell drain fires exactly once", "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    fl::ManualClock t;
    broadcaster.setClock(t);
    broadcaster.setOperatorPassword("secret");
    broadcaster.setAdminDispatch([](std::string_view) -> std::string { return "queued"; });

    ShellDrainMock shell;
    broadcaster.setAdminShell([&shell]() { return shell.mark(); }, [&shell](int m) { return shell.drainSince(m); });

    broadcaster.onConnect(0u);
    net.sends.clear();

    auto cmd = makeAdminCmd("secret", "spawn");
    broadcaster.onReceive(0u, cmd.data(), cmd.size());
    std::size_t sendsAfterRecv = net.sends.size();

    shell.lines.push_back("[admin] spawned entity=1/1");

    // Advance past deadline: drain fires on tick 1.
    t.advance(std::chrono::milliseconds(20));
    broadcaster.onTick(1.0 / 60.0, 1u);
    std::size_t sendsAfterTick1 = net.sends.size();

    // Tick 2: drain must NOT fire again (entry was erased).
    broadcaster.onTick(1.0 / 60.0, 2u);

    CHECK(drainSends(net, sendsAfterTick1).empty());
    // And tick 1 did deliver exactly one drain response.
    CHECK(drainSends(net, sendsAfterRecv).size() == 1u);
}

TEST_CASE("WorldBroadcaster: admin shell drain with long output streams as MsgAdminResponseChunk",
          "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    fl::ManualClock t;
    broadcaster.setClock(t);
    broadcaster.setOperatorPassword("secret");
    broadcaster.setAdminDispatch([](std::string_view) -> std::string { return "queued"; });

    ShellDrainMock shell;
    broadcaster.setAdminShell([&shell]() { return shell.mark(); }, [&shell](int m) { return shell.drainSince(m); });

    broadcaster.onConnect(0u);
    net.sends.clear();

    auto cmd = makeAdminCmd("secret", "peers");
    broadcaster.onReceive(0u, cmd.data(), cmd.size());
    std::size_t sendsAfterRecv = net.sends.size();

    // Line longer than kAdminResponseFastPathMax (123) → must route via chunk stream.
    shell.lines.push_back(std::string(200, 'x'));

    t.advance(std::chrono::milliseconds(20));
    broadcaster.onTick(1.0 / 60.0, 1u);

    auto drain = drainSends(net, sendsAfterRecv);
    REQUIRE(drain.size() == 1u);
    fl::MsgAdminResponseChunk chunk{};
    REQUIRE(drain[0].size() == sizeof(fl::MsgAdminResponseChunk));
    std::memcpy(&chunk, drain[0].data(), sizeof(chunk));
    CHECK(chunk.msgId == static_cast<uint8_t>(fl::MsgId::AdminResponseChunk));
    CHECK((chunk.flags & fl::kChunkFlagEnd) != 0u);
    CHECK(std::strlen(chunk.body) == 200u);
}

TEST_CASE("WorldBroadcaster: admin shell drain joins multiple lines with newline",
          "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    fl::ManualClock t;
    broadcaster.setClock(t);
    broadcaster.setOperatorPassword("secret");
    broadcaster.setAdminDispatch([](std::string_view) -> std::string { return "queued"; });

    ShellDrainMock shell;
    broadcaster.setAdminShell([&shell]() { return shell.mark(); }, [&shell](int m) { return shell.drainSince(m); });

    broadcaster.onConnect(0u);
    net.sends.clear();

    auto cmd = makeAdminCmd("secret", "peers");
    broadcaster.onReceive(0u, cmd.data(), cmd.size());
    std::size_t sendsAfterRecv = net.sends.size();

    shell.lines = {"line1", "line2", "line3"};

    t.advance(std::chrono::milliseconds(20));
    broadcaster.onTick(1.0 / 60.0, 1u);

    auto drain = drainSends(net, sendsAfterRecv);
    REQUIRE(drain.size() == 1u);
    fl::MsgAdminResponse resp{};
    REQUIRE(drain[0].size() == sizeof(fl::MsgAdminResponse));
    std::memcpy(&resp, drain[0].data(), sizeof(resp));
    CHECK(std::string(resp.text) == "line1\nline2\nline3");
}

TEST_CASE("WorldBroadcaster: admin shell drain sends nothing when all drain lines are empty",
          "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    fl::ManualClock t;
    broadcaster.setClock(t);
    broadcaster.setOperatorPassword("secret");
    broadcaster.setAdminDispatch([](std::string_view) -> std::string { return "queued"; });

    ShellDrainMock shell;
    broadcaster.setAdminShell([&shell]() { return shell.mark(); }, [&shell](int m) { return shell.drainSince(m); });

    broadcaster.onConnect(0u);
    net.sends.clear();

    auto cmd = makeAdminCmd("secret", "spawn");
    broadcaster.onReceive(0u, cmd.data(), cmd.size());
    std::size_t sendsAfterRecv = net.sends.size();

    // All-empty strings: join produces "\n", pop_back gives "", guard suppresses send.
    shell.lines = {"", ""};

    t.advance(std::chrono::milliseconds(20));
    broadcaster.onTick(1.0 / 60.0, 1u);

    CHECK(drainSends(net, sendsAfterRecv).empty());
}

TEST_CASE("WorldBroadcaster: admin shell drain fires at wall-clock deadline regardless of tick index",
          "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    fl::ManualClock t;
    broadcaster.setClock(t);
    broadcaster.setOperatorPassword("secret");
    broadcaster.setAdminDispatch([](std::string_view) -> std::string { return "queued"; });

    ShellDrainMock shell;
    broadcaster.setAdminShell([&shell]() { return shell.mark(); }, [&shell](int m) { return shell.drainSince(m); });

    broadcaster.onConnect(0u);
    net.sends.clear();

    auto cmd = makeAdminCmd("secret", "spawn");
    broadcaster.onReceive(0u, cmd.data(), cmd.size());
    std::size_t sendsAfterRecv = net.sends.size();

    shell.lines.push_back("[admin] spawned entity=1/1");

    // Three ticks without advancing the clock: drain must NOT fire on any of them
    // (simulates GameLoop catch-up where tick N+1 fires before callbacks from tick N run).
    broadcaster.onTick(1.0 / 60.0, 1u);
    broadcaster.onTick(1.0 / 60.0, 2u);
    broadcaster.onTick(1.0 / 60.0, 3u);
    CHECK(drainSends(net, sendsAfterRecv).empty());

    // Advance clock past the 20 ms deadline; drain fires on next onTick.
    t.advance(std::chrono::milliseconds(20));
    broadcaster.onTick(1.0 / 60.0, 4u);
    CHECK(drainSends(net, sendsAfterRecv).size() == 1u);
}

TEST_CASE("WorldBroadcaster: two admin commands at staggered deadlines drain independently",
          "[world_broadcaster][admin_command]") {
    MockLogger log;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:1234";
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);
    fl::ManualClock t;
    broadcaster.setClock(t);
    broadcaster.setOperatorPassword("secret");
    broadcaster.setAdminDispatch([](std::string_view) -> std::string { return "queued"; });

    ShellDrainMock shell;
    broadcaster.setAdminShell([&shell]() { return shell.mark(); }, [&shell](int m) { return shell.drainSince(m); });

    broadcaster.onConnect(0u);
    net.sends.clear();

    // Command A at t=0 ms; its deadline is t=20 ms.
    auto cmd1 = makeAdminCmd("secret", "spawn", 0x0001u);
    broadcaster.onReceive(0u, cmd1.data(), cmd1.size());
    shell.lines.push_back("[admin] output-A");

    // Advance to t=25 ms; command A's deadline (20 ms) has already passed.
    t.advance(std::chrono::milliseconds(25));

    // Command B at t=25 ms; its deadline is t=45 ms.
    auto cmd2 = makeAdminCmd("secret", "kill", 0x0002u);
    broadcaster.onReceive(0u, cmd2.data(), cmd2.size());
    std::size_t sendsAfterB = net.sends.size();

    // Tick at t=25 ms: only A fires (deadline passed); B is still pending.
    broadcaster.onTick(1.0 / 60.0, 1u);
    auto drainAfterTick1 = drainSends(net, sendsAfterB);
    REQUIRE(drainAfterTick1.size() == 1u);
    fl::MsgAdminResponse r{};
    REQUIRE(drainAfterTick1[0].size() == sizeof(fl::MsgAdminResponse));
    std::memcpy(&r, drainAfterTick1[0].data(), sizeof(r));
    CHECK(r.reqId == 0x0001u);

    // B's deadline not yet reached; advance only 10 ms more (t=35 ms < 45 ms).
    std::size_t sendsAfterTick1Total = net.sends.size();
    t.advance(std::chrono::milliseconds(10));
    broadcaster.onTick(1.0 / 60.0, 2u);
    CHECK(drainSends(net, sendsAfterTick1Total).empty());

    // Advance past B's deadline (t=45 ms + 1 ms); B fires.
    std::size_t sendsBeforeB = net.sends.size();
    t.advance(std::chrono::milliseconds(11));
    broadcaster.onTick(1.0 / 60.0, 3u);
    auto drainB = drainSends(net, sendsBeforeB);
    REQUIRE(drainB.size() == 1u);
    fl::MsgAdminResponse r2{};
    REQUIRE(drainB[0].size() == sizeof(fl::MsgAdminResponse));
    std::memcpy(&r2, drainB[0].data(), sizeof(r2));
    CHECK(r2.reqId == 0x0002u);
}

TEST_CASE("WorldBroadcaster: MsgConnectAck planetRadiusKm is Earth radius by default", "[world_broadcaster][gravity]") {
    MockLogger log;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);

    broadcaster.onConnect(0u);

    fl::MsgConnectAck ack = parseSendAck(net);
    CHECK(ack.planetRadiusKm == Catch::Approx(6371.f).epsilon(1e-4f));
}

TEST_CASE("WorldBroadcaster: setGravityField propagates planetRadiusKm to MsgConnectAck",
          "[world_broadcaster][gravity]") {
    MockLogger log;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(log, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, log);

    broadcaster.setGravityField(fl::CentralGravityField::earthInstance(), 6371.f);
    broadcaster.onConnect(0u);

    fl::MsgConnectAck ack = parseSendAck(net);
    CHECK(ack.planetRadiusKm == Catch::Approx(6371.f).epsilon(1e-4f));
}

TEST_CASE("WorldBroadcaster: spawn position preserves sub-mm precision at large world offset", "[world_broadcaster]") {
    // At x = 1e5 m, float ULP is ~0.0119 m — a 1 mm fractional component would be
    // rounded away when storing into float pos_world.  With double pos_world the
    // 1 mm offset must survive the spawn -> integrator -> broadcast round-trip.
    MockLogger logger;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:5000";
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setSpawnPoints({std::array<double, 3>{1e5 + 1e-3, 500.0, 0.0}});
    broadcaster.onConnect(0u);
    broadcaster.onTick(1.0 / 60.0, 1u);

    REQUIRE(!snapshotsFor(net, 0).empty());
    const auto pkt = snapshotsFor(net, 0).back();
    REQUIRE(parseSnapshotHeader(pkt).recordCount >= 1u);
    const auto _ents = decodeEntities(pkt);
    REQUIRE(!_ents.empty());
    const DecodedEntity& e = _ents[0];

    // With float pos_world the spawn would be rounded to exactly 1e5; the 1 mm fractional
    // offset must be preserved.  Lateral gravity (~4e-7 m/tick) is negligible.
    CHECK(e.pos[0] > 1e5 + 5e-4);
}

// ---------------------------------------------------------------------------
// Heartbeat / MsgPeerDelay tests
// ---------------------------------------------------------------------------

TEST_CASE("WorldBroadcaster: MsgHeartbeat triggers MsgPeerDelay reply", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    broadcaster.onConnect(0u);
    broadcaster.onTick(1.0 / 60.0, 70u); // m_currentTick = 70

    const std::size_t sendsBefore = net.sends.size();

    fl::MsgHeartbeat hb;
    hb.tickIndex = 10u; // delay = 70 - 10 = 60 ticks
    broadcaster.onReceive(0u, &hb, sizeof(hb));

    REQUIRE(net.sends.size() == sendsBefore + 1u);
    const auto& reply = net.sends.back();
    REQUIRE(reply.size() == sizeof(fl::MsgPeerDelay));
    fl::MsgPeerDelay pd;
    std::memcpy(&pd, reply.data(), sizeof(pd));
    CHECK(pd.msgId == static_cast<uint8_t>(fl::MsgId::PeerDelay));
    CHECK(pd.delayTicks == 60u);
    CHECK(!net.sendReliable); // must be unreliable
}

TEST_CASE("WorldBroadcaster: MsgHeartbeat with future tickIndex does not update delay", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    broadcaster.onConnect(0u);
    broadcaster.onTick(1.0 / 60.0, 50u); // m_currentTick = 50

    fl::MsgHeartbeat hb;
    hb.tickIndex = 60u; // future tick: 60 > 50 — server must ignore
    broadcaster.onReceive(0u, &hb, sizeof(hb));

    // A MsgPeerDelay is still sent but delayTicks should be 0 (estimate not updated)
    REQUIRE(!net.sends.empty());
    fl::MsgPeerDelay pd;
    std::memcpy(&pd, net.sends.back().data(), sizeof(pd));
    CHECK(pd.delayTicks == 0u);
}

TEST_CASE("WorldBroadcaster: MsgHeartbeat caps delayTicks at uint16 max", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    broadcaster.onConnect(0u);
    // Drive to a high tick so estimatedDelayTicks will exceed 65535
    broadcaster.onTick(1.0 / 60.0, 70000u);

    fl::MsgHeartbeat hb;
    hb.tickIndex = 0u; // delay = 70000 - 0 = 70000 > 65535
    broadcaster.onReceive(0u, &hb, sizeof(hb));

    REQUIRE(!net.sends.empty());
    fl::MsgPeerDelay pd;
    std::memcpy(&pd, net.sends.back().data(), sizeof(pd));
    CHECK(pd.delayTicks == 65535u);
}

TEST_CASE("WorldBroadcaster: truncated MsgHeartbeat is discarded", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    broadcaster.onConnect(0u);
    const std::size_t sendsBefore = net.sends.size();

    uint8_t tiny[4] = {static_cast<uint8_t>(fl::MsgId::Heartbeat), 0, 0, 0};
    broadcaster.onReceive(0u, tiny, sizeof(tiny));

    CHECK(net.sends.size() == sendsBefore); // no reply sent
}

TEST_CASE("WorldBroadcaster: two peers each receive their own MsgPeerDelay", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    broadcaster.onConnect(0u);
    broadcaster.onConnect(1u);
    broadcaster.onTick(1.0 / 60.0, 100u); // m_currentTick = 100

    // Peer 0: tickIndex=40 → delay = 60; peer 1: tickIndex=70 → delay = 30
    const std::size_t sendsBefore = net.sends.size();

    fl::MsgHeartbeat hb0;
    hb0.tickIndex = 40u;
    broadcaster.onReceive(0u, &hb0, sizeof(hb0));

    fl::MsgHeartbeat hb1;
    hb1.tickIndex = 70u;
    broadcaster.onReceive(1u, &hb1, sizeof(hb1));

    REQUIRE(net.sends.size() == sendsBefore + 2u);

    fl::MsgPeerDelay pd0, pd1;
    std::memcpy(&pd0, net.sends[sendsBefore].data(), sizeof(pd0));
    std::memcpy(&pd1, net.sends[sendsBefore + 1].data(), sizeof(pd1));
    CHECK(pd0.delayTicks == 60u);
    CHECK(pd1.delayTicks == 30u);
}

// ---------------------------------------------------------------------------
// Idle timeout tests
// ---------------------------------------------------------------------------

TEST_CASE("WorldBroadcaster: idle timeout 0 never kicks", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setIdleTimeout(0); // disabled

    broadcaster.onConnect(0u);
    for (uint64_t t = 1; t <= 600; ++t)
        broadcaster.onTick(1.0 / 60.0, t);

    CHECK(net.disconnectedPeers.empty());
}

TEST_CASE("WorldBroadcaster: idle timeout disconnects peer after inactivity", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setIdleTimeout(1); // 1 second = 60 ticks

    broadcaster.onConnect(0u); // lastActivityTick = m_currentTick = 0

    // Run 59 ticks (delay 59 < 60): no kick
    for (uint64_t t = 1; t <= 59; ++t)
        broadcaster.onTick(1.0 / 60.0, t);
    CHECK(net.disconnectedPeers.empty());

    // Tick 60: delay = 60 >= 60 → kick
    broadcaster.onTick(1.0 / 60.0, 60u);
    CHECK(!net.disconnectedPeers.empty());
}

TEST_CASE("WorldBroadcaster: MsgHeartbeat resets idle timer", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setIdleTimeout(1); // 60 ticks

    broadcaster.onConnect(0u); // lastActivityTick = 0

    // Tick 55: still within window (55 < 60)
    for (uint64_t t = 1; t <= 55; ++t)
        broadcaster.onTick(1.0 / 60.0, t);
    REQUIRE(net.disconnectedPeers.empty());

    // Send a heartbeat at tick 55: resets lastActivityTick to 55
    fl::MsgHeartbeat hb;
    hb.tickIndex = 30u; // doesn't matter for the idle reset test
    broadcaster.onReceive(0u, &hb, sizeof(hb));

    // Ticks 56..114: delay from 55 is at most 114-55=59 < 60 → no kick
    for (uint64_t t = 56; t <= 114; ++t)
        broadcaster.onTick(1.0 / 60.0, t);
    CHECK(net.disconnectedPeers.empty());

    // Tick 115: 115-55=60 >= 60 → kick
    broadcaster.onTick(1.0 / 60.0, 115u);
    CHECK(!net.disconnectedPeers.empty());
}

TEST_CASE("WorldBroadcaster: MsgClientInput resets idle timer", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setIdleTimeout(1); // 60 ticks

    broadcaster.onConnect(0u);

    // Tick 55: no kick yet
    for (uint64_t t = 1; t <= 55; ++t)
        broadcaster.onTick(1.0 / 60.0, t);
    REQUIRE(net.disconnectedPeers.empty());

    // Send a MsgClientInput at tick 55: resets lastActivityTick to 55
    fl::MsgClientInput inp{};
    inp.msgId = static_cast<uint8_t>(fl::MsgId::ClientInput);
    inp.seqNum = 1u;
    inp.tickIndex = 40u;
    broadcaster.onReceive(0u, &inp, sizeof(inp));

    // Ticks 56..114: no kick (max delay = 114-55 = 59 < 60)
    for (uint64_t t = 56; t <= 114; ++t)
        broadcaster.onTick(1.0 / 60.0, t);
    CHECK(net.disconnectedPeers.empty());
}

// ---------------------------------------------------------------------------
// SpatialIndex integration
// ---------------------------------------------------------------------------

TEST_CASE("WorldBroadcaster: spatialIndex is populated with live entity count after onTick", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::EntityTransform t{};
    em.spawn("builtin:debug-entity", t);
    em.spawn("builtin:debug-entity", t);
    em.spawn("builtin:debug-entity", t);

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.onTick(1.0 / 60.0, 1u);

    CHECK(broadcaster.spatialIndex().entityCount() == 3u);
}

// Spy controller: captures the SpatialIndex pointer received via sample().
struct SpyController : fl::IEntityController {
    const fl::SpatialIndex* lastSi{nullptr};
    fl::ControlInput sample(const fl::EntityState&, uint64_t, double, const fl::SpatialIndex* si) override {
        lastSi = si;
        return {};
    }
};

TEST_CASE("WorldBroadcaster: sample receives non-null SpatialIndex pointer from onTick", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::EntityTransform t{};
    t.pos[1] = 500.0;
    fl::EntityId id = em.spawn("builtin:debug-entity", t);
    REQUIRE(id.valid());

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    auto spy = std::make_unique<SpyController>();
    SpyController* spyPtr = spy.get();
    broadcaster.registerController(id, std::move(spy));

    broadcaster.onTick(1.0 / 60.0, 1u);

    // si must be non-null and the index must already hold the entity (rebuilt before sample())
    REQUIRE(spyPtr->lastSi != nullptr);
    CHECK(spyPtr->lastSi->entityCount() == 1u);
}

// ---------------------------------------------------------------------------
// Interest management + delta compression tests (#346)
// ---------------------------------------------------------------------------

TEST_CASE("WorldBroadcaster: entity within draw distance appears in peer snapshot", "[world_broadcaster][interest]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    fl::EntityTransform t{};
    t.pos[1] = 500.0; // spawn near origin
    em.spawn("builtin:debug-entity", t);

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setDrawDistance(200.f); // 200 km — entity at origin is visible
    broadcaster.onConnect(0u);
    broadcaster.onTick(1.0 / 60.0, 1u);

    auto snaps = snapshotsFor(net, 0);
    REQUIRE(!snaps.empty());
    auto hdr = parseSnapshotHeader(snaps[0]);
    CHECK(totalEntityCount(hdr) >= 1u); // at least the spawned entity + peer entity
}

TEST_CASE("WorldBroadcaster: entity beyond draw distance excluded from peer snapshot",
          "[world_broadcaster][interest]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    // Spawn entity far away
    fl::EntityTransform far{};
    far.pos[0] = 20'000.0; // 20 km in +X
    far.pos[1] = 500.0;
    auto farId = em.spawn("builtin:debug-entity", far);
    REQUIRE(farId.valid());
    const uint32_t farIdx = farId.index;

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setDrawDistance(1.f); // 1 km — far entity is outside
    broadcaster.onConnect(0u);        // peer spawns near origin (default)
    broadcaster.onTick(1.0 / 60.0, 1u);

    auto snaps = snapshotsFor(net, 0);
    REQUIRE(!snaps.empty());
    // Verify far entity does NOT appear in the full entries
    for (const auto& e : parseFullEntries(snaps[0]))
        CHECK(e.entityIdx != farIdx);
}

TEST_CASE("WorldBroadcaster: two peers at different positions see disjoint entity sets",
          "[world_broadcaster][interest]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setDrawDistance(5.f); // 5 km radius

    // Peer 0 spawns near origin; peer 1 spawns 100 km away
    broadcaster.setSpawnPoints({std::array<double, 3>{0.0, 500.0, 0.0}, std::array<double, 3>{100'000.0, 500.0, 0.0}});
    broadcaster.onConnect(0u);
    broadcaster.onConnect(1u);
    broadcaster.onTick(1.0 / 60.0, 1u);

    auto snaps0 = snapshotsFor(net, 0);
    auto snaps1 = snapshotsFor(net, 1);
    REQUIRE(!snaps0.empty());
    REQUIRE(!snaps1.empty());

    // Each peer should see exactly their own entity (1 entity each, 100 km apart)
    CHECK(totalEntityCount(parseSnapshotHeader(snaps0[0])) == 1u);
    CHECK(totalEntityCount(parseSnapshotHeader(snaps1[0])) == 1u);
}

TEST_CASE("WorldBroadcaster: 3D interest cull rejects an entity far in altitude (#402)",
          "[world_broadcaster][interest]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    // The peer spawns near (0, ~500, 0). Place two entities at the SAME XZ (same spatial-hash cell,
    // so the conservative XZ query returns both) but different altitudes.
    fl::EntityTransform near{};
    near.pos[0] = 0.0;
    near.pos[1] = 700.0; // ~200 m above the peer — inside a 1 km sphere
    near.pos[2] = 0.0;
    const uint32_t nearIdx = em.spawn("builtin:debug-entity", near).index;

    fl::EntityTransform high{};
    high.pos[0] = 0.0;
    high.pos[1] = 6000.0; // ~5.5 km above the peer — outside a 1 km sphere despite same XZ cell
    high.pos[2] = 0.0;
    const uint32_t highIdx = em.spawn("builtin:debug-entity", high).index;

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setDrawDistance(1.f); // 1 km interest sphere
    broadcaster.onConnect(0u);
    broadcaster.onTick(1.0 / 60.0, 1u);

    auto snaps = snapshotsFor(net, 0);
    REQUIRE(!snaps.empty());
    bool sawNear = false, sawHigh = false;
    for (const auto& e : decodeEntities(snaps[0])) {
        if (e.entityIdx == nearIdx)
            sawNear = true;
        if (e.entityIdx == highIdx)
            sawHigh = true;
    }
    CHECK(sawNear);       // within the 3D sphere
    CHECK_FALSE(sawHigh); // culled by the XYZ distance gate (would pass an XZ-only check)
}

TEST_CASE("WorldBroadcaster: setDrawDistance(0) produces empty snapshots", "[world_broadcaster][interest]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setDrawDistance(0.f); // radius 0 → no cells queried
    broadcaster.onConnect(0u);
    broadcaster.onTick(1.0 / 60.0, 1u);

    auto snaps = snapshotsFor(net, 0);
    REQUIRE(!snaps.empty());
    auto hdr = parseSnapshotHeader(snaps[0]);
    CHECK(hdr.recordCount == 0u);
}

TEST_CASE("WorldBroadcaster: dead peer entity results in empty snapshot", "[world_broadcaster][interest]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.onConnect(0u);
    // Retrieve peer's entity from ConnectAck
    fl::MsgConnectAck ack{};
    std::memcpy(&ack, net.sends[1].data(), sizeof(ack));
    fl::EntityId peerEid{ack.assignedEntityIdx, ack.assignedEntityGen};
    // Kill the peer's entity before the tick
    em.kill(peerEid);
    em.onTick(1.0 / 60.0, 0u); // reap dead entities

    broadcaster.onTick(1.0 / 60.0, 1u);

    auto snaps = snapshotsFor(net, 0);
    REQUIRE(!snaps.empty());
    auto hdr = parseSnapshotHeader(snaps[0]);
    CHECK(hdr.recordCount == 0u);
}

TEST_CASE("WorldBroadcaster: applyConfig propagates drawDistanceKm", "[world_broadcaster][interest]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    fl::EntityTransform far{};
    far.pos[0] = 20'000.0; // 20 km away — in a different spatial hash cell (default cell = 10 km)
    far.pos[1] = 500.0;
    auto farId = em.spawn("builtin:debug-entity", far);
    const uint32_t farIdx = farId.index;

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    fl::WorldBroadcasterConfig cfg;
    cfg.drawDistanceKm = 1.f; // 1 km — only queries cell at peer origin; 20 km entity is in a different cell
    broadcaster.applyConfig(cfg);
    broadcaster.onConnect(0u);
    broadcaster.onTick(1.0 / 60.0, 1u);

    auto snaps = snapshotsFor(net, 0);
    REQUIRE(!snaps.empty());
    for (const auto& e : parseFullEntries(snaps[0]))
        CHECK(e.entityIdx != farIdx);
}

TEST_CASE("WorldBroadcaster: first tick sends full entries, second tick sends updates", "[world_broadcaster][delta]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.onConnect(0u);

    // Tick 1: all entities new — must be full entries
    broadcaster.onTick(1.0 / 60.0, 1u);
    auto snaps1 = snapshotsFor(net, 0);
    REQUIRE(!snaps1.empty());
    CHECK(fullRecordCount(snaps1[0]) >= 1u);
    CHECK(deltaRecordCount(snaps1[0]) == 0u);

    // Client acks tick 1, then tick 2: identities are confirmed — must be delta records.
    ackTick(broadcaster, 0u, 1u, 1u);
    clearSnapshots(net);
    broadcaster.onTick(1.0 / 60.0, 2u);
    auto snaps2 = snapshotsFor(net, 0);
    REQUIRE(!snaps2.empty());
    CHECK(fullRecordCount(snaps2[0]) == 0u);
    CHECK(deltaRecordCount(snaps2[0]) >= 1u);
}

TEST_CASE("WorldBroadcaster: entity stays full every tick until the client acks", "[world_broadcaster][delta]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.onConnect(0u);

    // No ack: the peer's identity is unconfirmed, so the record is re-sent full every tick (loss
    // recovery — whatever snapshot the client first receives carries the full).
    for (uint64_t tick = 1; tick <= 5; ++tick) {
        clearSnapshots(net);
        broadcaster.onTick(1.0 / 60.0, tick);
        auto snaps = snapshotsFor(net, 0);
        REQUIRE(!snaps.empty());
        CHECK(fullRecordCount(snaps[0]) >= 1u);
        CHECK(deltaRecordCount(snaps[0]) == 0u);
    }

    // The client acks tick 5; subsequent ticks converge to deltas.
    ackTick(broadcaster, 0u, 5u, 1u);
    clearSnapshots(net);
    broadcaster.onTick(1.0 / 60.0, 6u);
    auto snaps = snapshotsFor(net, 0);
    REQUIRE(!snaps.empty());
    CHECK(fullRecordCount(snaps[0]) == 0u);
    CHECK(deltaRecordCount(snaps[0]) >= 1u);
}

TEST_CASE("WorldBroadcaster: a fresh peer is sent full records for all entities until first ack",
          "[world_broadcaster][delta]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    for (int i = 0; i < 5; ++i) {
        fl::EntityTransform t{};
        t.pos[0] = 100.0 + i * 10.0;
        t.pos[1] = 500.0;
        em.spawn("builtin:debug-entity", t);
    }

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.onConnect(0u);

    // ackedTick == 0 (no ack yet): every visible entity bootstraps as a full record.
    broadcaster.onTick(1.0 / 60.0, 1u);
    auto snaps1 = snapshotsFor(net, 0);
    REQUIRE(!snaps1.empty());
    CHECK(deltaRecordCount(snaps1[0]) == 0u);
    const uint16_t fullsTick1 = fullRecordCount(snaps1[0]);
    CHECK(fullsTick1 >= 5u);

    // After acking tick 1, the same set downgrades to deltas.
    ackTick(broadcaster, 0u, 1u, 1u);
    clearSnapshots(net);
    broadcaster.onTick(1.0 / 60.0, 2u);
    auto snaps2 = snapshotsFor(net, 0);
    REQUIRE(!snaps2.empty());
    CHECK(fullRecordCount(snaps2[0]) == 0u);
    CHECK(deltaRecordCount(snaps2[0]) >= 5u);
}

TEST_CASE("WorldBroadcaster: a heartbeat-only client still acks and downgrades to delta",
          "[world_broadcaster][delta]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.onConnect(0u);

    broadcaster.onTick(1.0 / 60.0, 1u);
    REQUIRE(fullRecordCount(snapshotsFor(net, 0).back()) >= 1u);

    // Ack tick 1 via a heartbeat (no MsgClientInput).
    fl::MsgHeartbeat hb{};
    hb.tickIndex = 1u;
    broadcaster.onReceive(0u, &hb, sizeof(hb));

    clearSnapshots(net);
    broadcaster.onTick(1.0 / 60.0, 2u);
    auto snaps = snapshotsFor(net, 0);
    REQUIRE(!snaps.empty());
    CHECK(fullRecordCount(snaps[0]) == 0u);
    CHECK(deltaRecordCount(snaps[0]) >= 1u);
}

TEST_CASE("WorldBroadcaster: a future-tick ack is clamped to the present and cannot pre-confirm",
          "[world_broadcaster][delta]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.onConnect(0u);

    // Tick 1, then the client claims to have acked a far-future tick (9999). Clamped to the current
    // tick (1), it cannot pre-confirm an entity the server has not even sent yet.
    broadcaster.onTick(1.0 / 60.0, 1u);
    ackTick(broadcaster, 0u, 9999u, 1u);

    // A new entity first appears at tick 2 (full streak starts at tick 2 > the clamped ack of 1).
    fl::EntityTransform t{};
    t.pos[1] = 500.0;
    const uint32_t newIdx = em.spawn("builtin:debug-entity", t).index;

    auto recordFor = [&](const std::vector<uint8_t>& pkt, uint32_t idx) -> std::optional<DecodedEntity> {
        for (const auto& d : decodeEntities(pkt))
            if (d.entityIdx == idx)
                return d;
        return std::nullopt;
    };

    broadcaster.onTick(1.0 / 60.0, 2u); // new entity → full (first sight)
    REQUIRE(recordFor(snapshotsFor(net, 0).back(), newIdx).value().isFull);

    // Tick 3 (no new ack): had the future ack NOT been clamped, ackedTick would be 9999 and the entity
    // would wrongly drop to a delta. Clamped, ackedTick is 1 < its streak start (2), so it stays full.
    clearSnapshots(net);
    broadcaster.onTick(1.0 / 60.0, 3u);
    auto rec3 = recordFor(snapshotsFor(net, 0).back(), newIdx);
    REQUIRE(rec3.has_value());
    CHECK(rec3->isFull);
}

TEST_CASE("WorldBroadcaster: a dropped full keeps re-sending full until a later tick is acked",
          "[world_broadcaster][delta]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.onConnect(0u);

    // Ticks 1-3 sent but the client's acks stall at 0 (e.g. the full packets were lost): every tick
    // re-sends a full so the first packet the client does receive carries the identity.
    for (uint64_t tick = 1; tick <= 3; ++tick) {
        clearSnapshots(net);
        broadcaster.onTick(1.0 / 60.0, tick);
        CHECK(fullRecordCount(snapshotsFor(net, 0).back()) >= 1u);
    }

    // The client finally receives and acks tick 3: the contiguous full streak started at tick 1,
    // and 3 >= 1, so it converges to deltas.
    ackTick(broadcaster, 0u, 3u, 1u);
    clearSnapshots(net);
    broadcaster.onTick(1.0 / 60.0, 4u);
    CHECK(fullRecordCount(snapshotsFor(net, 0).back()) == 0u);
    CHECK(deltaRecordCount(snapshotsFor(net, 0).back()) >= 1u);
}

TEST_CASE("WorldBroadcaster: a deferral restarts the full streak so an ack of the withheld tick cannot confirm",
          "[world_broadcaster][delta][budget]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    // Two far entities competing for a one-extra-record budget so the scheduler admits one and defers
    // the other each tick (the peer's own entity is always admitted first).
    for (int i = 0; i < 2; ++i) {
        fl::EntityTransform t{};
        t.pos[0] = 5000.0 + i * 10.0;
        t.pos[1] = 500.0;
        em.spawn("builtin:debug-entity", t);
    }

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setDrawDistance(200.f);
    // Budget must clear the fixed header+TLV overhead (~72 B) plus the peer's own record, leaving room
    // for exactly one of the two far records — so one far entity is deferred each tick.
    broadcaster.setSnapshotBudget(140u);
    broadcaster.onConnect(0u);

    auto farIdxIn = [&](const std::vector<uint8_t>& pkt) -> std::optional<uint32_t> {
        for (const auto& d : decodeEntities(pkt))
            if (d.pos[0] > 4000.0) // a far entity (not the peer's own, which is near origin)
                return d.entityIdx;
        return std::nullopt;
    };
    auto recordFor = [&](const std::vector<uint8_t>& pkt, uint32_t idx) -> std::optional<DecodedEntity> {
        for (const auto& d : decodeEntities(pkt))
            if (d.entityIdx == idx)
                return d;
        return std::nullopt;
    };

    // Tick 1: one far entity X is sent (full), the other is deferred.
    broadcaster.onTick(1.0 / 60.0, 1u);
    auto x = farIdxIn(snapshotsFor(net, 0).back());
    REQUIRE(x.has_value());
    REQUIRE(recordFor(snapshotsFor(net, 0).back(), *x)->isFull);

    // Tick 2: X is deferred (the other far entity, higher recency, takes the slot). X is absent.
    clearSnapshots(net);
    broadcaster.onTick(1.0 / 60.0, 2u);
    REQUIRE_FALSE(recordFor(snapshotsFor(net, 0).back(), *x).has_value());

    // The client acks tick 2 (which did NOT contain X) — this models the client having dropped tick 1
    // (X's full) and only received tick 2. A naive "delta if fullStreakTick <= ackedTick" would now
    // mis-send X as an undecodable delta; the deferral guard pushed X's streak start past tick 2.
    ackTick(broadcaster, 0u, 2u, 1u);

    // Tick 3: X reappears and MUST be a full record (the client never learned it).
    clearSnapshots(net);
    broadcaster.onTick(1.0 / 60.0, 3u);
    auto xRec = recordFor(snapshotsFor(net, 0).back(), *x);
    REQUIRE(xRec.has_value());
    CHECK(xRec->isFull);
}

TEST_CASE("WorldBroadcaster: entity gen change forces a full entry", "[world_broadcaster][delta]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    fl::EntityTransform t{};
    t.pos[1] = 500.0;
    auto id1 = em.spawn("builtin:debug-entity", t);
    REQUIRE(id1.valid());
    const uint32_t slotIdx = id1.index;

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.onConnect(0u);

    // Tick 1: entity appears as full entry, gen is cached
    broadcaster.onTick(1.0 / 60.0, 1u);
    ackTick(broadcaster, 0u, 1u, 1u);
    clearSnapshots(net);

    // Kill the entity and spawn a new one — new entity may reuse the same pool slot with a new gen
    em.kill(id1);
    em.onTick(1.0 / 60.0, 0u); // reap
    auto id2 = em.spawn("builtin:debug-entity", t);
    REQUIRE(id2.valid());
    // id2 may or may not have the same index as id1; if it does, gen is different
    // Either way, any newly spawned entity appears as a full entry because it's not in knownGens
    broadcaster.onTick(1.0 / 60.0, 2u);

    auto snaps = snapshotsFor(net, 0);
    REQUIRE(!snaps.empty());
    // The new entity (possibly same slot, new gen) must appear as a full entry
    bool newEntityIsFullEntry = false;
    for (const auto& e : parseFullEntries(snaps[0]))
        if (e.entityIdx == id2.index && e.entityGen == id2.generation)
            newEntityIsFullEntry = true;
    CHECK(newEntityIsFullEntry);
    (void)slotIdx;
}

TEST_CASE("WorldBroadcaster: reconnect after disconnect starts with fresh known-gen state",
          "[world_broadcaster][delta]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.onConnect(0u);

    // Tick 1: entity known, cached; client acks it
    broadcaster.onTick(1.0 / 60.0, 1u);
    ackTick(broadcaster, 0u, 1u, 1u);
    clearSnapshots(net);

    // Tick 2: entity should be update entry
    broadcaster.onTick(1.0 / 60.0, 2u);
    {
        auto snaps = snapshotsFor(net, 0);
        REQUIRE(!snaps.empty());
        CHECK(deltaRecordCount(snaps[0]) >= 1u);
    }
    clearSnapshots(net);

    // Disconnect clears knownGens and ackedTick; reconnect gives fresh state
    broadcaster.onDisconnect(0u);
    broadcaster.onConnect(0u); // new peer gets peerId=0 again (TrackingNetwork reuses it)

    // Tick 3: fresh connection — all entities must be full entries again
    broadcaster.onTick(1.0 / 60.0, 3u);
    auto snaps = snapshotsFor(net, 0);
    REQUIRE(!snaps.empty());
    CHECK(fullRecordCount(snaps[0]) >= 1u);
    CHECK(deltaRecordCount(snaps[0]) == 0u);
}

TEST_CASE("WorldBroadcaster: totalEntityCount matches buffer content", "[world_broadcaster][delta]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.onConnect(0u);

    // Tick 1 → full entries
    broadcaster.onTick(1.0 / 60.0, 1u);
    {
        auto snaps = snapshotsFor(net, 0);
        REQUIRE(!snaps.empty());
        const auto& pkt = snaps[0];
        auto hdr = parseSnapshotHeader(pkt);
        const std::size_t expectedSize =
            sizeof(fl::MsgWorldSnapshotHeader) + hdr.bitstreamBytes +
            6u; // SnapshotPeerCount TLV only (estimatedDelayTicks == 0; SnapshotPeerLatency absent)
        CHECK(pkt.size() == expectedSize);
    }
    clearSnapshots(net);

    // Tick 2 → update entries
    broadcaster.onTick(1.0 / 60.0, 2u);
    {
        auto snaps = snapshotsFor(net, 0);
        REQUIRE(!snaps.empty());
        const auto& pkt = snaps[0];
        auto hdr = parseSnapshotHeader(pkt);
        const std::size_t expectedSize = sizeof(fl::MsgWorldSnapshotHeader) + hdr.bitstreamBytes + 6u;
        CHECK(pkt.size() == expectedSize);
    }
}

TEST_CASE("WorldBroadcaster: no connected peers produces no snapshot sends", "[world_broadcaster][interest]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    fl::EntityTransform t{};
    t.pos[1] = 500.0;
    em.spawn("builtin:debug-entity", t);

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    // No onConnect — m_peerEntities is empty; per-peer loop does nothing
    broadcaster.onTick(1.0 / 60.0, 1u);

    CHECK(net.perPeerSends.empty());
}

// ---------------------------------------------------------------------------
// SnapshotPeerLatency TLV tests (#382)
// ---------------------------------------------------------------------------

TEST_CASE("WorldBroadcaster: SnapshotPeerLatency TLV present when estimatedDelayTicks > 0",
          "[world_broadcaster][latency]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.onConnect(0u);

    // Tick 2: advance m_currentTick to 2, peer has no delay yet.
    broadcaster.onTick(1.0 / 60.0, 2u);
    clearSnapshots(net);

    // Send MsgHeartbeat with tickIndex=0 → estimatedDelayTicks = 2 - 0 = 2.
    fl::MsgHeartbeat hb{};
    hb.msgId = static_cast<uint8_t>(fl::MsgId::Heartbeat);
    hb.tickIndex = 0u;
    broadcaster.onReceive(0u, &hb, sizeof(hb));

    // Tick 3: snapshot must include SnapshotPeerLatency = 2 * 1000 / 60 = 33 ms.
    broadcaster.onTick(1.0 / 60.0, 3u);
    auto snaps = snapshotsFor(net, 0u);
    REQUIRE(!snaps.empty());
    const auto& pkt = snaps[0];

    const auto hdr = parseSnapshotHeader(pkt);
    const std::size_t extOffset = sizeof(fl::MsgWorldSnapshotHeader) + hdr.bitstreamBytes;
    REQUIRE(pkt.size() > extOffset);
    const auto* ext = pkt.data() + extOffset;
    const auto extSz = pkt.size() - extOffset;

    uint16_t pc{};
    CHECK(fl::readExtValue(ext, extSz, static_cast<uint16_t>(fl::ExtTag::SnapshotPeerCount), pc));
    CHECK(pc == 1u);

    uint16_t lat{};
    CHECK(fl::readExtValue(ext, extSz, static_cast<uint16_t>(fl::ExtTag::SnapshotPeerLatency), lat));
    CHECK(lat == static_cast<uint16_t>(2u * 1000u / 60u)); // 33 ms
}

TEST_CASE("WorldBroadcaster: SnapshotPeerLatency TLV absent when estimatedDelayTicks == 0",
          "[world_broadcaster][latency]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.onConnect(0u);

    // No heartbeat sent — estimatedDelayTicks stays 0.
    broadcaster.onTick(1.0 / 60.0, 1u);
    auto snaps = snapshotsFor(net, 0u);
    REQUIRE(!snaps.empty());
    const auto& pkt = snaps[0];

    const auto hdr = parseSnapshotHeader(pkt);
    const std::size_t extOffset = sizeof(fl::MsgWorldSnapshotHeader) + hdr.bitstreamBytes;
    REQUIRE(pkt.size() > extOffset);
    const auto* ext = pkt.data() + extOffset;
    const auto extSz = pkt.size() - extOffset;

    // SnapshotPeerCount must be present.
    uint16_t pc{};
    CHECK(fl::readExtValue(ext, extSz, static_cast<uint16_t>(fl::ExtTag::SnapshotPeerCount), pc));

    // SnapshotPeerLatency must NOT be present.
    uint16_t lat{};
    CHECK_FALSE(fl::readExtValue(ext, extSz, static_cast<uint16_t>(fl::ExtTag::SnapshotPeerLatency), lat));

    // Packet size: header + quantized bitstream + 6 bytes (SnapshotPeerCount TLV only).
    const std::size_t expected = sizeof(fl::MsgWorldSnapshotHeader) + hdr.bitstreamBytes + 6u;
    CHECK(pkt.size() == expected);
}

TEST_CASE("WorldBroadcaster: snapshot entity record carries omega field without corruption",
          "[world_broadcaster][omega]") {
    // This test verifies the code path: FlightState.omega → TelemetryEntry.omega →
    // EntitySnap.omega → MsgEntityEntry/MsgEntityUpdate.omega.
    // The round-trip VALUE check (serialise → memcpy → verify exact values) is covered by
    // test_game_protocol.cpp.  Here we just confirm the field is present in the packet,
    // is finite (not NaN/inf), and that the packet has the correct size for the new struct.
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.onConnect(0u);

    // One tick is enough to verify the code path. omega starts at {0,0,0} and we just
    // check the field is accessible and finite in the serialized packet.
    fl::MsgClientInput inp{};
    inp.msgId = static_cast<uint8_t>(fl::MsgId::ClientInput);
    inp.protocolVersion = fl::kProtocolVersion;
    inp.seqNum = 1u;
    inp.tickIndex = 0u;
    inp.throttle = 1.0f;
    broadcaster.onReceive(0u, &inp, sizeof(inp));
    broadcaster.onTick(1.0 / 60.0, 1u);

    auto snaps = snapshotsFor(net, 0u);
    REQUIRE(!snaps.empty());
    const auto& pkt = snaps[0];

    const auto hdr = parseSnapshotHeader(pkt);
    REQUIRE(fullRecordCount(pkt) >= 1u); // first tick always sends a full record
    CHECK(hdr.bitstreamBytes > 0u);

    const auto entries = decodeEntities(pkt);
    REQUIRE(!entries.empty());
    const DecodedEntity& entry = entries[0]; // the peer's own entity carries omega

    // omega field is accessible; values must be finite (not NaN/inf from a bad cast/quantize).
    CHECK(std::isfinite(entry.omega[0]));
    CHECK(std::isfinite(entry.omega[1]));
    CHECK(std::isfinite(entry.omega[2]));
}

TEST_CASE("WorldBroadcaster: snapshot includes SnapshotPeerDelayTicks TLV when delay > 0",
          "[world_broadcaster][latency]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.onConnect(0u);
    broadcaster.onTick(1.0 / 60.0, 2u);
    clearSnapshots(net);

    // Send MsgHeartbeat with tickIndex=0 → estimatedDelayTicks = 2 - 0 = 2.
    fl::MsgHeartbeat hb{};
    hb.msgId = static_cast<uint8_t>(fl::MsgId::Heartbeat);
    hb.tickIndex = 0u;
    broadcaster.onReceive(0u, &hb, sizeof(hb));

    broadcaster.onTick(1.0 / 60.0, 3u);
    auto snaps = snapshotsFor(net, 0u);
    REQUIRE(!snaps.empty());
    const auto& pkt = snaps[0];

    const auto hdr = parseSnapshotHeader(pkt);
    const std::size_t extOffset = sizeof(fl::MsgWorldSnapshotHeader) + hdr.bitstreamBytes;
    REQUIRE(pkt.size() > extOffset);
    const auto* ext = pkt.data() + extOffset;
    const auto extSz = pkt.size() - extOffset;

    // Both SnapshotPeerLatency and SnapshotPeerDelayTicks must be present and consistent.
    uint16_t lat{};
    REQUIRE(fl::readExtValue(ext, extSz, static_cast<uint16_t>(fl::ExtTag::SnapshotPeerLatency), lat));
    CHECK(lat == static_cast<uint16_t>(2u * 1000u / 60u)); // 33 ms

    uint16_t delayTicks{};
    REQUIRE(fl::readExtValue(ext, extSz, static_cast<uint16_t>(fl::ExtTag::SnapshotPeerDelayTicks), delayTicks));
    CHECK(delayTicks == 2u);
}

// ---------------------------------------------------------------------------
// JitterBuffer unit tests
// ---------------------------------------------------------------------------

TEST_CASE("JitterBuffer: pop on empty buffer returns false", "[jitter_buffer]") {
    fl::JitterBuffer buf;
    fl::BufferedInput out{};
    out.throttle = 0.5f;
    CHECK_FALSE(buf.pop(out));
    CHECK(out.throttle == 0.5f); // unchanged
    CHECK(buf.empty());
    CHECK(buf.size() == 0u);
}

TEST_CASE("JitterBuffer: push then pop returns same values", "[jitter_buffer]") {
    fl::JitterBuffer buf{4};
    fl::BufferedInput in{};
    in.throttle = 0.8f;
    in.elevator = -0.3f;
    in.aileron = 0.1f;
    in.rudder = -0.05f;
    in.buttons = 0x03u;
    buf.push(in);
    CHECK(buf.size() == 1u);

    fl::BufferedInput out{};
    REQUIRE(buf.pop(out));
    CHECK(out.throttle == 0.8f);
    CHECK(out.elevator == -0.3f);
    CHECK(out.aileron == 0.1f);
    CHECK(out.rudder == -0.05f);
    CHECK(out.buttons == 0x03u);
    CHECK(buf.empty());
}

TEST_CASE("JitterBuffer: FIFO ordering", "[jitter_buffer]") {
    fl::JitterBuffer buf{8};
    for (uint32_t i = 0; i < 3; ++i) {
        fl::BufferedInput in{};
        in.throttle = static_cast<float>(i + 1) * 0.1f;
        buf.push(in);
    }
    CHECK(buf.size() == 3u);

    for (uint32_t i = 0; i < 3; ++i) {
        fl::BufferedInput out{};
        REQUIRE(buf.pop(out));
        CHECK(out.throttle == Catch::Approx(static_cast<float>(i + 1) * 0.1f));
    }
    CHECK(buf.empty());
}

TEST_CASE("JitterBuffer: overflow drops oldest", "[jitter_buffer]") {
    fl::JitterBuffer buf{2};
    fl::BufferedInput a{}, b{}, c{};
    a.throttle = 0.1f;
    b.throttle = 0.2f;
    c.throttle = 0.3f;
    buf.push(a);
    buf.push(b);
    buf.push(c); // overflow: a is dropped
    CHECK(buf.size() == 2u);

    fl::BufferedInput out{};
    REQUIRE(buf.pop(out));
    CHECK(out.throttle == Catch::Approx(0.2f)); // b first
    REQUIRE(buf.pop(out));
    CHECK(out.throttle == Catch::Approx(0.3f)); // c second
    CHECK(buf.empty());
}

TEST_CASE("JitterBuffer: size tracks correctly with interleaved push and pop", "[jitter_buffer]") {
    fl::JitterBuffer buf{4};
    fl::BufferedInput dummy{};
    buf.push(dummy);
    buf.push(dummy);
    CHECK(buf.size() == 2u);
    buf.pop(dummy);
    CHECK(buf.size() == 1u);
    buf.push(dummy);
    buf.push(dummy);
    CHECK(buf.size() == 3u);
    buf.pop(dummy);
    buf.pop(dummy);
    buf.pop(dummy);
    CHECK(buf.empty());
}

TEST_CASE("JitterBuffer: setMaxDepth truncates when smaller than current size", "[jitter_buffer]") {
    fl::JitterBuffer buf{4};
    fl::BufferedInput in{};
    for (uint32_t i = 0; i < 4; ++i) {
        in.throttle = static_cast<float>(i + 1) * 0.1f;
        buf.push(in);
    }
    CHECK(buf.size() == 4u);
    buf.setMaxDepth(2);
    CHECK(buf.size() == 2u);
    CHECK(buf.maxDepth() == 2u);

    // Oldest two (0.1, 0.2) were dropped; remaining are 0.3, 0.4 in order.
    fl::BufferedInput out{};
    REQUIRE(buf.pop(out));
    CHECK(out.throttle == Catch::Approx(0.3f));
    REQUIRE(buf.pop(out));
    CHECK(out.throttle == Catch::Approx(0.4f));
}

TEST_CASE("JitterBuffer: setMaxDepth to 0 is clamped to 1", "[jitter_buffer]") {
    fl::JitterBuffer buf{4};
    buf.setMaxDepth(0u);
    CHECK(buf.maxDepth() == 1u);
}

TEST_CASE("JitterBuffer: ring index wraps correctly at kHardMaxDepth", "[jitter_buffer]") {
    fl::JitterBuffer buf{fl::JitterBuffer::kHardMaxDepth};
    // Push kHardMaxDepth + 3 items; oldest 3 are dropped by overflow.
    for (uint32_t i = 0; i < fl::JitterBuffer::kHardMaxDepth + 3u; ++i) {
        fl::BufferedInput in{};
        in.throttle = static_cast<float>(i) * 0.01f;
        buf.push(in);
    }
    CHECK(buf.size() == fl::JitterBuffer::kHardMaxDepth);

    // Pop all; throttle values should be sequential starting from index 3.
    for (uint32_t i = 0; i < fl::JitterBuffer::kHardMaxDepth; ++i) {
        fl::BufferedInput out{};
        REQUIRE(buf.pop(out));
        CHECK(out.throttle == Catch::Approx(static_cast<float>(i + 3u) * 0.01f));
    }
    CHECK(buf.empty());
}

// ---------------------------------------------------------------------------
// WorldBroadcaster jitter buffer integration tests
// ---------------------------------------------------------------------------

static fl::MsgClientInput makeJitterInput(uint32_t seqNum, float throttle, uint64_t tickIndex = 0u) {
    fl::MsgClientInput inp{};
    inp.msgId = static_cast<uint8_t>(fl::MsgId::ClientInput);
    inp.protocolVersion = fl::kProtocolVersion;
    inp.seqNum = seqNum;
    inp.tickIndex = tickIndex;
    inp.throttle = throttle;
    inp.viewAxis[0] = 1.f;
    return inp;
}

TEST_CASE("WorldBroadcaster: received input is buffered and not applied until tick",
          "[world_broadcaster][jitter_buffer]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.onConnect(0u);

    auto inp = makeJitterInput(1u, 0.9f);
    broadcaster.onReceive(0u, &inp, sizeof(inp));

    // Buffer should hold 1 item before any tick drains it.
    uint32_t gotDepth = 0u;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) { gotDepth = pi.queueDepth; });
    CHECK(gotDepth == 1u);
}

TEST_CASE("WorldBroadcaster: jitter buffer drains one per tick", "[world_broadcaster][jitter_buffer]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setJitterBufferDepth(8u);
    broadcaster.onConnect(0u);

    // Advance to tick 10 so the first input gets estimatedDelayTicks=10 → buffer depth=min(10,8)=8.
    broadcaster.onTick(1.0 / 60.0, 10u);

    // Push 3 inputs with distinct seqNums (tickIndex=0 so delay=10).
    for (uint32_t i = 0; i < 3u; ++i) {
        auto inp = makeJitterInput(i + 1u, static_cast<float>(i + 1u) * 0.2f);
        broadcaster.onReceive(0u, &inp, sizeof(inp));
    }

    for (uint32_t expected = 3u; expected > 0u; --expected) {
        uint32_t gotDepth = 0xFFu;
        broadcaster.forEachPeer([&](const fl::PeerInfo& pi) { gotDepth = pi.queueDepth; });
        CHECK(gotDepth == expected);
        broadcaster.onTick(1.0 / 60.0, expected);
    }
    // After 3 ticks the buffer is empty.
    uint32_t finalDepth = 0xFFu;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) { finalDepth = pi.queueDepth; });
    CHECK(finalDepth == 0u);
}

TEST_CASE("WorldBroadcaster: empty buffer tick uses stale repeat without crash", "[world_broadcaster][jitter_buffer]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.onConnect(0u);

    auto inp = makeJitterInput(1u, 0.5f);
    broadcaster.onReceive(0u, &inp, sizeof(inp));

    // Tick 1: drains the one buffered input.
    broadcaster.onTick(1.0 / 60.0, 1u);

    // Tick 2: buffer is empty — stale repeat; entity must remain live (no crash).
    broadcaster.onTick(1.0 / 60.0, 2u);

    fl::EntityId eid;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) { eid = pi.eid; });
    CHECK(eid.valid());
}

TEST_CASE("WorldBroadcaster: forEachPeer reports queueDepth", "[world_broadcaster][jitter_buffer]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setJitterBufferDepth(8u);
    broadcaster.onConnect(0u);

    // Advance to tick 10 so the first input seeds buffer depth=min(10,8)=8.
    broadcaster.onTick(1.0 / 60.0, 10u);

    auto i1 = makeJitterInput(1u, 0.3f);
    auto i2 = makeJitterInput(2u, 0.6f);
    broadcaster.onReceive(0u, &i1, sizeof(i1));
    broadcaster.onReceive(0u, &i2, sizeof(i2));

    uint32_t gotDepth = 0u;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) { gotDepth = pi.queueDepth; });
    CHECK(gotDepth == 2u);
}

TEST_CASE("WorldBroadcaster: jitter buffer depth seeded from estimatedDelayTicks",
          "[world_broadcaster][jitter_buffer]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setJitterBufferDepth(8u); // global max = 8
    broadcaster.onConnect(0u);

    // Advance to tick 10 so delay = 10 - 5 = 5 ticks.
    broadcaster.onTick(1.0 / 60.0, 10u);
    clearSnapshots(net);

    // First input seeds buffer depth = min(5, 8) = 5.
    auto inp = makeJitterInput(1u, 0.5f, 5u);
    broadcaster.onReceive(0u, &inp, sizeof(inp));

    // Push 5 more inputs to fill the buffer; the 6th should overflow (depth=5).
    for (uint32_t i = 2u; i <= 6u; ++i) {
        auto extra = makeJitterInput(i, 0.1f * static_cast<float>(i));
        broadcaster.onReceive(0u, &extra, sizeof(extra));
    }
    // Buffer should hold exactly 5 (depth cap).
    uint32_t gotDepth = 0u;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) { gotDepth = pi.queueDepth; });
    CHECK(gotDepth == 5u);
}

TEST_CASE("WorldBroadcaster: jitter buffer depth capped at jitterMaxDepth", "[world_broadcaster][jitter_buffer]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setJitterBufferDepth(2u); // hard cap = 2
    broadcaster.onConnect(0u);

    // Advance to tick 20 so delay = 20 - 0 = 20, which would give depth 20 uncapped.
    broadcaster.onTick(1.0 / 60.0, 20u);
    clearSnapshots(net);

    // First input: delay=20 but cap=2, so depth = min(20,2) = 2.
    auto inp = makeJitterInput(1u, 0.5f, 0u);
    broadcaster.onReceive(0u, &inp, sizeof(inp));

    // Push 2 more; buffer should saturate at 2 and overflow.
    auto i2 = makeJitterInput(2u, 0.6f);
    auto i3 = makeJitterInput(3u, 0.7f);
    broadcaster.onReceive(0u, &i2, sizeof(i2));
    broadcaster.onReceive(0u, &i3, sizeof(i3)); // overflow

    uint32_t gotDepth = 0u;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) { gotDepth = pi.queueDepth; });
    CHECK(gotDepth == 2u);
}

TEST_CASE("WorldBroadcaster: jitter buffers are independent per peer", "[world_broadcaster][jitter_buffer]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setJitterBufferDepth(8u);
    net.peerAddresses[0] = "1.1.1.1:1000";
    net.peerAddresses[1] = "2.2.2.2:2000";
    broadcaster.onConnect(0u);
    broadcaster.onConnect(1u);

    // Advance to tick 10 so first input from each peer gets depth=min(10,8)=8.
    broadcaster.onTick(1.0 / 60.0, 10u);

    // Send 2 inputs to peer 0, 1 input to peer 1 (tickIndex=0 → delay=10).
    auto a1 = makeJitterInput(1u, 0.1f);
    auto a2 = makeJitterInput(2u, 0.2f);
    auto b1 = makeJitterInput(1u, 0.5f);
    broadcaster.onReceive(0u, &a1, sizeof(a1));
    broadcaster.onReceive(0u, &a2, sizeof(a2));
    broadcaster.onReceive(1u, &b1, sizeof(b1));

    std::map<uint32_t, uint32_t> depths;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) { depths[pi.peerId] = pi.queueDepth; });
    CHECK(depths[0u] == 2u);
    CHECK(depths[1u] == 1u);
}

TEST_CASE("WorldBroadcaster: setJitterBufferDepth affects initial depth for new peers",
          "[world_broadcaster][jitter_buffer]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setJitterBufferDepth(6u);
    net.peerAddresses[0] = "1.1.1.1:1000";
    net.peerAddresses[1] = "2.2.2.2:2000";

    // Advance to tick 10 so delay estimates are non-zero.
    broadcaster.onTick(1.0 / 60.0, 10u);
    clearSnapshots(net);

    // Peer 0 connects, sends input with tickIndex=0 (delay=10, cap=6 -> depth=6).
    broadcaster.onConnect(0u);
    auto inp0 = makeJitterInput(1u, 0.5f, 0u);
    broadcaster.onReceive(0u, &inp0, sizeof(inp0));

    // Change the global max.
    broadcaster.setJitterBufferDepth(3u);

    // Peer 1 connects after the change, sends input with tickIndex=0 (delay=10, cap=3 -> depth=3).
    broadcaster.onConnect(1u);
    auto inp1 = makeJitterInput(1u, 0.5f, 0u);
    broadcaster.onReceive(1u, &inp1, sizeof(inp1));

    // Fill both buffers to their respective caps.
    for (uint32_t i = 2u; i <= 7u; ++i) {
        auto extra = makeJitterInput(i, 0.1f);
        broadcaster.onReceive(0u, &extra, sizeof(extra));
    }
    for (uint32_t i = 2u; i <= 4u; ++i) {
        auto extra = makeJitterInput(i, 0.1f);
        broadcaster.onReceive(1u, &extra, sizeof(extra));
    }

    std::map<uint32_t, uint32_t> depths;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) { depths[pi.peerId] = pi.queueDepth; });
    // Peer 0 was seeded with depth=6 before the change.
    CHECK(depths[0u] == 6u);
    // Peer 1 was seeded with depth=3 after the change.
    CHECK(depths[1u] == 3u);
}

// ---------------------------------------------------------------------------
// Adaptive jitter buffer tests (#424 + #429)
// ---------------------------------------------------------------------------

TEST_CASE("WorldBroadcaster: EWMA delay seeded from first estimatedDelayTicks", "[world_broadcaster][jitter_buffer]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setJitterBufferDepth(32u);
    broadcaster.setJitterAdaptWindow(2u); // alpha=0.5, fast convergence
    broadcaster.setJitterHysteresis(0u);  // resize on any diff
    broadcaster.setJitterMultiplier(0.f); // delay-only
    broadcaster.onConnect(0u);

    // Advance to tick 6 so first input delay = 6 - 1 = 5.
    broadcaster.onTick(1.0 / 60.0, 6u);
    clearSnapshots(net);

    auto inp = makeJitterInput(1u, 0.5f, 1u);
    broadcaster.onReceive(0u, &inp, sizeof(inp));

    // After seeding: EWMA = 5, depth = 5. Verify via forEachPeer.
    float gotEwma = -1.f;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) { gotEwma = pi.ewmaDelayTicks; });
    CHECK(gotEwma == Catch::Approx(5.f));
}

TEST_CASE("WorldBroadcaster: EWMA delay converges toward new samples", "[world_broadcaster][jitter_buffer]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setJitterBufferDepth(32u);
    broadcaster.setJitterAdaptWindow(2u); // alpha=0.5
    broadcaster.setJitterHysteresis(0u);
    broadcaster.setJitterMultiplier(0.f);
    broadcaster.onConnect(0u);

    // Seed at delay=2 (tick=3, tickIndex=1).
    broadcaster.onTick(1.0 / 60.0, 3u);
    clearSnapshots(net);
    auto inp0 = makeJitterInput(1u, 0.5f, 1u);
    broadcaster.onReceive(0u, &inp0, sizeof(inp0));

    // Send many inputs at delay=20 (tickIndex=0, server still at tick 3 after onTick above).
    // EWMA update: tick stays at 3 but subsequent inputs use seqNums to track.
    // Send more inputs advancing server to tick 23 so delay = 23 - 1 = 22 each time.
    for (uint32_t seq = 2u; seq <= 12u; ++seq) {
        broadcaster.onTick(1.0 / 60.0, static_cast<uint64_t>(3u + seq));
        clearSnapshots(net);
        auto inp = makeJitterInput(seq, 0.5f, 1u); // tickIndex=1, delay grows with server tick
        broadcaster.onReceive(0u, &inp, sizeof(inp));
    }

    // After 10 updates at large delay, EWMA has moved well above 2.
    float gotEwma = -1.f;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) { gotEwma = pi.ewmaDelayTicks; });
    CHECK(gotEwma > 5.f); // well above initial seed
}

TEST_CASE("WorldBroadcaster: adaptive resize grows buffer when EWMA delay increases",
          "[world_broadcaster][jitter_buffer]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setJitterBufferDepth(32u);
    broadcaster.setJitterAdaptWindow(2u); // alpha=0.5, 8 samples for ~99% convergence
    broadcaster.setJitterHysteresis(0u);
    broadcaster.setJitterMultiplier(0.f);
    broadcaster.onConnect(0u);

    // Seed at delay=2 (depth=2).
    broadcaster.onTick(1.0 / 60.0, 3u);
    clearSnapshots(net);
    auto inp0 = makeJitterInput(1u, 0.5f, 1u);
    broadcaster.onReceive(0u, &inp0, sizeof(inp0));
    broadcaster.onTick(1.0 / 60.0, 4u); // resize check: target=2, current=2, no resize
    clearSnapshots(net);

    // Now send inputs at high delay (tickIndex=0 so delay = serverTick - 0 = serverTick).
    // With alpha=0.5 and 10 updates at delay=12, EWMA approaches 12 asymptotically.
    for (uint32_t seq = 2u; seq <= 12u; ++seq) {
        auto inp = makeJitterInput(seq, 0.5f, 0u);
        broadcaster.onReceive(0u, &inp, sizeof(inp));
        broadcaster.onTick(1.0 / 60.0, static_cast<uint64_t>(4u + seq));
        clearSnapshots(net);
    }

    // Buffer should have grown from 2 to at least 6 (EWMA ~= 4 after 10 iterations seeded at 2).
    // Push 30 inputs and check queueDepth cap reflects growth.
    for (uint32_t seq = 13u; seq <= 42u; ++seq) {
        auto inp = makeJitterInput(seq, 0.5f, 0u);
        broadcaster.onReceive(0u, &inp, sizeof(inp));
    }
    uint32_t finalMax = 0u;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) { finalMax = pi.bufferMaxDepth; });
    CHECK(finalMax > 2u);
}

TEST_CASE("WorldBroadcaster: adaptive resize shrinks buffer when delay drops", "[world_broadcaster][jitter_buffer]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setJitterBufferDepth(32u);
    broadcaster.setJitterAdaptWindow(2u);
    broadcaster.setJitterHysteresis(0u);
    broadcaster.setJitterMultiplier(0.f);
    broadcaster.onConnect(0u);

    // Seed at delay=12 (depth=12).
    broadcaster.onTick(1.0 / 60.0, 13u);
    clearSnapshots(net);
    auto inp0 = makeJitterInput(1u, 0.5f, 1u);
    broadcaster.onReceive(0u, &inp0, sizeof(inp0));
    broadcaster.onTick(1.0 / 60.0, 14u);
    clearSnapshots(net);

    // Now drive delay to 1 with many updates (tickIndex = serverTick-1 each time).
    for (uint32_t seq = 2u; seq <= 20u; ++seq) {
        uint64_t tick = static_cast<uint64_t>(14u + seq);
        auto inp = makeJitterInput(seq, 0.5f, tick - 1u);
        broadcaster.onReceive(0u, &inp, sizeof(inp));
        broadcaster.onTick(1.0 / 60.0, tick);
        clearSnapshots(net);
    }

    uint32_t finalMax = 0u;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) { finalMax = pi.bufferMaxDepth; });
    CHECK(finalMax < 12u); // buffer shrank
    CHECK(finalMax >= 1u); // never below floor
}

TEST_CASE("WorldBroadcaster: hysteresis prevents resize for small EWMA drift", "[world_broadcaster][jitter_buffer]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setJitterBufferDepth(32u);
    broadcaster.setJitterAdaptWindow(2u);
    broadcaster.setJitterHysteresis(8u); // large dead-band
    broadcaster.setJitterMultiplier(0.f);
    broadcaster.onConnect(0u);

    // Seed at delay=5 (depth=5).
    broadcaster.onTick(1.0 / 60.0, 6u);
    clearSnapshots(net);
    auto inp0 = makeJitterInput(1u, 0.5f, 1u);
    broadcaster.onReceive(0u, &inp0, sizeof(inp0));
    broadcaster.onTick(1.0 / 60.0, 7u);
    clearSnapshots(net);

    // Send many inputs at delay=7 — EWMA drifts toward 7, but |7-5|=2 < hysteresis=8 → no resize.
    for (uint32_t seq = 2u; seq <= 16u; ++seq) {
        uint64_t tick = static_cast<uint64_t>(7u + seq);
        auto inp = makeJitterInput(seq, 0.5f, tick - 7u);
        broadcaster.onReceive(0u, &inp, sizeof(inp));
        broadcaster.onTick(1.0 / 60.0, tick);
        clearSnapshots(net);
    }

    uint32_t finalMax = 0u;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) { finalMax = pi.bufferMaxDepth; });
    CHECK(finalMax == 5u); // unchanged due to hysteresis
}

TEST_CASE("WorldBroadcaster: adaptive resize clamped at jitterMaxDepth", "[world_broadcaster][jitter_buffer]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setJitterBufferDepth(6u); // global cap = 6
    broadcaster.setJitterAdaptWindow(2u);
    broadcaster.setJitterHysteresis(0u);
    broadcaster.setJitterMultiplier(0.f);
    broadcaster.onConnect(0u);

    // Seed at delay=30 — capped to 6 at seeding.
    broadcaster.onTick(1.0 / 60.0, 31u);
    clearSnapshots(net);
    auto inp0 = makeJitterInput(1u, 0.5f, 1u);
    broadcaster.onReceive(0u, &inp0, sizeof(inp0));

    // Drive EWMA toward 30 (delay stays large).
    for (uint32_t seq = 2u; seq <= 12u; ++seq) {
        auto inp = makeJitterInput(seq, 0.5f, 1u);
        broadcaster.onReceive(0u, &inp, sizeof(inp));
        broadcaster.onTick(1.0 / 60.0, static_cast<uint64_t>(31u + seq));
        clearSnapshots(net);
    }

    uint32_t finalMax = 0u;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) { finalMax = pi.bufferMaxDepth; });
    CHECK(finalMax == 6u); // capped at global max
}

TEST_CASE("WorldBroadcaster: setJitterMultiplier 0 gives delay-only depth", "[world_broadcaster][jitter_buffer]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setJitterBufferDepth(32u);
    broadcaster.setJitterAdaptWindow(2u);
    broadcaster.setJitterHysteresis(0u);
    broadcaster.setJitterMultiplier(0.f); // pure delay-only
    broadcaster.onConnect(0u);

    // Seed at delay=4 (depth=4).
    broadcaster.onTick(1.0 / 60.0, 5u);
    clearSnapshots(net);
    // Send inputs at irregular spacings to build up jitter EWMA.
    auto inp0 = makeJitterInput(1u, 0.5f, 1u);
    broadcaster.onReceive(0u, &inp0, sizeof(inp0));

    broadcaster.onTick(1.0 / 60.0, 6u);
    clearSnapshots(net);
    // Skip a few ticks to create jitter, then send at delay=4.
    auto inp1 = makeJitterInput(2u, 0.5f, 2u);
    broadcaster.onReceive(0u, &inp1, sizeof(inp1));
    broadcaster.onTick(1.0 / 60.0, 7u);
    clearSnapshots(net);

    // Even with jitter EWMA > 0, multiplier=0 means it has no effect on target.
    // EWMA delay ≈ 4 (delay stayed at ~4), so target should remain 4.
    float gotJitter = -1.f;
    uint32_t gotMax = 0u;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) {
        gotJitter = pi.ewmaJitterTicks;
        gotMax = pi.bufferMaxDepth;
    });
    CHECK(gotMax == 4u);
    (void)gotJitter; // jitter EWMA may be non-zero but has no effect on depth
}

TEST_CASE("WorldBroadcaster: forEachPeer PeerInfo carries bufferMaxDepth after adaptive resize",
          "[world_broadcaster][jitter_buffer]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setJitterBufferDepth(32u);
    broadcaster.setJitterAdaptWindow(2u);
    broadcaster.setJitterHysteresis(0u);
    broadcaster.setJitterMultiplier(0.f);
    broadcaster.onConnect(0u);

    // Seed at delay=2.
    broadcaster.onTick(1.0 / 60.0, 3u);
    clearSnapshots(net);
    auto inp0 = makeJitterInput(1u, 0.5f, 1u);
    broadcaster.onReceive(0u, &inp0, sizeof(inp0));

    // Drive EWMA to ~10 and let onTick resize.
    for (uint32_t seq = 2u; seq <= 16u; ++seq) {
        auto inp = makeJitterInput(seq, 0.5f, 0u);
        broadcaster.onReceive(0u, &inp, sizeof(inp));
        broadcaster.onTick(1.0 / 60.0, static_cast<uint64_t>(3u + seq));
        clearSnapshots(net);
    }

    uint32_t gotMax = 0u;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) { gotMax = pi.bufferMaxDepth; });
    CHECK(gotMax > 2u); // PeerInfo reflects updated bufferMaxDepth
}

TEST_CASE("WorldBroadcaster: adaptive resize skips peer with no EWMA sample", "[world_broadcaster][jitter_buffer]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setJitterAdaptWindow(2u);
    broadcaster.setJitterHysteresis(0u);
    broadcaster.setJitterMultiplier(0.f);
    broadcaster.onConnect(0u);

    // onTick without any input from this peer — must not crash.
    broadcaster.onTick(1.0 / 60.0, 1u);
    clearSnapshots(net);

    // forEachPeer should still work; EWMA fields default to zero.
    float gotEwma = -1.f;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) { gotEwma = pi.ewmaDelayTicks; });
    CHECK(gotEwma == 0.f);
}

TEST_CASE("WorldBroadcaster: adaptive resize floors target at 1 with zero delay",
          "[world_broadcaster][jitter_buffer]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setJitterBufferDepth(32u);
    broadcaster.setJitterAdaptWindow(2u);
    broadcaster.setJitterHysteresis(0u);
    broadcaster.setJitterMultiplier(0.f);
    broadcaster.onConnect(0u);

    // First input at delay=0 (tickIndex == m_currentTick after onTick(1)).
    broadcaster.onTick(1.0 / 60.0, 1u);
    clearSnapshots(net);
    auto inp = makeJitterInput(1u, 0.5f, 1u); // tickIndex=1, m_currentTick=1, delay=0
    broadcaster.onReceive(0u, &inp, sizeof(inp));

    // Resize check: EWMA=0, target = clamp(ceil(0),1,32) = 1.
    broadcaster.onTick(1.0 / 60.0, 2u);
    clearSnapshots(net);

    uint32_t gotMax = 0u;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) { gotMax = pi.bufferMaxDepth; });
    CHECK(gotMax == 1u);
}

TEST_CASE("WorldBroadcaster: jitter EWMA stays near zero for regular 1-tick-spaced inputs",
          "[world_broadcaster][jitter_buffer]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setJitterAdaptWindow(4u);
    broadcaster.setJitterMultiplier(1.f);
    broadcaster.onConnect(0u);

    // Send 20 inputs, each exactly 1 server tick apart.
    for (uint32_t seq = 1u; seq <= 20u; ++seq) {
        broadcaster.onTick(1.0 / 60.0, static_cast<uint64_t>(seq));
        clearSnapshots(net);
        auto inp = makeJitterInput(seq, 0.5f, 0u);
        broadcaster.onReceive(0u, &inp, sizeof(inp));
    }

    float gotJitter = 1.f;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) { gotJitter = pi.ewmaJitterTicks; });
    // Inputs arrive exactly 1 tick apart → deviation = |1 - 1| = 0 each time → EWMA stays 0.
    CHECK(gotJitter == Catch::Approx(0.f).margin(0.01f));
}

TEST_CASE("WorldBroadcaster: jitter EWMA grows for irregular arrivals", "[world_broadcaster][jitter_buffer]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setJitterAdaptWindow(4u); // alpha=0.25
    broadcaster.setJitterMultiplier(1.f);
    broadcaster.onConnect(0u);

    // Seed at tick 1.
    broadcaster.onTick(1.0 / 60.0, 1u);
    clearSnapshots(net);
    auto inp0 = makeJitterInput(1u, 0.5f, 0u);
    broadcaster.onReceive(0u, &inp0, sizeof(inp0));

    // Now send at irregular spacings: ticks 2, 4, 5, 9, 10 (gaps of 2, 1, 4, 1).
    uint64_t irregularTicks[] = {2u, 4u, 5u, 9u, 10u};
    uint32_t seq = 2u;
    for (uint64_t tick : irregularTicks) {
        broadcaster.onTick(1.0 / 60.0, tick);
        clearSnapshots(net);
        auto inp = makeJitterInput(seq++, 0.5f, 0u);
        broadcaster.onReceive(0u, &inp, sizeof(inp));
    }

    float gotJitter = 0.f;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) { gotJitter = pi.ewmaJitterTicks; });
    CHECK(gotJitter > 0.f); // irregular arrivals → non-zero jitter EWMA
}

TEST_CASE("WorldBroadcaster: adaptive resize shrinks buffer and drops excess fill",
          "[world_broadcaster][jitter_buffer]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setJitterBufferDepth(32u);
    broadcaster.setJitterAdaptWindow(2u); // alpha=0.5
    broadcaster.setJitterHysteresis(0u);
    broadcaster.setJitterMultiplier(0.f);
    broadcaster.onConnect(0u);

    // Seed at delay=10 (depth=10).
    broadcaster.onTick(1.0 / 60.0, 11u);
    clearSnapshots(net);
    auto inp0 = makeJitterInput(1u, 0.5f, 1u);
    broadcaster.onReceive(0u, &inp0, sizeof(inp0));

    // Fill to 8 entries.
    for (uint32_t seq = 2u; seq <= 8u; ++seq) {
        auto inp = makeJitterInput(seq, 0.5f, 0u);
        broadcaster.onReceive(0u, &inp, sizeof(inp));
    }

    // Verify fill=8, max=10.
    uint32_t preFill = 0u, preMax = 0u;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) {
        preFill = pi.queueDepth;
        preMax = pi.bufferMaxDepth;
    });
    CHECK(preFill == 8u);
    CHECK(preMax == 10u);

    // Now drive EWMA to 3 by sending at delay=3 repeatedly.
    for (uint32_t seq = 9u; seq <= 22u; ++seq) {
        uint64_t tick = static_cast<uint64_t>(11u + seq);
        auto inp = makeJitterInput(seq, 0.5f, tick - 3u);
        broadcaster.onReceive(0u, &inp, sizeof(inp));
        broadcaster.onTick(1.0 / 60.0, tick);
        clearSnapshots(net);
    }

    // After resize to 3, buffer fill must have been truncated to at most 3.
    uint32_t postFill = 0u, postMax = 0u;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) {
        postFill = pi.queueDepth;
        postMax = pi.bufferMaxDepth;
    });
    CHECK(postMax < 10u);       // shrank
    CHECK(postFill <= postMax); // fill never exceeds new max
}

TEST_CASE("WorldBroadcaster: applyConfig wires jitterAdaptWindow hysteresis multiplier",
          "[world_broadcaster][jitter_buffer]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    fl::WorldBroadcasterConfig cfg;
    cfg.jitterBufferMaxDepth = 32u;
    cfg.jitterAdaptWindow = 2u; // fast convergence
    cfg.jitterHysteresis = 0u;  // resize immediately
    cfg.jitterMultiplier = 0.f; // delay-only
    broadcaster.applyConfig(cfg);

    broadcaster.onConnect(0u);

    // Seed at delay=2, then drive to delay=10; adaptive resize should fire.
    broadcaster.onTick(1.0 / 60.0, 3u);
    clearSnapshots(net);
    auto inp0 = makeJitterInput(1u, 0.5f, 1u);
    broadcaster.onReceive(0u, &inp0, sizeof(inp0));

    for (uint32_t seq = 2u; seq <= 14u; ++seq) {
        auto inp = makeJitterInput(seq, 0.5f, 0u);
        broadcaster.onReceive(0u, &inp, sizeof(inp));
        broadcaster.onTick(1.0 / 60.0, static_cast<uint64_t>(3u + seq));
        clearSnapshots(net);
    }

    uint32_t finalMax = 0u;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) { finalMax = pi.bufferMaxDepth; });
    CHECK(finalMax > 2u); // config was applied: adapt window + hysteresis drove resize
}

// ---------------------------------------------------------------------------
// Priority/budget snapshot scheduler (#516)
// ---------------------------------------------------------------------------

// Read the SnapshotDespawn TLV (uint32[] of removed indices) from a snapshot packet.
static std::vector<uint32_t> decodeDespawns(const std::vector<uint8_t>& pkt) {
    std::vector<uint32_t> ids;
    fl::MsgWorldSnapshotHeader hdr = parseSnapshotHeader(pkt);
    const std::size_t extOffset = sizeof(hdr) + hdr.bitstreamBytes;
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

TEST_CASE("WorldBroadcaster: snapshot budget caps records and always includes own entity",
          "[world_broadcaster][interest][budget]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    // Spawn 40 entities clustered near the origin so they're all within interest.
    for (int i = 0; i < 40; ++i) {
        fl::EntityTransform t{};
        t.pos[0] = i * 5.0; // within a few hundred metres
        t.pos[1] = 500.0;
        em.spawn("builtin:debug-entity", t);
    }

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setDrawDistance(200.f);
    broadcaster.setSnapshotBudget(200u); // tiny budget: only a handful of ~24-31 B records fit
    broadcaster.onConnect(0u);
    broadcaster.onTick(1.0 / 60.0, 1u);

    auto snaps = snapshotsFor(net, 0);
    REQUIRE(!snaps.empty());
    auto hdr = parseSnapshotHeader(snaps.back());
    // Budget bounds the record count well below the 41 visible (40 + peer).
    CHECK(totalEntityCount(hdr) >= 1u);
    CHECK(totalEntityCount(hdr) < 41u);

    // The peer's own entity is always present (prediction reconciliation needs it).
    fl::MsgConnectAck ack{};
    for (const auto& [pid, pkt] : net.perPeerSends)
        if (pid == 0u && !pkt.empty() && pkt[0] == static_cast<uint8_t>(fl::MsgId::ConnectAck))
            std::memcpy(&ack, pkt.data(), sizeof(ack));
    bool sawOwn = false;
    for (const auto& e : decodeEntities(snaps.back()))
        if (e.entityIdx == ack.assignedEntityIdx)
            sawOwn = true;
    CHECK(sawOwn);
}

TEST_CASE("WorldBroadcaster: budget==0 sends every visible entity (legacy path)",
          "[world_broadcaster][interest][budget]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    for (int i = 0; i < 10; ++i) {
        fl::EntityTransform t{};
        t.pos[0] = i * 5.0;
        t.pos[1] = 500.0;
        em.spawn("builtin:debug-entity", t);
    }

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setDrawDistance(200.f); // budget defaults to 0 (unlimited)
    broadcaster.onConnect(0u);
    broadcaster.onTick(1.0 / 60.0, 1u);

    auto snaps = snapshotsFor(net, 0);
    REQUIRE(!snaps.empty());
    CHECK(totalEntityCount(parseSnapshotHeader(snaps.back())) == 11u); // 10 + peer, all sent
}

TEST_CASE("WorldBroadcaster: starved entity eventually included under a tight budget",
          "[world_broadcaster][interest][budget]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    std::vector<uint32_t> idxs;
    for (int i = 0; i < 30; ++i) {
        fl::EntityTransform t{};
        t.pos[0] = 100.0 + i * 10.0;
        t.pos[1] = 500.0;
        idxs.push_back(em.spawn("builtin:debug-entity", t).index);
    }

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setDrawDistance(200.f);
    broadcaster.setSnapshotBudget(160u); // only a few records per tick
    broadcaster.onConnect(0u);

    // The farthest entity (highest idx) starts low priority; over enough ticks its recency term must
    // lift it into a snapshot at least once.
    const uint32_t starved = idxs.back();
    bool everSent = false;
    for (uint64_t tick = 1; tick <= 200 && !everSent; ++tick) {
        clearSnapshots(net);
        broadcaster.onTick(1.0 / 60.0, tick);
        for (const auto& e : decodeEntities(snapshotsFor(net, 0).back()))
            if (e.entityIdx == starved)
                everSent = true;
    }
    CHECK(everSent); // anti-starvation guarantee (recency term)
}

TEST_CASE("WorldBroadcaster: killing a known entity emits a despawn TLV, interest-out does not",
          "[world_broadcaster][interest][budget]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    fl::EntityTransform t{};
    t.pos[0] = 100.0;
    t.pos[1] = 500.0;
    fl::EntityId victim = em.spawn("builtin:debug-entity", t);
    const uint32_t victimIdx = victim.index;

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setDrawDistance(200.f);
    broadcaster.onConnect(0u);

    // Tick 1: peer learns the victim.
    broadcaster.onTick(1.0 / 60.0, 1u);
    {
        bool saw = false;
        for (const auto& e : decodeEntities(snapshotsFor(net, 0).back()))
            if (e.entityIdx == victimIdx)
                saw = true;
        REQUIRE(saw);
        CHECK(decodeDespawns(snapshotsFor(net, 0).back()).empty());
    }

    // Kill the victim and tick again: an explicit despawn must be emitted.
    em.kill(victim);
    clearSnapshots(net);
    broadcaster.onTick(1.0 / 60.0, 2u);
    {
        auto despawns = decodeDespawns(snapshotsFor(net, 0).back());
        bool listed = std::find(despawns.begin(), despawns.end(), victimIdx) != despawns.end();
        CHECK(listed);
    }
}

TEST_CASE("WorldBroadcaster: entity flown out of interest is not despawned", "[world_broadcaster][interest][budget]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    // A controllable entity that we can teleport out of interest by moving it via its state.
    fl::EntityTransform t{};
    t.pos[0] = 100.0;
    t.pos[1] = 500.0;
    fl::EntityId mover = em.spawn("builtin:debug-entity", t);
    const uint32_t moverIdx = mover.index;

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setDrawDistance(1.f); // 1 km
    broadcaster.onConnect(0u);
    broadcaster.onTick(1.0 / 60.0, 1u); // peer learns the mover

    // Move the entity far outside the interest sphere (still alive in the sim).
    if (auto* st = em.get(mover)) {
        const_cast<fl::EntityState*>(st)->transform.pos[0] = 50'000.0; // 50 km away
    }
    clearSnapshots(net);
    broadcaster.onTick(1.0 / 60.0, 2u);

    // It is no longer in the snapshot, but it must NOT be despawned (it's still alive, just far) —
    // the client's retention timeout handles interest-out.
    auto pkt = snapshotsFor(net, 0).back();
    auto despawns = decodeDespawns(pkt);
    CHECK(std::find(despawns.begin(), despawns.end(), moverIdx) == despawns.end());
}

TEST_CASE("WorldBroadcaster: re-entry after retention gap forces a full record",
          "[world_broadcaster][interest][budget]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    fl::EntityTransform t{};
    t.pos[0] = 100.0;
    t.pos[1] = 500.0;
    fl::EntityId e = em.spawn("builtin:debug-entity", t);
    const uint32_t eIdx = e.index;

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setDrawDistance(200.f);
    broadcaster.onConnect(0u);

    // Tick 1: full record (first sight). After the client acks it, ticks where it stays known → deltas.
    broadcaster.onTick(1.0 / 60.0, 1u);
    auto isFullFor = [&](const std::vector<uint8_t>& pkt) {
        for (const auto& d : decodeEntities(pkt))
            if (d.entityIdx == eIdx)
                return d.isFull;
        return false;
    };
    REQUIRE(isFullFor(snapshotsFor(net, 0).back())); // first sight = full

    ackTick(broadcaster, 0u, 1u, 1u);
    clearSnapshots(net);
    broadcaster.onTick(1.0 / 60.0, 2u);
    CHECK_FALSE(isFullFor(snapshotsFor(net, 0).back())); // known + acked → delta

    // Jump the tick index far past kSnapshotRetentionTicks since lastSentTick (=2): the entity is
    // re-sent as a full record because the client may have evicted it.
    clearSnapshots(net);
    broadcaster.onTick(1.0 / 60.0, 2u + fl::kSnapshotRetentionTicks + 5u);
    CHECK(isFullFor(snapshotsFor(net, 0).back()));
}

// ---------------------------------------------------------------------------
// Adaptive send-rate / congestion response (#518)
// ---------------------------------------------------------------------------

// Build CongestionParams with a fast (every-tick) eval cadence for deterministic tests.
static fl::CongestionParams testCongestion(bool enabled = true) {
    fl::CongestionParams p = fl::makeCongestionParams(enabled, /*minSendHz=*/10.f, /*lossThreshold=*/0.02f,
                                                      /*budgetFloorBytes=*/400u);
    p.evalIntervalTicks = 1u; // step AIMD every tick so back-off/recovery is observable over few ticks
    return p;
}

static fl::PeerLinkStats lossLink(float loss) {
    fl::PeerLinkStats s;
    s.packetLoss = loss;
    return s;
}

TEST_CASE("WorldBroadcaster: congested peer is decimated, healthy peer keeps full rate",
          "[world_broadcaster][congestion]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setCongestionParams(testCongestion());
    broadcaster.onConnect(0u); // congested peer
    broadcaster.onConnect(1u); // healthy peer
    net.peerLinkStats[0] = lossLink(0.5f);
    net.peerLinkStats[1] = lossLink(0.0f);

    for (uint64_t tick = 1; tick <= 40; ++tick)
        broadcaster.onTick(1.0 / 60.0, tick);

    const auto congested = snapshotsFor(net, 0).size();
    const auto healthy = snapshotsFor(net, 1).size();
    CHECK(healthy == 40u);      // full 60 Hz: a snapshot every tick
    CHECK(congested < healthy); // decimated under sustained loss
    CHECK(congested >= 1u);     // recency still guarantees periodic sends (no starvation)
}

TEST_CASE("WorldBroadcaster: zero link stats leave every peer at the full per-tick rate",
          "[world_broadcaster][congestion]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setCongestionParams(testCongestion()); // enabled, but no link stats injected => zeros
    broadcaster.onConnect(0u);

    for (uint64_t tick = 1; tick <= 30; ++tick)
        broadcaster.onTick(1.0 / 60.0, tick);

    CHECK(snapshotsFor(net, 0).size() == 30u); // unchanged from pre-#518 behaviour
}

TEST_CASE("WorldBroadcaster: congestion shrinks the effective byte budget (fewer records)",
          "[world_broadcaster][congestion]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    // Many co-located entities so the byte budget — not interest — is the limiting factor.
    for (int i = 0; i < 40; ++i) {
        fl::EntityTransform t{};
        t.pos[1] = 500.0;
        em.spawn("builtin:debug-entity", t);
    }

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setDrawDistance(200.f);
    broadcaster.setSnapshotBudget(1200u);
    broadcaster.setCongestionParams(testCongestion());
    broadcaster.onConnect(0u); // congested
    broadcaster.onConnect(1u); // healthy (both spawn at the fallback origin => same visible set)
    net.peerLinkStats[0] = lossLink(0.5f);
    net.peerLinkStats[1] = lossLink(0.0f);

    for (uint64_t tick = 1; tick <= 50; ++tick)
        broadcaster.onTick(1.0 / 60.0, tick);

    const auto congested = snapshotsFor(net, 0);
    const auto healthy = snapshotsFor(net, 1);
    REQUIRE(!congested.empty());
    REQUIRE(!healthy.empty());
    const uint16_t congestedRecords = parseSnapshotHeader(congested.back()).recordCount;
    const uint16_t healthyRecords = parseSnapshotHeader(healthy.back()).recordCount;
    CHECK(congestedRecords < healthyRecords); // floor-budget peer carries fewer entities per snapshot
}

TEST_CASE("WorldBroadcaster: peer returns to full rate after congestion clears", "[world_broadcaster][congestion]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setCongestionParams(testCongestion());
    broadcaster.onConnect(0u);
    net.peerLinkStats[0] = lossLink(0.5f);
    for (uint64_t tick = 1; tick <= 40; ++tick) // collapse to the floor
        broadcaster.onTick(1.0 / 60.0, tick);

    net.peerLinkStats[0] = lossLink(0.0f);        // link recovers
    for (uint64_t tick = 41; tick <= 240; ++tick) // ramp back up
        broadcaster.onTick(1.0 / 60.0, tick);

    clearSnapshots(net);
    for (uint64_t tick = 241; tick <= 260; ++tick)
        broadcaster.onTick(1.0 / 60.0, tick);
    CHECK(snapshotsFor(net, 0).size() == 20u); // every tick again
}

TEST_CASE("WorldBroadcaster: congestion disabled never decimates under loss", "[world_broadcaster][congestion]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setCongestionParams(testCongestion(/*enabled=*/false));
    broadcaster.onConnect(0u);
    net.peerLinkStats[0] = lossLink(0.9f); // heavy loss, but the controller is off

    for (uint64_t tick = 1; tick <= 30; ++tick)
        broadcaster.onTick(1.0 / 60.0, tick);

    CHECK(snapshotsFor(net, 0).size() == 30u);
}

TEST_CASE("WorldBroadcaster: forEachPeer reports throttled send rate and packet loss",
          "[world_broadcaster][congestion]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setCongestionParams(testCongestion());
    broadcaster.onConnect(0u);
    net.peerLinkStats[0] = lossLink(0.5f);
    for (uint64_t tick = 1; tick <= 40; ++tick)
        broadcaster.onTick(1.0 / 60.0, tick);

    float rate = 60.f;
    float loss = -1.f;
    broadcaster.forEachPeer([&](const fl::PeerInfo& pi) {
        rate = pi.sendRateHz;
        loss = pi.packetLoss;
    });
    CHECK(rate < 60.f);                 // decimated
    CHECK(loss == Catch::Approx(0.5f)); // live ENet loss surfaced
}
