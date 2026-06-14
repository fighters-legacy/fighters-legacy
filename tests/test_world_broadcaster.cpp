// SPDX-License-Identifier: GPL-3.0-or-later
#include "ILogger.h"
#include "INetwork.h"
#include "entity/DamageDef.h"
#include "entity/EntityDef.h"
#include "entity/EntityManager.h"
#include "entity/EntityTypeRegistry.h"
#include "net/GameProtocol.h"
#include "net/WorldBroadcaster.h"
#include "render/RenderSnapshot.h"
#include "weather/WeatherController.h"

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

struct MockNetwork : INetwork {
    std::vector<std::vector<uint8_t>> broadcasts;
    std::vector<std::vector<uint8_t>> sends;
    bool sendReliable{false};
    std::map<uint32_t, std::string> peerAddresses; // configure per-test
    std::vector<uint32_t> disconnectedPeers;       // tracks disconnectPeer calls
    mutable std::string addrBuf;                   // backing store for getPeerAddress

    bool init() override {
        return true;
    }
    void shutdown() override {}
    void setEventHandler(INetworkEventHandler*) override {}
    bool bind(const char*, uint16_t, int) override {
        return true;
    }
    bool connect(const char*, uint16_t) override {
        return true;
    }
    void disconnect() override {}
    void disconnectPeer(uint32_t peerId) override {
        disconnectedPeers.push_back(peerId);
    }
    bool send(uint32_t, const void* data, std::size_t size, bool reliable) override {
        sends.push_back({static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size});
        sendReliable = reliable;
        return true;
    }
    void broadcast(const void* data, std::size_t size, bool) override {
        broadcasts.push_back({static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size});
    }
    void service(int) override {}
    int getPeerCount() const override {
        return 0;
    }
    PeerState getPeerState(uint32_t) const override {
        return PeerState::Disconnected;
    }
    const char* getPeerAddress(uint32_t peerId) const override {
        auto it = peerAddresses.find(peerId);
        if (it == peerAddresses.end())
            return nullptr;
        addrBuf = it->second;
        return addrBuf.c_str();
    }
    const char* getLastError() const override {
        return nullptr;
    }
};

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

    // Drive one tick manually.
    broadcaster.onTick(1.0 / 60.0, 1u);

    REQUIRE(net.broadcasts.size() == 1u);
    const auto& pkt = net.broadcasts[0];
    REQUIRE(pkt.size() >= sizeof(fl::MsgWorldSnapshotHeader));

    fl::MsgWorldSnapshotHeader hdr;
    std::memcpy(&hdr, pkt.data(), sizeof(hdr));
    CHECK(hdr.msgId == static_cast<uint8_t>(fl::MsgId::WorldSnapshot));
    CHECK(hdr.entityCount == 3u);
    CHECK(hdr.tickIndex == 1u);

    // Verify first entry position.
    REQUIRE(pkt.size() >= sizeof(hdr) + sizeof(fl::MsgEntityEntry));
    fl::MsgEntityEntry e0;
    std::memcpy(&e0, pkt.data() + sizeof(hdr), sizeof(e0));
    CHECK(e0.pos[1] == 500.0);
}

TEST_CASE("WorldBroadcaster: onTick with zero entities broadcasts empty header", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    broadcaster.onTick(1.0 / 60.0, 5u);

    REQUIRE(net.broadcasts.size() == 1u);
    fl::MsgWorldSnapshotHeader hdr;
    std::memcpy(&hdr, net.broadcasts[0].data(), sizeof(hdr));
    CHECK(hdr.entityCount == 0u);
    CHECK(hdr.tickIndex == 5u);
    CHECK(net.broadcasts[0].size() == sizeof(hdr));
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

    REQUIRE(!net.broadcasts.empty());
    const auto& pkt = net.broadcasts.back();
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

    REQUIRE(!net.broadcasts.empty());
    const auto& pkt = net.broadcasts.back();
    REQUIRE(pkt.size() >= sizeof(fl::MsgWorldSnapshotHeader) + sizeof(fl::MsgEntityEntry));
    fl::MsgEntityEntry e;
    std::memcpy(&e, pkt.data() + sizeof(fl::MsgWorldSnapshotHeader), sizeof(e));
    // 0 m terrain + 500 m AGL = 500 m; allow ±10 m for one flight-integrator tick
    CHECK(e.pos[1] >= 490.0);
    CHECK(e.pos[1] <= 510.0);
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
    broadcaster.onTick(1.0 / 60.0, 1u);
    CHECK(em.liveCount() == 0u);
    CHECK(net.sends.size() == 2u); // MsgHello + ConnectAck on connect; nothing sent on disconnect
}

