// SPDX-License-Identifier: GPL-3.0-or-later
#include "ENetNetwork.h"
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

struct Event {
    enum class Type { Connect, Disconnect, Receive };
    Type type;
    uint32_t peerId{0};
    std::vector<uint8_t> data; // populated for Receive events
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
        for (const auto& ev : events)
            if (ev.type == t)
                ++n;
        return n;
    }
};

static void pump(INetwork& server, INetwork& client, int iters, int msPerIter = 10) {
    for (int i = 0; i < iters; ++i) {
        server.service(msPerIter);
        client.service(msPerIter);
    }
}

static void pumpN(INetwork& server, std::initializer_list<INetwork*> clients, int iters, int msPerIter = 10) {
    for (int i = 0; i < iters; ++i) {
        server.service(msPerIter);
        for (INetwork* c : clients)
            c->service(msPerIter);
    }
}

// Each integration test uses a unique port to prevent cross-test interference
// within the sequential Catch2 binary run. Reserved ranges:
//   [19001, 19010] -- basic tests
//   [19021, 19024] -- IPv6 tests
//   [19030, 19033] -- pre-handshake rate limit tests

// ---------------------------------------------------------------------------
// Init / shutdown
// ---------------------------------------------------------------------------

TEST_CASE("init and shutdown", "[network]") {
    ENetNetwork net;
    REQUIRE(net.init());
    CHECK(net.getLastError() == nullptr);
    net.shutdown();
}

TEST_CASE("double init is safe", "[network]") {
    ENetNetwork net;
    REQUIRE(net.init());
    REQUIRE(net.init()); // idempotent
    net.shutdown();
}

TEST_CASE("double shutdown is safe", "[network]") {
    ENetNetwork net;
    REQUIRE(net.init());
    net.shutdown();
    net.shutdown(); // must not crash
}

// ---------------------------------------------------------------------------
// Pre-connection guards
// ---------------------------------------------------------------------------

TEST_CASE("getPeerState out-of-range", "[network]") {
    ENetNetwork net;
    REQUIRE(net.init());
    CHECK(net.getPeerState(999) == PeerState::Disconnected);
    net.shutdown();
}

TEST_CASE("getPeerAddress before connect", "[network]") {
    ENetNetwork net;
    REQUIRE(net.init());
    CHECK(net.getPeerAddress(0) == nullptr);
    net.shutdown();
}

TEST_CASE("send before bind returns false", "[network]") {
    ENetNetwork net;
    REQUIRE(net.init());
    const uint8_t buf[] = {1, 2, 3};
    CHECK_FALSE(net.send(0, buf, sizeof(buf), true));
    CHECK(net.getLastError() != nullptr);
    net.shutdown();
}

// ---------------------------------------------------------------------------
// Loopback connect
// ---------------------------------------------------------------------------

TEST_CASE("loopback connect", "[network][integration]") {
    ENetNetwork server, client;
    EventSink srvSink, cliSink;
    REQUIRE(server.init());
    REQUIRE(client.init());
    server.setEventHandler(&srvSink);
    client.setEventHandler(&cliSink);

    REQUIRE(server.bind(nullptr, 19001, 4));
    REQUIRE(client.connect("127.0.0.1", 19001));

    pump(server, client, 20);

    REQUIRE(srvSink.countType(Event::Type::Connect) == 1);
    REQUIRE(cliSink.countType(Event::Type::Connect) == 1);
    CHECK(server.getPeerState(0) == PeerState::Connected);
    CHECK(server.getPeerCount() == 1);

    server.shutdown();
    client.shutdown();
}

// ---------------------------------------------------------------------------
// getPeerAddress
// ---------------------------------------------------------------------------

TEST_CASE("getPeerAddress returns ip:port", "[network][integration]") {
    ENetNetwork server, client;
    EventSink srvSink, cliSink;
    REQUIRE(server.init());
    REQUIRE(client.init());
    server.setEventHandler(&srvSink);
    client.setEventHandler(&cliSink);

    REQUIRE(server.bind(nullptr, 19002, 4));
    REQUIRE(client.connect("127.0.0.1", 19002));

    pump(server, client, 20);

    REQUIRE(srvSink.countType(Event::Type::Connect) == 1);
    const char* addr = server.getPeerAddress(0);
    REQUIRE(addr != nullptr);
    CHECK(std::string(addr).find("127.0.0.1") != std::string::npos);

    server.shutdown();
    client.shutdown();
}

