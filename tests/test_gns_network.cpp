// SPDX-License-Identifier: GPL-3.0-or-later
// Runtime contract tests for the GameNetworkingSockets backend (#507). Mirrors test_network.cpp's
// structure and holds GnsNetwork to the same INetwork contract as ENetNetwork. Uses real loopback
// UDP sockets, so cases are tagged [gns][integration]; a distinct port band (28100+) avoids
// colliding with the enet test's ports.
#include "GnsNetwork.h"

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string>
#include <vector>

using namespace fl;

namespace {

struct Event {
    enum class Type { Connect, Disconnect, Receive };
    Type type;
    uint32_t peerId{0};
    std::vector<uint8_t> data;
};

struct EventSink : INetworkEventHandler {
    std::vector<Event> events;
    void onConnect(uint32_t peerId) override {
        events.push_back({Event::Type::Connect, peerId, {}});
    }
    void onDisconnect(uint32_t peerId) override {
        events.push_back({Event::Type::Disconnect, peerId, {}});
    }
    void onReceive(uint32_t peerId, const void* data, std::size_t size) override {
        Event e;
        e.type = Event::Type::Receive;
        e.peerId = peerId;
        e.data.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
        events.push_back(std::move(e));
    }
    int countType(Event::Type t) const {
        int n = 0;
        for (const auto& e : events)
            if (e.type == t)
                ++n;
        return n;
    }
};

void pump(INetwork& server, INetwork& client, int iters, int msPerIter = 15) {
    for (int i = 0; i < iters; ++i) {
        server.service(msPerIter);
        client.service(msPerIter);
    }
}

// Establishes a loopback connection; returns the server-side peerId of the connected client.
uint32_t connectLoopback(GnsNetwork& server, EventSink& serverSink, GnsNetwork& client, EventSink& clientSink,
                         uint16_t port) {
    REQUIRE(server.bind("127.0.0.1", port, 16));
    server.setEventHandler(&serverSink);
    REQUIRE(client.connect("127.0.0.1", port));
    client.setEventHandler(&clientSink);
    for (int i = 0; i < 60 && serverSink.countType(Event::Type::Connect) == 0; ++i)
        pump(server, client, 1);
    REQUIRE(serverSink.countType(Event::Type::Connect) == 1);
    REQUIRE(clientSink.countType(Event::Type::Connect) == 1);
    return serverSink.events.front().peerId;
}

} // namespace

TEST_CASE("gns init and shutdown", "[gns]") {
    GnsNetwork net;
    REQUIRE(net.init());
    net.shutdown();
}

TEST_CASE("gns double init is safe", "[gns]") {
    GnsNetwork net;
    REQUIRE(net.init());
    REQUIRE(net.init());
    net.shutdown();
}

TEST_CASE("gns library stays initialized until the last instance shuts down", "[gns]") {
    auto a = std::make_unique<GnsNetwork>();
    auto b = std::make_unique<GnsNetwork>();
    REQUIRE(a->init());
    REQUIRE(b->init());
    a->shutdown(); // b still alive — global GNS interface must stay valid
    REQUIRE(b->bind("127.0.0.1", 28100, 4));
    b->shutdown();
}

TEST_CASE("gns getPeerState out-of-range", "[gns]") {
    GnsNetwork net;
    REQUIRE(net.init());
    CHECK(net.getPeerState(999) == PeerState::Disconnected);
    net.shutdown();
}

TEST_CASE("gns getPeerAddress before connect", "[gns]") {
    GnsNetwork net;
    REQUIRE(net.init());
    CHECK(net.getPeerAddress(0) == nullptr);
    net.shutdown();
}

TEST_CASE("gns getPeerLinkStats returns zeros for out-of-range peer", "[gns]") {
    GnsNetwork net;
    REQUIRE(net.init());
    const PeerLinkStats s = net.getPeerLinkStats(999);
    CHECK(s.rttMs == 0u);
    CHECK(s.reliableBytesInFlight == 0u);
    net.shutdown();
}

TEST_CASE("gns send before bind returns false", "[gns]") {
    GnsNetwork net;
    REQUIRE(net.init());
    const uint8_t byte = 0x42;
    CHECK_FALSE(net.send(0, &byte, 1, true));
    net.shutdown();
}

TEST_CASE("gns loopback connect", "[gns][integration]") {
    GnsNetwork server, client;
    REQUIRE(server.init());
    REQUIRE(client.init());
    EventSink ss, cs;
    connectLoopback(server, ss, client, cs, 28110);
    CHECK(server.getPeerCount() == 1);
    client.shutdown();
    server.shutdown();
}

