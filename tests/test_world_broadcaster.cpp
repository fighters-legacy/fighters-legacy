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
#include "net/GameProtocol.h"
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
#include <string>
#include <unordered_set>
#include <vector>

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

// Total visible entities for this peer = fullEntityCount + updateCount.
static uint16_t totalEntityCount(const fl::MsgWorldSnapshotHeader& hdr) {
    return static_cast<uint16_t>(hdr.fullEntityCount + hdr.updateCount);
}

// Parse all full MsgEntityEntry records from a snapshot packet.
static std::vector<fl::MsgEntityEntry> parseFullEntries(const std::vector<uint8_t>& pkt) {
    REQUIRE(pkt.size() >= sizeof(fl::MsgWorldSnapshotHeader));
    fl::MsgWorldSnapshotHeader hdr{};
    std::memcpy(&hdr, pkt.data(), sizeof(hdr));
    std::vector<fl::MsgEntityEntry> entries;
    std::size_t off = sizeof(hdr);
    for (uint16_t i = 0; i < hdr.fullEntityCount; ++i) {
        if (off + sizeof(fl::MsgEntityEntry) > pkt.size())
            break;
        fl::MsgEntityEntry e{};
        std::memcpy(&e, pkt.data() + off, sizeof(e));
        entries.push_back(e);
        off += sizeof(e);
    }
    return entries;
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

    // Tick 1 is a baseline tick (1 % 120 != 0 ... actually 1 % 120 == 1, not 0 — but all entities
    // are new to the peer so they appear as full entries regardless).
    auto snaps = snapshotsFor(net, 0);
    REQUIRE(snaps.size() == 1u);
    const auto& pkt = snaps[0];
    REQUIRE(pkt.size() >= sizeof(fl::MsgWorldSnapshotHeader));

    auto hdr = parseSnapshotHeader(pkt);
    CHECK(hdr.msgId == static_cast<uint8_t>(fl::MsgId::WorldSnapshot));
    CHECK(totalEntityCount(hdr) == 4u); // 3 pre-spawned + 1 peer entity
    CHECK(hdr.tickIndex == 1u);

    // Verify one of the full entries has pos[1] == 500.
    const auto fullEntries = parseFullEntries(pkt);
    REQUIRE(!fullEntries.empty());
    CHECK(fullEntries[0].pos[1] == 500.0);
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
    // AI entity appears in full entries (baseline tick 120 % 120 == 0 → all full entries)
    bool foundAiEntity = false;
    for (const auto& e : parseFullEntries(pkt)) {
        if (e.entityIdx == id.index) {
            CHECK(e.throttle > 0u); // throttle spooled up toward the commanded 100%
            foundAiEntity = true;
        }
    }
    CHECK(foundAiEntity);
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

    // Packet includes the peer entity full entry + SnapshotPeerCount TLV (6 bytes).
    const std::size_t expectedMin = sizeof(fl::MsgWorldSnapshotHeader) + sizeof(fl::MsgEntityEntry) + 6u;
    REQUIRE(pkt.size() == expectedMin);
    uint16_t pc{};
    CHECK(fl::readExtValue(pkt.data() + sizeof(fl::MsgWorldSnapshotHeader) + sizeof(fl::MsgEntityEntry), 6u,
                           static_cast<uint16_t>(fl::ExtTag::SnapshotPeerCount), pc));
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
    REQUIRE(pkt.size() >= sizeof(fl::MsgWorldSnapshotHeader) + sizeof(fl::MsgEntityEntry));
    fl::MsgEntityEntry e;
    std::memcpy(&e, pkt.data() + sizeof(fl::MsgWorldSnapshotHeader), sizeof(e));
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
    REQUIRE(pkt.size() >= sizeof(fl::MsgWorldSnapshotHeader) + sizeof(fl::MsgEntityEntry));
    fl::MsgEntityEntry e;
    std::memcpy(&e, pkt.data() + sizeof(fl::MsgWorldSnapshotHeader), sizeof(e));
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
    REQUIRE(pkt.size() >= sizeof(fl::MsgWorldSnapshotHeader) + sizeof(fl::MsgEntityEntry));
    fl::MsgEntityEntry e;
    std::memcpy(&e, pkt.data() + sizeof(fl::MsgWorldSnapshotHeader), sizeof(e));
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
    REQUIRE(pkt.size() >= sizeof(fl::MsgWorldSnapshotHeader) + 2 * sizeof(fl::MsgEntityEntry));

    fl::MsgEntityEntry e0, e1;
    std::memcpy(&e0, pkt.data() + sizeof(fl::MsgWorldSnapshotHeader), sizeof(e0));
    std::memcpy(&e1, pkt.data() + sizeof(fl::MsgWorldSnapshotHeader) + sizeof(fl::MsgEntityEntry), sizeof(e1));

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
    REQUIRE(pkt.size() >= sizeof(fl::MsgWorldSnapshotHeader) + 3 * sizeof(fl::MsgEntityEntry));

    fl::MsgEntityEntry entries[3];
    for (int i = 0; i < 3; ++i)
        std::memcpy(&entries[i], pkt.data() + sizeof(fl::MsgWorldSnapshotHeader) + i * sizeof(fl::MsgEntityEntry),
                    sizeof(fl::MsgEntityEntry));

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
    fl::MsgEntityEntry e;
    std::memcpy(&e, pkt.data() + sizeof(hdr), sizeof(e));
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
    fl::MsgEntityEntry e;
    std::memcpy(&e, pkt.data() + sizeof(hdr), sizeof(e));

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
    fl::MsgEntityEntry e;
    std::memcpy(&e, pkt.data() + sizeof(hdr), sizeof(e));
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
    fl::MsgEntityEntry e;
    std::memcpy(&e, pkt.data() + sizeof(hdr), sizeof(e));

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
    fl::MsgEntityEntry e;
    std::memcpy(&e, pkt.data() + sizeof(hdr), sizeof(e));
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
    // Set inputs BEFORE tick 0 (a baseline tick: 0 % 120 == 0) so all entities appear as
    // full MsgEntityEntry records. This lets us verify velocity via parseFullEntries.
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

    broadcaster.onTick(1.0 / 60.0, 0u); // tick 0: baseline → full entries for all entities
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
    fl::MsgEntityEntry ePeer0{}, ePeer1{};
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
    fl::MsgEntityEntry e;
    std::memcpy(&e, pkt.data() + sizeof(hdr), sizeof(e));
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
    // The entity is a full entry on tick 1 (first appearance) and an update entry on ticks 2-10.
    // Both MsgEntityEntry and MsgEntityUpdate carry the throttle field — read it from whichever is present.
    uint8_t throttle = 0;
    if (hdr.fullEntityCount > 0) {
        fl::MsgEntityEntry e{};
        if (fl::readRecordAt(pkt.data(), pkt.size(), sizeof(hdr), e))
            throttle = e.throttle;
    } else if (hdr.updateCount > 0) {
        fl::MsgEntityUpdate u{};
        if (fl::readRecordAt(pkt.data(), pkt.size(), sizeof(hdr), u))
            throttle = u.throttle;
    }
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
    REQUIRE(pkt.size() >= sizeof(fl::MsgWorldSnapshotHeader) + sizeof(fl::MsgEntityEntry));

    fl::MsgWorldSnapshotHeader hdr;
    std::memcpy(&hdr, pkt.data(), sizeof(hdr));
    REQUIRE(totalEntityCount(hdr) >= 1u);

    fl::MsgEntityEntry e;
    std::memcpy(&e, pkt.data() + sizeof(hdr), sizeof(e));

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
    REQUIRE(pkt.size() >= sizeof(fl::MsgWorldSnapshotHeader) + sizeof(fl::MsgEntityEntry));

    fl::MsgWorldSnapshotHeader hdr;
    std::memcpy(&hdr, pkt.data(), sizeof(hdr));
    REQUIRE(totalEntityCount(hdr) >= 1u);

    fl::MsgEntityEntry e;
    std::memcpy(&e, pkt.data() + sizeof(hdr), sizeof(e));

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
    fl::MsgEntityEntry e;
    std::memcpy(&e, snapshotsFor(net, 0)[0].data() + sizeof(hdr), sizeof(e));
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
    fl::MsgEntityEntry e;
    std::memcpy(&e, snapshotsFor(net, 0)[0].data() + sizeof(hdr), sizeof(e));
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
    fl::MsgEntityEntry e;
    std::memcpy(&e, snapshotsFor(net, 0)[0].data() + sizeof(hdr), sizeof(e));
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
    broadcaster.forEachPeer(
        [&](uint32_t, const std::string&, fl::EntityId, uint32_t delayTicks) { gotDelay = delayTicks; });
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
    broadcaster.forEachPeer(
        [&](uint32_t, const std::string&, fl::EntityId, uint32_t delayTicks) { gotDelay = delayTicks; });
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
    broadcaster.forEachPeer([&](uint32_t, const std::string&, fl::EntityId eid, uint32_t) {
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
    broadcaster.forEachPeer([&](uint32_t, const std::string&, fl::EntityId, uint32_t) { ++callCount; });
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
    REQUIRE(pkt.size() >= sizeof(fl::MsgWorldSnapshotHeader) + sizeof(fl::MsgEntityEntry));
    fl::MsgEntityEntry e;
    std::memcpy(&e, pkt.data() + sizeof(fl::MsgWorldSnapshotHeader), sizeof(e));

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
    CHECK(hdr.fullEntityCount == 0u);
    CHECK(hdr.updateCount == 0u);
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
    CHECK(hdr.fullEntityCount == 0u);
    CHECK(hdr.updateCount == 0u);
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
    broadcaster.setBaselineInterval(120); // baseline at tick 0, 120, 240...
    broadcaster.onConnect(0u);

    // Tick 1: all entities new — must be full entries
    broadcaster.onTick(1.0 / 60.0, 1u);
    auto snaps1 = snapshotsFor(net, 0);
    REQUIRE(!snaps1.empty());
    auto hdr1 = parseSnapshotHeader(snaps1[0]);
    CHECK(hdr1.fullEntityCount >= 1u);
    CHECK(hdr1.updateCount == 0u);

    // Tick 2: entities already known — must be update entries
    clearSnapshots(net);
    broadcaster.onTick(1.0 / 60.0, 2u);
    auto snaps2 = snapshotsFor(net, 0);
    REQUIRE(!snaps2.empty());
    auto hdr2 = parseSnapshotHeader(snaps2[0]);
    CHECK(hdr2.fullEntityCount == 0u);
    CHECK(hdr2.updateCount >= 1u);
}

TEST_CASE("WorldBroadcaster: baseline tick forces full entries", "[world_broadcaster][delta]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setBaselineInterval(2); // baseline at ticks 0, 2, 4...
    broadcaster.onConnect(0u);

    // Tick 1: new entities → full entries
    broadcaster.onTick(1.0 / 60.0, 1u);
    clearSnapshots(net);

    // Tick 2: baseline (2 % 2 == 0) → known-gens cleared → full entries again
    broadcaster.onTick(1.0 / 60.0, 2u);
    auto snaps = snapshotsFor(net, 0);
    REQUIRE(!snaps.empty());
    auto hdr = parseSnapshotHeader(snaps[0]);
    CHECK(hdr.fullEntityCount >= 1u);
    CHECK(hdr.updateCount == 0u);
}

TEST_CASE("WorldBroadcaster: entity gen change forces full entry on non-baseline tick", "[world_broadcaster][delta]") {
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
    broadcaster.setBaselineInterval(1000); // no baseline for a long time
    broadcaster.onConnect(0u);

    // Tick 1: entity appears as full entry, gen is cached
    broadcaster.onTick(1.0 / 60.0, 1u);
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
    broadcaster.setBaselineInterval(1000);
    broadcaster.onConnect(0u);

    // Tick 1: entity known, cached
    broadcaster.onTick(1.0 / 60.0, 1u);
    clearSnapshots(net);

    // Tick 2: entity should be update entry
    broadcaster.onTick(1.0 / 60.0, 2u);
    {
        auto snaps = snapshotsFor(net, 0);
        REQUIRE(!snaps.empty());
        CHECK(parseSnapshotHeader(snaps[0]).updateCount >= 1u);
    }
    clearSnapshots(net);

    // Disconnect clears knownGens; reconnect gives fresh state
    broadcaster.onDisconnect(0u);
    broadcaster.onConnect(0u); // new peer gets peerId=0 again (TrackingNetwork reuses it)

    // Tick 3: fresh connection — all entities must be full entries again
    broadcaster.onTick(1.0 / 60.0, 3u);
    auto snaps = snapshotsFor(net, 0);
    REQUIRE(!snaps.empty());
    auto hdr = parseSnapshotHeader(snaps[0]);
    CHECK(hdr.fullEntityCount >= 1u);
    CHECK(hdr.updateCount == 0u);
}

TEST_CASE("WorldBroadcaster: totalEntityCount matches buffer content", "[world_broadcaster][delta]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDebugDef());
    fl::EntityManager em(logger, registry);

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.setBaselineInterval(1000);
    broadcaster.onConnect(0u);

    // Tick 1 → full entries
    broadcaster.onTick(1.0 / 60.0, 1u);
    {
        auto snaps = snapshotsFor(net, 0);
        REQUIRE(!snaps.empty());
        const auto& pkt = snaps[0];
        auto hdr = parseSnapshotHeader(pkt);
        const std::size_t expectedSize = sizeof(fl::MsgWorldSnapshotHeader) +
                                         hdr.fullEntityCount * sizeof(fl::MsgEntityEntry) +
                                         hdr.updateCount * sizeof(fl::MsgEntityUpdate) + 6u; // SnapshotPeerCount TLV
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
        const std::size_t expectedSize = sizeof(fl::MsgWorldSnapshotHeader) +
                                         hdr.fullEntityCount * sizeof(fl::MsgEntityEntry) +
                                         hdr.updateCount * sizeof(fl::MsgEntityUpdate) + 6u;
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