// ---------------------------------------------------------------------------
// Data transfer
// ---------------------------------------------------------------------------

TEST_CASE("reliable send client to server", "[network][integration]") {
    ENetNetwork server, client;
    EventSink srvSink, cliSink;
    REQUIRE(server.init());
    REQUIRE(client.init());
    server.setEventHandler(&srvSink);
    client.setEventHandler(&cliSink);

    REQUIRE(server.bind(nullptr, 19003, 4));
    REQUIRE(client.connect("127.0.0.1", 19003));
    pump(server, client, 20);

    const uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
    REQUIRE(client.send(0, payload, sizeof(payload), true));
    pump(server, client, 20);

    REQUIRE(srvSink.countType(Event::Type::Receive) == 1);
    const auto& ev = srvSink.events.back();
    REQUIRE(ev.data.size() == sizeof(payload));
    CHECK(std::memcmp(ev.data.data(), payload, sizeof(payload)) == 0);

    server.shutdown();
    client.shutdown();
}

TEST_CASE("unreliable send server to client", "[network][integration]") {
    ENetNetwork server, client;
    EventSink srvSink, cliSink;
    REQUIRE(server.init());
    REQUIRE(client.init());
    server.setEventHandler(&srvSink);
    client.setEventHandler(&cliSink);

    REQUIRE(server.bind(nullptr, 19004, 4));
    REQUIRE(client.connect("127.0.0.1", 19004));
    pump(server, client, 20);

    REQUIRE(srvSink.countType(Event::Type::Connect) == 1);

    const uint8_t payload[] = {1, 2, 3, 4, 5};
    REQUIRE(server.send(0, payload, sizeof(payload), false));
    pump(server, client, 20);

    REQUIRE(cliSink.countType(Event::Type::Receive) == 1);
    const auto& ev = cliSink.events.back();
    REQUIRE(ev.data.size() == sizeof(payload));
    CHECK(std::memcmp(ev.data.data(), payload, sizeof(payload)) == 0);

    server.shutdown();
    client.shutdown();
}

TEST_CASE("large packet fragmentation", "[network][integration]") {
    ENetNetwork server, client;
    EventSink srvSink, cliSink;
    REQUIRE(server.init());
    REQUIRE(client.init());
    server.setEventHandler(&srvSink);
    client.setEventHandler(&cliSink);

    REQUIRE(server.bind(nullptr, 19005, 4));
    REQUIRE(client.connect("127.0.0.1", 19005));
    pump(server, client, 20);

    // 10 KB — well above the MTU (~1400 bytes); ENet must fragment and reassemble.
    constexpr std::size_t kSize = 10 * 1024;
    std::vector<uint8_t> big(kSize);
    for (std::size_t i = 0; i < kSize; ++i)
        big[i] = static_cast<uint8_t>(i % 256);

    REQUIRE(client.send(0, big.data(), kSize, true));
    pump(server, client, 50);

    REQUIRE(srvSink.countType(Event::Type::Receive) == 1);
    const auto& ev = srvSink.events.back();
    REQUIRE(ev.data.size() == kSize);
    CHECK(std::memcmp(ev.data.data(), big.data(), kSize) == 0);

    server.shutdown();
    client.shutdown();
}

// ---------------------------------------------------------------------------
// Multiple clients
// ---------------------------------------------------------------------------

