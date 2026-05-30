// SPDX-License-Identifier: GPL-3.0-or-later
#include "ILogger.h"
#include "INetwork.h"
#include "entity/EntityDef.h"
#include "entity/EntityManager.h"
#include "entity/EntityTypeRegistry.h"
#include "net/GameProtocol.h"
#include "net/WorldBroadcaster.h"

#include <catch2/catch_test_macros.hpp>
#include <cstring>
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
    const char* getPeerAddress(uint32_t) const override {
        return nullptr;
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
        t.pos[0] = static_cast<float>(i * 10);
        t.pos[1] = 500.0f;
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
    CHECK(e0.pos[1] == 500.0f);
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

TEST_CASE("WorldBroadcaster: onConnect sends ConnectAck with registered types", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);

    registry.registerType(makeDebugDef("type:a"));
    registry.registerType(makeDebugDef("type:b"));

    fl::WorldBroadcaster broadcaster(em, registry, net, logger);
    broadcaster.onConnect(0u);

    REQUIRE(net.sends.size() == 1u);
    CHECK(net.sendReliable);

    const auto& pkt = net.sends[0];
    REQUIRE(pkt.size() >= sizeof(fl::MsgConnectAck));

    fl::MsgConnectAck ack;
    std::memcpy(&ack, pkt.data(), sizeof(ack));
    CHECK(ack.msgId == static_cast<uint8_t>(fl::MsgId::ConnectAck));
    CHECK(ack.tickRateHz == 60);
    CHECK(ack.typeCount == 2u);

    // Verify first type def id.
    REQUIRE(pkt.size() >= sizeof(ack) + sizeof(fl::MsgEntityTypeDef));
    fl::MsgEntityTypeDef td;
    std::memcpy(&td, pkt.data() + sizeof(ack), sizeof(td));
    CHECK(std::string_view(td.id) == "type:a");
}

TEST_CASE("WorldBroadcaster: onConnect with empty registry sends typeCount=0", "[world_broadcaster]") {
    MockLogger logger;
    MockNetwork net;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em(logger, registry);
    fl::WorldBroadcaster broadcaster(em, registry, net, logger);

    broadcaster.onConnect(0u);

    REQUIRE(net.sends.size() == 1u);
    fl::MsgConnectAck ack;
    std::memcpy(&ack, net.sends[0].data(), sizeof(ack));
    CHECK(ack.typeCount == 0u);
    CHECK(net.sends[0].size() == sizeof(ack));
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

TEST_CASE("WorldBroadcaster: onReceive is a no-op: no crash, no sends", "[world_broadcaster]") {
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
