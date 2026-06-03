// SPDX-License-Identifier: GPL-3.0-or-later
#include "ILogger.h"
#include "INetwork.h"
#include "entity/EntityDef.h"
#include "entity/EntityManager.h"
#include "entity/EntityTypeRegistry.h"
#include "net/GameProtocol.h"
#include "net/WorldBroadcaster.h"
#include "weather/WeatherController.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
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

TEST_CASE("WorldBroadcaster: peer entity spawns at 2000 m altitude", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    net.peerAddresses[0] = "1.2.3.4:5000";
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    registry.registerType(makeDebugDef());

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.onConnect(0u);
    broadcaster.onTick(1.0 / 60.0, 1u);

    REQUIRE(!net.broadcasts.empty());
    const auto& pkt = net.broadcasts.back();
    REQUIRE(pkt.size() >= sizeof(fl::MsgWorldSnapshotHeader) + sizeof(fl::MsgEntityEntry));
    fl::MsgEntityEntry e;
    std::memcpy(&e, pkt.data() + sizeof(fl::MsgWorldSnapshotHeader), sizeof(e));
    // spawn at 2000 m; flight integrator may shift slightly in first tick
    CHECK(e.pos[1] >= 1990.0);
    CHECK(e.pos[1] <= 2020.0);
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

    // Peer was rejected: disconnectPeer called, no MsgHello/Ack sent.
    REQUIRE(net.disconnectedPeers.size() == 1u);
    CHECK(net.disconnectedPeers[0] == 0u);
    CHECK(net.sends.empty()); // no handshake messages
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
    CHECK(net.sends.empty());
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