TEST_CASE("multiple clients connect", "[network][integration]") {
    ENetNetwork server, c1, c2;
    EventSink srvSink, s1, s2;
    REQUIRE(server.init());
    REQUIRE(c1.init());
    REQUIRE(c2.init());
    server.setEventHandler(&srvSink);
    c1.setEventHandler(&s1);
    c2.setEventHandler(&s2);

    REQUIRE(server.bind(nullptr, 19006, 4));
    REQUIRE(c1.connect("127.0.0.1", 19006));
    REQUIRE(c2.connect("127.0.0.1", 19006));

    pumpN(server, {&c1, &c2}, 30);

    REQUIRE(srvSink.countType(Event::Type::Connect) == 2);
    CHECK(server.getPeerCount() == 2);

    // Peer IDs are distinct
    uint32_t id1 = srvSink.events[0].peerId;
    uint32_t id2 = srvSink.events[1].peerId;
    CHECK(id1 != id2);

    // Server can send to each independently
    const uint8_t msg1[] = {0xAA};
    const uint8_t msg2[] = {0xBB};
    REQUIRE(server.send(id1, msg1, 1, true));
    REQUIRE(server.send(id2, msg2, 1, true));

    pumpN(server, {&c1, &c2}, 20);
    CHECK(s1.countType(Event::Type::Receive) == 1);
    CHECK(s2.countType(Event::Type::Receive) == 1);
    CHECK(s1.events.back().data[0] == 0xAA);
    CHECK(s2.events.back().data[0] == 0xBB);

    server.shutdown();
    c1.shutdown();
    c2.shutdown();
}

TEST_CASE("server broadcast reaches all clients", "[network][integration]") {
    ENetNetwork server, c1, c2;
    EventSink srvSink, s1, s2;
    REQUIRE(server.init());
    REQUIRE(c1.init());
    REQUIRE(c2.init());
    server.setEventHandler(&srvSink);
    c1.setEventHandler(&s1);
    c2.setEventHandler(&s2);

    REQUIRE(server.bind(nullptr, 19007, 4));
    REQUIRE(c1.connect("127.0.0.1", 19007));
    REQUIRE(c2.connect("127.0.0.1", 19007));

    pumpN(server, {&c1, &c2}, 30);
    REQUIRE(srvSink.countType(Event::Type::Connect) == 2);

    const uint8_t msg[] = {0xFF};
    server.broadcast(msg, 1, true);
    pumpN(server, {&c1, &c2}, 20);

    CHECK(s1.countType(Event::Type::Receive) == 1);
    CHECK(s2.countType(Event::Type::Receive) == 1);

    server.shutdown();
    c1.shutdown();
    c2.shutdown();
}

// ---------------------------------------------------------------------------
// Disconnect
// ---------------------------------------------------------------------------

TEST_CASE("disconnect fires callback", "[network][integration]") {
    ENetNetwork server, client;
    EventSink srvSink, cliSink;
    REQUIRE(server.init());
    REQUIRE(client.init());
    server.setEventHandler(&srvSink);
    client.setEventHandler(&cliSink);

    REQUIRE(server.bind(nullptr, 19008, 4));
    REQUIRE(client.connect("127.0.0.1", 19008));
    pump(server, client, 20);
    REQUIRE(srvSink.countType(Event::Type::Connect) == 1);

    client.disconnect();
    pump(server, client, 20);

    CHECK(srvSink.countType(Event::Type::Disconnect) == 1);

    server.shutdown();
    client.shutdown();
}

TEST_CASE("send to disconnected peer returns false", "[network][integration]") {
    ENetNetwork server, client;
    EventSink srvSink, cliSink;
    REQUIRE(server.init());
    REQUIRE(client.init());
    server.setEventHandler(&srvSink);
    client.setEventHandler(&cliSink);

    REQUIRE(server.bind(nullptr, 19009, 4));
    REQUIRE(client.connect("127.0.0.1", 19009));
    pump(server, client, 20);
    REQUIRE(srvSink.countType(Event::Type::Connect) == 1);

    client.disconnect();
    pump(server, client, 20);

    const uint8_t buf[] = {1};
    bool ok = server.send(0, buf, 1, true);
    CHECK_FALSE(ok);
    CHECK(server.getLastError() != nullptr);

    server.shutdown();
    client.shutdown();
}

// ---------------------------------------------------------------------------
// Server full
// ---------------------------------------------------------------------------