TEST_CASE("gns getPeerAddress returns ip:port", "[gns][integration]") {
    GnsNetwork server, client;
    REQUIRE(server.init());
    REQUIRE(client.init());
    EventSink ss, cs;
    const uint32_t peer = connectLoopback(server, ss, client, cs, 28111);
    const char* addr = server.getPeerAddress(peer);
    REQUIRE(addr != nullptr);
    CHECK(std::string(addr).find("127.0.0.1") != std::string::npos);
    client.shutdown();
    server.shutdown();
}

TEST_CASE("gns reliable send client to server", "[gns][integration]") {
    GnsNetwork server, client;
    REQUIRE(server.init());
    REQUIRE(client.init());
    EventSink ss, cs;
    connectLoopback(server, ss, client, cs, 28112);
    const std::vector<uint8_t> payload{1, 2, 3, 4, 5};
    REQUIRE(client.send(0, payload.data(), payload.size(), true));
    for (int i = 0; i < 40 && ss.countType(Event::Type::Receive) == 0; ++i)
        pump(server, client, 1);
    REQUIRE(ss.countType(Event::Type::Receive) == 1);
    CHECK(ss.events.back().data == payload);
    client.shutdown();
    server.shutdown();
}

TEST_CASE("gns unreliable send server to client", "[gns][integration]") {
    GnsNetwork server, client;
    REQUIRE(server.init());
    REQUIRE(client.init());
    EventSink ss, cs;
    const uint32_t peer = connectLoopback(server, ss, client, cs, 28113);
    const std::vector<uint8_t> payload{9, 8, 7};
    // Retry a few times — unreliable packets may be dropped during handshake settling.
    bool got = false;
    for (int attempt = 0; attempt < 10 && !got; ++attempt) {
        server.send(peer, payload.data(), payload.size(), false);
        for (int i = 0; i < 10 && cs.countType(Event::Type::Receive) == 0; ++i)
            pump(server, client, 1);
        got = cs.countType(Event::Type::Receive) > 0;
    }
    REQUIRE(got);
    CHECK(cs.events.back().data == payload);
    client.shutdown();
    server.shutdown();
}

TEST_CASE("gns server broadcast reaches all clients", "[gns][integration]") {
    GnsNetwork server, c1, c2;
    REQUIRE(server.init());
    REQUIRE(c1.init());
    REQUIRE(c2.init());
    EventSink ss, cs1, cs2;
    REQUIRE(server.bind("127.0.0.1", 28114, 16));
    server.setEventHandler(&ss);
    REQUIRE(c1.connect("127.0.0.1", 28114));
    c1.setEventHandler(&cs1);
    REQUIRE(c2.connect("127.0.0.1", 28114));
    c2.setEventHandler(&cs2);
    for (int i = 0; i < 80 && ss.countType(Event::Type::Connect) < 2; ++i) {
        server.service(15);
        c1.service(15);
        c2.service(15);
    }
    REQUIRE(ss.countType(Event::Type::Connect) == 2);
    CHECK(server.getPeerCount() == 2);

    const std::vector<uint8_t> payload{0xAB, 0xCD};
    server.broadcast(payload.data(), payload.size(), true);
    for (int i = 0; i < 40 && (cs1.countType(Event::Type::Receive) == 0 || cs2.countType(Event::Type::Receive) == 0);
         ++i) {
        server.service(15);
        c1.service(15);
        c2.service(15);
    }
    CHECK(cs1.countType(Event::Type::Receive) == 1);
    CHECK(cs2.countType(Event::Type::Receive) == 1);
    c1.shutdown();
    c2.shutdown();
    server.shutdown();
}

TEST_CASE("gns disconnect fires callback", "[gns][integration]") {
    GnsNetwork server, client;
    REQUIRE(server.init());
    REQUIRE(client.init());
    EventSink ss, cs;
    connectLoopback(server, ss, client, cs, 28115);
    client.disconnect();
    for (int i = 0; i < 60 && ss.countType(Event::Type::Disconnect) == 0; ++i)
        server.service(15);
    CHECK(ss.countType(Event::Type::Disconnect) == 1);
    server.shutdown();
}

TEST_CASE("gns getPeerLinkStats populated after handshake", "[gns][integration]") {
    GnsNetwork server, client;
    REQUIRE(server.init());
    REQUIRE(client.init());
    EventSink ss, cs;
    const uint32_t peer = connectLoopback(server, ss, client, cs, 28116);
    // Exchange a few packets so GNS has RTT samples, then read stats (should not crash / return sane).
    for (int i = 0; i < 20; ++i) {
        const uint8_t b = static_cast<uint8_t>(i);
        server.send(peer, &b, 1, true);
        pump(server, client, 1);
    }
    const PeerLinkStats s = server.getPeerLinkStats(peer);
    CHECK(s.packetLoss >= 0.f);
    CHECK(s.packetLoss <= 1.f);
    client.shutdown();
    server.shutdown();
}