TEST_CASE("WorldBroadcaster: onDisconnect does not crash and sends nothing", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    REQUIRE_NOTHROW(broadcaster.onDisconnect(0u));
    CHECK(net.sends.empty());
    CHECK(net.broadcasts.empty());
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
    CHECK(net.broadcasts.empty());
}

TEST_CASE("WorldBroadcaster: onReceive empty packet is discarded", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);

    registry.registerType(makeDebugDef());
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.onConnect(0u);
    net.broadcasts.clear();
    net.sends.clear();

    broadcaster.onReceive(0u, nullptr, 0u);
    broadcaster.onTick(1.0 / 60.0, 1u);

    // Entity should not have moved (no input applied).
    const auto& pkt = net.broadcasts[0];
    fl::MsgWorldSnapshotHeader hdr;
    std::memcpy(&hdr, pkt.data(), sizeof(hdr));
    REQUIRE(hdr.entityCount >= 1u);
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

    net.broadcasts.clear();
    broadcaster.onTick(1.0 / 60.0, 1u);

    REQUIRE(!net.broadcasts.empty());
    const auto& pkt = net.broadcasts[0];
    fl::MsgWorldSnapshotHeader hdr;
    std::memcpy(&hdr, pkt.data(), sizeof(hdr));
    REQUIRE(hdr.entityCount >= 1u);
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
    net.broadcasts.clear();

    // Only 10 bytes — less than sizeof(MsgClientInput) = 44.
    const uint8_t tiny[] = {static_cast<uint8_t>(fl::MsgId::ClientInput), 0, 0, 0, 0, 0, 0, 0, 0, 0};
    broadcaster.onReceive(0u, tiny, sizeof(tiny));
    broadcaster.onTick(1.0 / 60.0, 1u);

    const auto& pkt = net.broadcasts[0];
    fl::MsgWorldSnapshotHeader hdr;
    std::memcpy(&hdr, pkt.data(), sizeof(hdr));
    REQUIRE(hdr.entityCount >= 1u);
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
    net.broadcasts.clear();

    fl::MsgClientInput inp{};
    inp.msgId = static_cast<uint8_t>(fl::MsgId::ClientInput);
    inp.throttle = 5.f; // out-of-range; must be clamped to 1.0
    broadcaster.onReceive(0u, &inp, sizeof(inp));

    broadcaster.onTick(1.0 / 60.0, 1u);

    const auto& pkt = net.broadcasts[0];
    fl::MsgWorldSnapshotHeader hdr;
    std::memcpy(&hdr, pkt.data(), sizeof(hdr));
    REQUIRE(hdr.entityCount >= 1u);
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
    net.broadcasts.clear();

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
    const auto& pkt = net.broadcasts[0];
    fl::MsgWorldSnapshotHeader hdr;
    std::memcpy(&hdr, pkt.data(), sizeof(hdr));
    REQUIRE(hdr.entityCount >= 1u);
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

    net.broadcasts.clear();
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
    net.broadcasts.clear();
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
    // liveCount() updated by onTick — drive a tick first.
    broadcaster.onTick(1.0 / 60.0, 0u);
    REQUIRE(em.liveCount() == 2u);

    // Peer 0: full throttle forward; peer 1: no throttle.
    fl::MsgClientInput inp0{};
    inp0.msgId = static_cast<uint8_t>(fl::MsgId::ClientInput);
    inp0.throttle = 1.f;
    broadcaster.onReceive(0u, &inp0, sizeof(inp0));

    fl::MsgClientInput inp1{};
    inp1.msgId = static_cast<uint8_t>(fl::MsgId::ClientInput);
    inp1.throttle = 0.f;
    broadcaster.onReceive(1u, &inp1, sizeof(inp1));

    net.broadcasts.clear();
    broadcaster.onTick(1.0 / 60.0, 1u);

    // Snapshot has both entities; find peer 0's and peer 1's entries by assigned idx.
    // onConnect sends MsgHello (even index) then ConnectAck (odd index) for each peer.
    fl::MsgConnectAck ack0, ack1;
    std::memcpy(&ack0, net.sends[1].data(), sizeof(ack0)); // peer 0: sends[0]=Hello, sends[1]=Ack
    std::memcpy(&ack1, net.sends[3].data(), sizeof(ack1)); // peer 1: sends[2]=Hello, sends[3]=Ack

    REQUIRE(!net.broadcasts.empty());
    const auto& pkt = net.broadcasts[0];
    fl::MsgWorldSnapshotHeader hdr;
    std::memcpy(&hdr, pkt.data(), sizeof(hdr));
    CHECK(hdr.entityCount == 2u);

    // Find entries for each peer's entity by index.
    fl::MsgEntityEntry ePeer0{}, ePeer1{};
    const uint8_t* ep = pkt.data() + sizeof(hdr);
    for (uint16_t i = 0; i < hdr.entityCount; ++i) {
        fl::MsgEntityEntry e;
        std::memcpy(&e, ep + i * sizeof(e), sizeof(e));
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

    net.broadcasts.clear();
    broadcaster.onTick(1.0 / 60.0, 1u);

    REQUIRE(!net.broadcasts.empty());
    const auto& pkt = net.broadcasts[0];
    fl::MsgWorldSnapshotHeader hdr;
    std::memcpy(&hdr, pkt.data(), sizeof(hdr));
    REQUIRE(hdr.entityCount >= 1u);
    fl::MsgEntityEntry e;
    std::memcpy(&e, pkt.data() + sizeof(hdr), sizeof(e));
    // Mismatched version discarded: default throttle=0 input used instead of throttle=1.
    // Full throttle from 40 m/s produces ~40.6 m/s in one tick; this must stay < 40.1 m/s.
    CHECK(e.vel[0] < 40.1f);
}