TEST_CASE("server full rejects new connection", "[network][integration]") {
    ENetNetwork server, c1, c2;
    EventSink srvSink, s1, s2;
    REQUIRE(server.init());
    REQUIRE(c1.init());
    REQUIRE(c2.init());
    server.setEventHandler(&srvSink);
    c1.setEventHandler(&s1);
    c2.setEventHandler(&s2);

    REQUIRE(server.bind(nullptr, 19010, 1)); // only 1 peer slot
    REQUIRE(c1.connect("127.0.0.1", 19010));
    pumpN(server, {&c1, &c2}, 20);
    REQUIRE(s1.countType(Event::Type::Connect) == 1);

    REQUIRE(c2.connect("127.0.0.1", 19010));
    // Pump 100 x 10 ms = 1 s -- enough to see if the server accepted a second peer.
    pumpN(server, {&c1, &c2}, 100);

    CHECK(srvSink.countType(Event::Type::Connect) == 1);
    CHECK(server.getPeerCount() == 1);

    server.shutdown();
    c1.shutdown();
    c2.shutdown();
}

// ---------------------------------------------------------------------------
// IPv6 dual-stack (enet6)
// Ports 19021-19024 are reserved for these tests.
// ---------------------------------------------------------------------------

TEST_CASE("IPv6 loopback round-trip", "[network][integration]") {
    ENetNetwork server, client;
    EventSink srvSink, cliSink;
    REQUIRE(server.init());
    REQUIRE(client.init());
    server.setEventHandler(&srvSink);
    client.setEventHandler(&cliSink);

    REQUIRE(server.bind("::", 19021, 4));
    REQUIRE(client.connect("::1", 19021));

    pump(server, client, 20);

    REQUIRE(srvSink.countType(Event::Type::Connect) == 1);
    REQUIRE(cliSink.countType(Event::Type::Connect) == 1);

    const uint8_t payload[] = {0x69, 0x70, 0x76, 0x36};
    REQUIRE(client.send(0, payload, sizeof(payload), true));
    pump(server, client, 20);

    REQUIRE(srvSink.countType(Event::Type::Receive) == 1);
    const auto& ev = srvSink.events.back();
    REQUIRE(ev.data.size() == sizeof(payload));
    CHECK(std::memcmp(ev.data.data(), payload, sizeof(payload)) == 0);

    server.shutdown();
    client.shutdown();
}

TEST_CASE("getPeerAddress bracket notation for IPv6 peer", "[network][integration]") {
    ENetNetwork server, client;
    EventSink srvSink, cliSink;
    REQUIRE(server.init());
    REQUIRE(client.init());
    server.setEventHandler(&srvSink);
    client.setEventHandler(&cliSink);

    REQUIRE(server.bind("::", 19022, 4));
    REQUIRE(client.connect("::1", 19022));

    pump(server, client, 20);

    REQUIRE(srvSink.countType(Event::Type::Connect) == 1);

    const char* addr = server.getPeerAddress(0);
    REQUIRE(addr != nullptr);
    const std::string addrStr(addr);
    // IPv6 peers must use bracket notation: [::1]:port
    CHECK(addrStr.front() == '[');
    CHECK(addrStr.find("]:") != std::string::npos);

    server.shutdown();
    client.shutdown();
}

TEST_CASE("dual-stack: IPv4 client connects to :: server", "[network][integration]") {
    // Acceptance criterion from #180: fl-server bound on :: must accept IPv4 clients.
    // On Windows, IPV6_V6ONLY may default to 1 (IPv6-only); skip if enet6 does not
    // set IPV6_V6ONLY=0 automatically.
    ENetNetwork server, client;
    EventSink srvSink, cliSink;
    REQUIRE(server.init());
    REQUIRE(client.init());
    server.setEventHandler(&srvSink);
    client.setEventHandler(&cliSink);

    REQUIRE(server.bind("::", 19023, 4));
    REQUIRE(client.connect("127.0.0.1", 19023));

    pump(server, client, 50);

    // On platforms where dual-stack works the server sees an IPv4-mapped peer.
    if (srvSink.countType(Event::Type::Connect) == 0) {
        // Dual-stack not available on this platform/configuration -- skip gracefully.
        server.shutdown();
        client.shutdown();
        SKIP("dual-stack IPv4-mapped not available on this platform");
    }

    REQUIRE(cliSink.countType(Event::Type::Connect) == 1);

    const uint8_t payload[] = {0xD5};
    REQUIRE(client.send(0, payload, 1, true));
    pump(server, client, 20);

    REQUIRE(srvSink.countType(Event::Type::Receive) == 1);
    CHECK(srvSink.events.back().data[0] == 0xD5);

    server.shutdown();
    client.shutdown();
}

TEST_CASE("getPeerAddress plain format preserved for IPv4 peer", "[network][integration]") {
    // Regression: enet6 upgrade must not add brackets to IPv4 peer addresses.
    ENetNetwork server, client;
    EventSink srvSink, cliSink;
    REQUIRE(server.init());
    REQUIRE(client.init());
    server.setEventHandler(&srvSink);
    client.setEventHandler(&cliSink);

    REQUIRE(server.bind(nullptr, 19024, 4));
    REQUIRE(client.connect("127.0.0.1", 19024));

    pump(server, client, 20);

    REQUIRE(srvSink.countType(Event::Type::Connect) == 1);

    const char* addr = server.getPeerAddress(0);
    REQUIRE(addr != nullptr);
    const std::string addrStr(addr);
    // IPv4 address must be plain dotted-decimal: no leading bracket.
    CHECK(addrStr.front() != '[');
    CHECK(addrStr.find("127.0.0.1") != std::string::npos);

    server.shutdown();
    client.shutdown();
}

// ---------------------------------------------------------------------------
// Pre-handshake rate limiting
// Ports 19030-19033 are reserved for these tests.
// ---------------------------------------------------------------------------

TEST_CASE("pre-handshake rate limit blocks excess connect attempts", "[network][integration]") {
    using Clock = std::chrono::steady_clock;
    Clock::time_point fakeNow = Clock::now();

    ENetNetwork server, c1, c2, c3;
    EventSink srvSink, s1, s2, s3;
    REQUIRE(server.init());
    REQUIRE(c1.init());
    REQUIRE(c2.init());
    REQUIRE(c3.init());
    server.setEventHandler(&srvSink);
    c1.setEventHandler(&s1);
    c2.setEventHandler(&s2);
    c3.setEventHandler(&s3);

    REQUIRE(server.bind(nullptr, 19030, 8));
    server.setPreHandshakeRateLimit(2, 2000);
    server.setPreHandshakeClockOverride([&] { return fakeNow; });

    // Flush all three SYNs onto the wire before the server processes any of them.
    REQUIRE(c1.connect("127.0.0.1", 19030));
    REQUIRE(c2.connect("127.0.0.1", 19030));
    REQUIRE(c3.connect("127.0.0.1", 19030));
    c1.service(0);
    c2.service(0);
    c3.service(0);
    // Server receives all three SYNs in one pass: first two allowed, third dropped.
    server.service(0);

    // Pump to complete handshakes for the two allowed peers.
    // Clock is fixed so all c3 retries are also dropped.
    pumpN(server, {&c1, &c2, &c3}, 100);

    CHECK(srvSink.countType(Event::Type::Connect) == 2);
    CHECK(server.getPeerCount() == 2);

    server.shutdown();
    c1.shutdown();
    c2.shutdown();
    c3.shutdown();
}

TEST_CASE("pre-handshake rate limit window expiry allows reconnection", "[network][integration]") {
    using Clock = std::chrono::steady_clock;
    Clock::time_point fakeNow = Clock::now();

    ENetNetwork server, c1, c2, c3;
    EventSink srvSink, s1, s2, s3;
    REQUIRE(server.init());
    REQUIRE(c1.init());
    REQUIRE(c2.init());
    REQUIRE(c3.init());
    server.setEventHandler(&srvSink);
    c1.setEventHandler(&s1);
    c2.setEventHandler(&s2);
    c3.setEventHandler(&s3);

    REQUIRE(server.bind(nullptr, 19031, 8));
    server.setPreHandshakeRateLimit(1, 1000);
    server.setPreHandshakeClockOverride([&] { return fakeNow; });

    // c1 SYN -> count=1 -> allowed; c2 SYN -> count=1 >= limit=1 -> dropped.
    REQUIRE(c1.connect("127.0.0.1", 19031));
    REQUIRE(c2.connect("127.0.0.1", 19031));
    c1.service(0);
    c2.service(0);
    server.service(0);
    pumpN(server, {&c1, &c2}, 60);

    REQUIRE(srvSink.countType(Event::Type::Connect) == 1);
    CHECK(server.getPeerCount() == 1);

    // Advance the injected clock past the 1000 ms window.
    fakeNow += std::chrono::milliseconds(1500);

    // Use a fresh client (c3) rather than waiting for c2's ENet retry timer
    // (ENet's initial retry backoff is ~1000 ms; pumpN completes in microseconds
    // of real time, so we cannot rely on c2 retrying within the pump window).
    // c3's SYN is now allowed because c1's timestamp has been pruned.
    REQUIRE(c3.connect("127.0.0.1", 19031));
    c3.service(0);
    server.service(0);
    pumpN(server, {&c1, &c2, &c3}, 60);

    CHECK(srvSink.countType(Event::Type::Connect) == 2);
    CHECK(server.getPeerCount() == 2);

    server.shutdown();
    c1.shutdown();
    c2.shutdown();
    c3.shutdown();
}