TEST_CASE("WorldBroadcaster: onTick broadcasts WorldSnapshot with correct protocolVersion", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    broadcaster.onTick(1.0 / 60.0, 1u);

    REQUIRE(net.broadcasts.size() == 1u);
    fl::MsgWorldSnapshotHeader hdr;
    std::memcpy(&hdr, net.broadcasts[0].data(), sizeof(hdr));
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

    net.broadcasts.clear();
    // Multiple ticks allow the spool-up lag to partially settle.
    for (int i = 0; i < 10; ++i)
        broadcaster.onTick(1.0 / 60.0, static_cast<uint64_t>(i + 1));

    REQUIRE(!net.broadcasts.empty());
    const auto& pkt = net.broadcasts.back();
    REQUIRE(pkt.size() >= sizeof(fl::MsgWorldSnapshotHeader) + sizeof(fl::MsgEntityEntry));

    fl::MsgWorldSnapshotHeader hdr;
    std::memcpy(&hdr, pkt.data(), sizeof(hdr));
    REQUIRE(hdr.entityCount >= 1u);

    fl::MsgEntityEntry e;
    std::memcpy(&e, pkt.data() + sizeof(hdr), sizeof(e));

    // throttle_actual spools up over time; after 10 ticks at 1.f input it should be > 0.
    CHECK(e.throttle > 0u);
    // The encoded value is throttle_actual * 100, clamped to [0, 100].
    CHECK(e.throttle <= 100u);
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

    net.broadcasts.clear();
    broadcaster.onTick(1.0 / 60.0, 1u);

    REQUIRE(!net.broadcasts.empty());
    const auto& pkt = net.broadcasts.back();
    REQUIRE(pkt.size() >= sizeof(fl::MsgWorldSnapshotHeader) + sizeof(fl::MsgEntityEntry));

    fl::MsgWorldSnapshotHeader hdr;
    std::memcpy(&hdr, pkt.data(), sizeof(hdr));
    REQUIRE(hdr.entityCount >= 1u);

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

    net.broadcasts.clear();
    broadcaster.onTick(1.0 / 60.0, 1u);

    REQUIRE(!net.broadcasts.empty());
    const auto& pkt = net.broadcasts.back();
    REQUIRE(pkt.size() >= sizeof(fl::MsgWorldSnapshotHeader) + sizeof(fl::MsgEntityEntry));

    fl::MsgWorldSnapshotHeader hdr;
    std::memcpy(&hdr, pkt.data(), sizeof(hdr));
    REQUIRE(hdr.entityCount >= 1u);

    fl::MsgEntityEntry e;
    std::memcpy(&e, pkt.data() + sizeof(hdr), sizeof(e));

    CHECK((e.engineFailFlags & fl::kEngineFailGeneric) != 0u);
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
    broadcaster.forEachPeer([&](uint32_t, const std::string&, fl::EntityId eid) {
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
    broadcaster.forEachPeer([&](uint32_t, const std::string&, fl::EntityId) { ++callCount; });
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

    auto t = std::chrono::steady_clock::now();
    broadcaster.setClockOverride([&t]() { return t; });
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

    auto t = std::chrono::steady_clock::now();
    broadcaster.setClockOverride([&t]() { return t; });
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

    auto t = std::chrono::steady_clock::now();
    broadcaster.setClockOverride([&t]() { return t; });
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
    t += std::chrono::seconds(6);

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

    auto t = std::chrono::steady_clock::now();
    broadcaster.setClockOverride([&t]() { return t; });
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

    auto t = std::chrono::steady_clock::now();
    broadcaster.setClockOverride([&t]() { return t; });

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

    auto t = std::chrono::steady_clock::now();
    broadcaster.setClockOverride([&t]() { return t; });

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

    auto t = std::chrono::steady_clock::now();
    broadcaster.setClockOverride([&t]() { return t; });

    net.peerAddresses[0] = "1.2.3.4:1000";
    broadcaster.onConnect(0u);

    auto pkt = makeClientInputPacket();
    // Send 120 (at limit)
    for (int i = 0; i < 120; ++i)
        broadcaster.onReceive(0u, pkt.data(), pkt.size());
    CHECK(net.disconnectedPeers.empty());

    // Advance past the 1s window, counter resets
    t += std::chrono::seconds(2);
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

    auto t = std::chrono::steady_clock::now();
    broadcaster.setClockOverride([&t]() { return t; });

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

    auto t = std::chrono::steady_clock::now();
    broadcaster.setClockOverride([&t]() { return t; });

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

    auto t = std::chrono::steady_clock::now();
    broadcaster.setClockOverride([&t]() { return t; });

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

    auto t = std::chrono::steady_clock::now();
    broadcaster.setClockOverride([&t]() { return t; });

    net.peerAddresses[0] = "1.2.3.4:1000";
    // One connect fills limit (limit=1)
    broadcaster.onConnect(0u);
    broadcaster.onDisconnect(0u);
    net.disconnectedPeers.clear();
    net.sends.clear();

    // Advance clock past window
    t += std::chrono::seconds(6);

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

    auto t = std::chrono::steady_clock::now();
    broadcaster.setClockOverride([&t]() { return t; });
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

    auto t = std::chrono::steady_clock::now();
    broadcaster.setClockOverride([&t]() { return t; });
    broadcaster.initiateShutdown(60, 30); // 30s interval

    // First tick fires first notice.
    broadcaster.onTick(1.0 / 60.0, 0u);
    std::size_t noticesAfterFirst = 0;
    for (const auto& pkt : net.broadcasts)
        if (pkt.size() == sizeof(fl::MsgServerNotice) && pkt[0] == static_cast<uint8_t>(fl::MsgId::ServerNotice))
            ++noticesAfterFirst;
    REQUIRE(noticesAfterFirst == 1u);

    // Advance only 10s (halfway through 30s interval) — no new notice expected.
    t += std::chrono::seconds(10);
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

    auto t = std::chrono::steady_clock::now();
    broadcaster.setClockOverride([&t]() { return t; });
    broadcaster.initiateShutdown(60, 30);
    broadcaster.onTick(1.0 / 60.0, 0u);
    broadcaster.cancelShutdown();
    CHECK(!broadcaster.isShuttingDown());

    net.broadcasts.clear();
    t += std::chrono::seconds(70); // past original shutdown time
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

    auto t = std::chrono::steady_clock::now();
    broadcaster.setClockOverride([&t]() { return t; });
    broadcaster.initiateShutdown(30, 5);
    broadcaster.onTick(1.0 / 60.0, 0u);

    t += std::chrono::seconds(20);
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

    auto t = std::chrono::steady_clock::now();
    broadcaster.setClockOverride([&t]() { return t; });
    broadcaster.initiateShutdown(10, 5);

    t += std::chrono::seconds(11);
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

    auto t = std::chrono::steady_clock::now();
    broadcaster.setClockOverride([&t]() { return t; });
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

    auto t = std::chrono::steady_clock::now();
    broadcaster.setClockOverride([&t]() { return t; });
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

    auto t = std::chrono::steady_clock::now();
    broadcaster.setClockOverride([&t]() { return t; });
    broadcaster.initiateShutdown(600, 300); // 10 min, 5-min interval

    broadcaster.onTick(1.0 / 60.0, 0u); // first notice at T-600s

    // Advance to T-61s (would skip T-60s without the clamp logic).
    t += std::chrono::seconds(539);
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

    auto t = std::chrono::steady_clock::now();
    broadcaster.setClockOverride([&t]() { return t; });
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

    auto t = std::chrono::steady_clock::now();
    broadcaster.setClockOverride([&t]() { return t; });
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

    auto t = std::chrono::steady_clock::now();
    broadcaster.setClockOverride([&t]() { return t; });
    broadcaster.initiateShutdown(300, 60);

    t += std::chrono::seconds(245); // T-55s remaining
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

    auto t = std::chrono::steady_clock::now();
    broadcaster.setClockOverride([&t]() { return t; });
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

    auto t = std::chrono::steady_clock::now();
    broadcaster.setClockOverride([&t]() { return t; });
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

    auto t = std::chrono::steady_clock::now();
    broadcaster.setClockOverride([&t]() { return t; });
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

    auto t = std::chrono::steady_clock::now();
    broadcaster.setClockOverride([&t]() { return t; });
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

    auto t = std::chrono::steady_clock::now();
    broadcaster.setClockOverride([&t]() { return t; });
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

    auto t = std::chrono::steady_clock::now();
    broadcaster.setClockOverride([&t]() { return t; });
    broadcaster.initiateShutdown(120, 60, "Server restarting");

    // First tick fires first notice.
    broadcaster.onTick(1.0 / 60.0, 0u);
    std::size_t firstNoticeIdx = net.broadcasts.size() - 1;

    // Advance 60s to reach next notice interval then extend.
    t += std::chrono::seconds(60);
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
static std::vector<uint8_t> makeAdminCmd(const char* token, const char* command) {
    fl::MsgAdminCommand msg{};
    msg.msgId = static_cast<uint8_t>(fl::MsgId::AdminCommand);
    std::snprintf(msg.token, sizeof(msg.token), "%s", token);
    std::snprintf(msg.command, sizeof(msg.command), "%s", command);
    return {reinterpret_cast<const uint8_t*>(&msg), reinterpret_cast<const uint8_t*>(&msg) + sizeof(msg)};
}

// ---------------------------------------------------------------------------
// MOTD helpers
// ---------------------------------------------------------------------------

static std::string parseMotdText(const std::vector<uint8_t>& pkt) {
    if (pkt.size() < 4 || pkt[0] != static_cast<uint8_t>(fl::MsgId::Motd))
        return {};
    // exclude msgId (0), displaySeconds (1-2), and trailing NUL (last byte)
    return std::string(reinterpret_cast<const char*>(pkt.data() + 3), pkt.size() - 4);
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
    // 1 (msgId) + 2 (displaySeconds) + kMaxMotdBytes (text) + 1 (NUL) = kMaxMotdBytes + 4
    CHECK(net.sends[2].size() == fl::kMaxMotdBytes + 4u);
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
    REQUIRE(net.sends[2].size() >= 3u);
    uint16_t secs = 0;
    std::memcpy(&secs, net.sends[2].data() + 1, sizeof(secs));
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
    REQUIRE(net.sends[2].size() >= 3u);
    uint16_t secs = 0;
    std::memcpy(&secs, net.sends[2].data() + 1, sizeof(secs));
    CHECK(secs == 45u);
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

TEST_CASE("WorldBroadcaster: MsgAdminCommand long result truncated to 125 chars",
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
    fl::MsgAdminResponse resp{};
    REQUIRE(parseLastAdminResponse(net, resp));
    CHECK(std::strlen(resp.text) == 125u);
    CHECK(resp.text[125] == '\0');
}

// ---------------------------------------------------------------------------
// Admin auth lockout tests
// ---------------------------------------------------------------------------

// Shared setup helper: broadcaster with password "pw", dispatch noop, 3-failure
// threshold, 60 s lockout, injectable clock, and peer 0 connected from 1.2.3.4.
static void setupAuthFixture(fl::WorldBroadcaster& broadcaster, MockNetwork& net,
                             std::chrono::steady_clock::time_point& now) {
    broadcaster.setOperatorPassword("pw");
    broadcaster.setAdminDispatch([](std::string_view) -> std::string { return "ok"; });
    broadcaster.setAdminAuthParams(3, 60);
    broadcaster.setClockOverride([&now] { return now; });
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
    auto now = std::chrono::steady_clock::now();
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
    auto now = std::chrono::steady_clock::now();
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
    auto now = std::chrono::steady_clock::now();
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
    auto now = std::chrono::steady_clock::now();
    setupAuthFixture(broadcaster, net, now);

    // Trigger lockout
    for (int i = 0; i < 3; ++i) {
        auto pkt = makeAdminCmd("wrongpass", "status");
        broadcaster.onReceive(0u, pkt.data(), pkt.size());
    }
    net.disconnectedPeers.clear();
    net.sends.clear();

    // Advance clock past 60 s TTL
    now += std::chrono::seconds(61);

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
    auto now = std::chrono::steady_clock::now();
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
    auto now = std::chrono::steady_clock::now();
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
    auto now = std::chrono::steady_clock::now();
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
    auto now = std::chrono::steady_clock::now();
    broadcaster.setAdminAuthParams(3, 60);
    broadcaster.setClockOverride([&now] { return now; });
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
    auto now = std::chrono::steady_clock::now();
    setupAuthFixture(broadcaster, net, now);

    // Trigger lockout
    for (int i = 0; i < 3; ++i) {
        auto pkt = makeAdminCmd("wrongpass", "status");
        broadcaster.onReceive(0u, pkt.data(), pkt.size());
    }
    net.disconnectedPeers.clear();
    net.sends.clear();

    // Advance past TTL — lockout entry is now expired but not yet pruned
    now += std::chrono::seconds(61);

    // Drive 600 onTick calls to trigger the prune cycle
    for (int i = 0; i < 600; ++i)
        broadcaster.onTick(1.0 / 60.0, static_cast<uint64_t>(i + 1));

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
    auto now = std::chrono::steady_clock::now();
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
    auto now = std::chrono::steady_clock::now();
    setupAuthFixture(broadcaster, net, now);

    // No failures — unlockAdminAuth must report IP was not locked
    CHECK_FALSE(broadcaster.unlockAdminAuth("1.2.3.4"));

    // Connect peer 1 from same IP: must not be refused
    broadcaster.onConnect(1u);
    CHECK(net.disconnectedPeers.empty());
}