TEST_CASE("pre-handshake rate limit 0 disables pre-handshake filter", "[network][integration]") {
    ENetNetwork server, c1, c2, c3;
    EventSink srvSink, s1, s2, s3;
    REQUIRE(server.init());
    REQUIRE(c1.init());
    REQUIRE(c2.init());
    REQUIRE(c3.init());
    server.setEventHandler(&srvSink);
    c1.setEventHandler(&s1);
    c2.setEventHandler(&s2);
    c3.setEventHandler(&s3);

    REQUIRE(server.bind(nullptr, 19032, 8));
    server.setPreHandshakeRateLimit(0, 1000); // 0 = disabled

    REQUIRE(c1.connect("127.0.0.1", 19032));
    REQUIRE(c2.connect("127.0.0.1", 19032));
    REQUIRE(c3.connect("127.0.0.1", 19032));
    pumpN(server, {&c1, &c2, &c3}, 100);

    CHECK(srvSink.countType(Event::Type::Connect) == 3);
    CHECK(server.getPeerCount() == 3);

    server.shutdown();
    c1.shutdown();
    c2.shutdown();
    c3.shutdown();
}

TEST_CASE("pre-handshake rate limit passes through established peer traffic", "[network][integration]") {
    using Clock = std::chrono::steady_clock;
    Clock::time_point fakeNow = Clock::now();

    ENetNetwork server, c1, c2;
    EventSink srvSink, s1, s2;
    REQUIRE(server.init());
    REQUIRE(c1.init());
    REQUIRE(c2.init());
    server.setEventHandler(&srvSink);
    c1.setEventHandler(&s1);
    c2.setEventHandler(&s2);

    REQUIRE(server.bind(nullptr, 19033, 8));
    server.setPreHandshakeRateLimit(1, 60000); // limit=1, very long window
    server.setPreHandshakeClockOverride([&] { return fakeNow; });

    // c1 connects (count=1 = limit reached for 127.0.0.1).
    REQUIRE(c1.connect("127.0.0.1", 19033));
    c1.service(0);
    server.service(0);
    pumpN(server, {&c1}, 30);
    REQUIRE(srvSink.countType(Event::Type::Connect) == 1);

    // c1 sends 10 reliable packets. Each passes the intercept (peerID != 0xFFF)
    // without incrementing the rate-limit count.
    for (int i = 0; i < 10; ++i) {
        const uint8_t payload[] = {static_cast<uint8_t>(i)};
        REQUIRE(c1.send(0, payload, 1, true));
    }
    pumpN(server, {&c1}, 20);
    // All 10 packets must reach the server -- none dropped by the intercept.
    CHECK(srvSink.countType(Event::Type::Receive) == 10);

    // c2 attempts to connect. SYN has peerID=0xFFF; count=1 (from c1 SYN only,
    // not inflated by the 10 data packets) >= limit=1 -- dropped.
    REQUIRE(c2.connect("127.0.0.1", 19033));
    c2.service(0);
    server.service(0);
    pumpN(server, {&c1, &c2}, 50);

    CHECK(srvSink.countType(Event::Type::Connect) == 1);
    CHECK(server.getPeerCount() == 1);

    server.shutdown();
    c1.shutdown();
    c2.shutdown();
}
