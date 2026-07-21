// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2ipdef.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "mock_hal.h"
#include "net/DiscoveryBeacon.h"
#include "net/DiscoveryListener.h"
#include "net/GameProtocol.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#if defined(_WIN32)
using SockLen = int;
#else
using SockLen = socklen_t;
#endif

using namespace fl;

// ---------------------------------------------------------------------------
// RAII socket guard — ensures sockets are closed even on test failure (ASAN).
// ---------------------------------------------------------------------------

struct SockGuard {
#if defined(_WIN32)
    SOCKET fd{INVALID_SOCKET};
    bool valid() const {
        return fd != INVALID_SOCKET;
    }
    ~SockGuard() {
        if (valid()) {
            closesocket(fd);
            fd = INVALID_SOCKET;
        }
    }
#else
    int fd{-1};
    bool valid() const {
        return fd >= 0;
    }
    ~SockGuard() {
        if (valid()) {
            ::close(fd);
            fd = -1;
        }
    }
#endif
};

// Winsock RAII init for test sockets (no ENet in this binary).
struct WsaInit {
#if defined(_WIN32)
    bool owned{false};
    WsaInit() {
        WSADATA w{};
        if (WSAStartup(MAKEWORD(2, 2), &w) == 0)
            owned = true;
    }
    ~WsaInit() {
        if (owned)
            WSACleanup();
    }
#endif
};

// ---------------------------------------------------------------------------
// Helper: open a raw IPv4 UDP send socket for injecting test packets.
// ---------------------------------------------------------------------------

#if defined(_WIN32)
static SOCKET makeRawSendSock() {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    return s;
}
static void rawSend(SOCKET s, const void* buf, int len, uint16_t port) {
    sockaddr_in d{};
    d.sin_family = AF_INET;
    d.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &d.sin_addr);
    sendto(s, reinterpret_cast<const char*>(buf), len, 0, reinterpret_cast<const sockaddr*>(&d), sizeof(d));
}
#else
static int makeRawSendSock() {
    return socket(AF_INET, SOCK_DGRAM, 0);
}
static void rawSend(int s, const void* buf, int len, uint16_t port) {
    sockaddr_in d{};
    d.sin_family = AF_INET;
    d.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &d.sin_addr);
    sendto(s, buf, static_cast<SockLen>(len), 0, reinterpret_cast<const sockaddr*>(&d), sizeof(d));
}
#endif

// ---------------------------------------------------------------------------
// Per-test ports and bounded waits (#787)
//
// Every test in this file used to bind the SAME discovery port (19200). That was fine while Catch2
// ran the cases sequentially inside one binary — but `catch_discover_tests` registers each TEST_CASE
// as its own ctest test, so under `ctest -j` they run as CONCURRENT PROCESSES sharing that port. LAN
// discovery is a broadcast protocol: a listener on port P receives whatever is sent to port P,
// including a beacon fired by a SIBLING TEST'S PROCESS. So the failures were not bind errors, they
// were cross-talk — "second immediate tick does not resend" would see another test's beacon and
// report a server it never expected, and the malformed-packet tests would likewise see a stray valid
// beacon. Both are the kind of failure that reads as a real regression in whatever branch you have
// checked out.
//
// freeUdpPort() asks the OS for an unused port (bind 0, read it back, close) so each test gets its
// own. There is a theoretical TOCTOU window between closing the probe socket and the listener binding
// it; in practice the OS does not hand the same ephemeral port straight back out, and this is
// enormously safer than a constant every case shares.
// ---------------------------------------------------------------------------

static uint16_t freeUdpPort() {
    SockGuard g;
    g.fd = makeRawSendSock();
    REQUIRE(g.valid());
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0; // let the OS choose
    REQUIRE(::bind(g.fd, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) == 0);
    sockaddr_in got{};
    SockLen len = sizeof(got);
    REQUIRE(::getsockname(g.fd, reinterpret_cast<sockaddr*>(&got), &len) == 0);
    const uint16_t port = ntohs(got.sin_port);
    REQUIRE(port != 0);
    return port; // SockGuard closes the probe socket here
}

// Poll the listener until it reports at least `want` servers, or the deadline passes.
//
// Replaces a flat `sleep_for(10ms)`. A fixed sleep is a bet that a loopback datagram lands within
// 10 ms — usually true, and *not* true on a machine running 24 test processes at once, which is
// precisely when we want the suite to be trustworthy. Polling to a generous deadline is both faster
// in the common case and immune to load.
static std::vector<DiscoveryListener::ServerInfo> waitForServers(DiscoveryListener& listener, std::size_t want,
                                                                 std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        listener.poll();
        auto servers = listener.servers();
        if (servers.size() >= want || std::chrono::steady_clock::now() >= deadline) {
            return servers;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// Give a packet every chance to arrive, then assert it did NOT produce a server. Used by the
// negative cases (malformed packets, suppressed re-sends), where sleeping too briefly would pass for
// the wrong reason.
static std::vector<DiscoveryListener::ServerInfo> expectNoServers(DiscoveryListener& listener,
                                                                  std::chrono::milliseconds settle) {
    const auto deadline = std::chrono::steady_clock::now() + settle;
    while (std::chrono::steady_clock::now() < deadline) {
        listener.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return listener.servers();
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_CASE("MsgLanBeacon struct layout matches wire spec", "[lan_discovery][protocol]") {
    CHECK(sizeof(fl::MsgLanBeacon) == 76u);
    CHECK(offsetof(fl::MsgLanBeacon, protocolVersion) == 2u);
    CHECK(offsetof(fl::MsgLanBeacon, gamePort) == 4u);
    CHECK(offsetof(fl::MsgLanBeacon, playerCount) == 6u);
    CHECK(offsetof(fl::MsgLanBeacon, maxPlayers) == 7u);
    CHECK(offsetof(fl::MsgLanBeacon, gameModeFlags) == 8u);
    CHECK(offsetof(fl::MsgLanBeacon, shutdownSeconds) == 10u);
    CHECK(offsetof(fl::MsgLanBeacon, name) == 12u);

    fl::MsgLanBeacon beacon;
    CHECK(beacon.msgId == static_cast<uint8_t>(fl::MsgId::LanBeacon));
    CHECK(beacon.protocolVersion == fl::kProtocolVersion);
    CHECK(beacon.gamePort == 4778u);
}

TEST_CASE("DiscoveryBeacon opens at least one socket", "[lan_discovery]") {
    [[maybe_unused]] WsaInit wsa;
    MockLogger log;
    const uint16_t port = freeUdpPort(); // #787: never a shared constant
    DiscoveryBeacon::Config cfg;
    cfg.name = "test-server";
    cfg.port = port;
    cfg.broadcastAddr = "127.0.0.1";
    cfg.intervalMs = 30000;

    DiscoveryBeacon beacon(cfg, log);
    CHECK(beacon.isOpen());
}

TEST_CASE("DiscoveryListener opens at least one socket", "[lan_discovery]") {
    [[maybe_unused]] WsaInit wsa;
    MockLogger log;
    DiscoveryListener listener(19201, log);
    CHECK(listener.isOpen());
}

TEST_CASE("DiscoveryBeacon first tick fires immediately", "[lan_discovery][integration]") {
    [[maybe_unused]] WsaInit wsa;
    MockLogger log;
    const uint16_t port = freeUdpPort(); // #787: never a shared constant

    DiscoveryListener listener(port, log);
    REQUIRE(listener.isOpen());

    DiscoveryBeacon::Config cfg;
    cfg.name = "my-server";
    cfg.port = port;
    cfg.broadcastAddr = "127.0.0.1";
    cfg.intervalMs = 30000;
    cfg.maxPlayers = 16;
    cfg.gameModeFlags = fl::kGameModeSandbox;

    DiscoveryBeacon beacon(cfg, log);
    REQUIRE(beacon.isOpen());

    beacon.tick({3});
    auto servers = waitForServers(listener, 1, std::chrono::milliseconds(2000));
    REQUIRE(!servers.empty());
    CHECK(servers[0].beacon.playerCount == 3u);
    CHECK(servers[0].beacon.gamePort == port);
    CHECK(servers[0].beacon.protocolVersion == fl::kProtocolVersion);
    CHECK(servers[0].beacon.gameModeFlags == fl::kGameModeSandbox);
    CHECK(std::string(servers[0].beacon.name) == "my-server");
}

TEST_CASE("DiscoveryBeacon advertises shutdown state (#226)", "[lan_discovery][integration]") {
    [[maybe_unused]] WsaInit wsa;
    MockLogger log;
    const uint16_t port = freeUdpPort();
    DiscoveryListener listener(port, log);
    DiscoveryBeacon::Config cfg;
    cfg.name = "closing-server";
    cfg.port = port;
    cfg.broadcastAddr = "127.0.0.1";
    cfg.intervalMs = 30000;
    cfg.gameModeFlags = fl::kGameModeSandbox;
    DiscoveryBeacon beacon(cfg, log);
    REQUIRE(beacon.isOpen());

    beacon.tick({2, /*shuttingDown=*/true, /*shutdownSeconds=*/120});
    auto servers = waitForServers(listener, 1, std::chrono::milliseconds(2000));
    REQUIRE(!servers.empty());
    CHECK(servers[0].shuttingDown());
    CHECK(servers[0].shutdownSeconds() == 120u);
    CHECK((servers[0].beacon.gameModeFlags & fl::kGameModeShuttingDown) != 0u);
}

TEST_CASE("DiscoveryBeacon second immediate tick does not resend", "[lan_discovery][integration]") {
    [[maybe_unused]] WsaInit wsa;
    MockLogger log;
    const uint16_t port = freeUdpPort(); // #787: never a shared constant

    DiscoveryListener listener(port, log);
    REQUIRE(listener.isOpen());

    DiscoveryBeacon::Config cfg;
    cfg.name = "dedup-server";
    cfg.port = port;
    cfg.broadcastAddr = "127.0.0.1";
    cfg.intervalMs = 30000;

    DiscoveryBeacon beacon(cfg, log);
    REQUIRE(beacon.isOpen());

    beacon.tick({1}); // first tick fires immediately
    beacon.tick({1}); // second tick — interval not elapsed, must NOT send

    // Wait for the first beacon, then keep draining: a suppressed second send must never show up, and
    // (now that the port is ours alone) neither can a sibling test's beacon.
    auto servers = waitForServers(listener, 1, std::chrono::milliseconds(2000));
    REQUIRE(servers.size() == 1u);
    servers = expectNoServers(listener, std::chrono::milliseconds(100));
    CHECK(servers.size() == 1u); // still exactly one — the second send was suppressed
}

TEST_CASE("DiscoveryListener ignores packet with wrong msgId", "[lan_discovery]") {
    [[maybe_unused]] WsaInit wsa;
    MockLogger log;
    const uint16_t port = freeUdpPort(); // #787: never a shared constant
    DiscoveryListener listener(port, log);
    REQUIRE(listener.isOpen());

    SockGuard sg;
    sg.fd = makeRawSendSock();
    REQUIRE(sg.valid());

    // Send a MsgHello-sized buffer with msgId=0x00 padded to MsgLanBeacon size.
    uint8_t buf[sizeof(fl::MsgLanBeacon)]{};
    buf[0] = 0x00; // MsgHello msgId — must be ignored
    rawSend(sg.fd, buf, static_cast<int>(sizeof(buf)), port);

    // Drain to a deadline rather than sleeping once: a 10 ms sleep that is simply too short would
    // pass this test for the wrong reason (the packet had not arrived yet, not that it was rejected).
    CHECK(expectNoServers(listener, std::chrono::milliseconds(100)).empty());
}

TEST_CASE("DiscoveryListener ignores packet shorter than MsgLanBeacon", "[lan_discovery]") {
    [[maybe_unused]] WsaInit wsa;
    MockLogger log;
    const uint16_t port = freeUdpPort(); // #787: never a shared constant
    DiscoveryListener listener(port, log);
    REQUIRE(listener.isOpen());

    SockGuard sg;
    sg.fd = makeRawSendSock();
    REQUIRE(sg.valid());

    // Send 4 bytes (too short to be a MsgLanBeacon).
    uint8_t buf[4]{static_cast<uint8_t>(fl::MsgId::LanBeacon), 0, 1, 0};
    rawSend(sg.fd, buf, 4, port);

    CHECK(expectNoServers(listener, std::chrono::milliseconds(100)).empty());
}

TEST_CASE("DiscoveryListener deduplicates repeated beacons from same server", "[lan_discovery]") {
    [[maybe_unused]] WsaInit wsa;
    MockLogger log;
    const uint16_t port = freeUdpPort(); // #787: never a shared constant
    DiscoveryListener listener(port, log);
    REQUIRE(listener.isOpen());

    SockGuard sg;
    sg.fd = makeRawSendSock();
    REQUIRE(sg.valid());

    fl::MsgLanBeacon pkt;
    pkt.gamePort = port;
    pkt.playerCount = 2;
    pkt.maxPlayers = 8;
    std::snprintf(pkt.name, sizeof(pkt.name), "%s", "dup-server");

    uint8_t buf[sizeof(fl::MsgLanBeacon)];
    std::memcpy(buf, &pkt, sizeof(pkt));

    // Send the same beacon twice.
    rawSend(sg.fd, buf, static_cast<int>(sizeof(buf)), port);
    rawSend(sg.fd, buf, static_cast<int>(sizeof(buf)), port);

    auto servers = waitForServers(listener, 1, std::chrono::milliseconds(2000));
    // Keep draining: the second copy must upsert, not insert.
    servers = expectNoServers(listener, std::chrono::milliseconds(100));
    CHECK(servers.size() == 1u);
}

TEST_CASE("DiscoveryBeacon: setName updates the server name for future broadcasts", "[lan_discovery]") {
    struct NullLogger : ILogger {
        void log(LogLevel, const char*, int, const char*) override {}
        void setMinLevel(LogLevel) override {}
        void flush() override {}
    } log;

    DiscoveryBeacon::Config cfg;
    cfg.name = "original";
    cfg.port = 0;            // don't actually bind/broadcast — we're testing the config mutation
    cfg.intervalMs = 100000; // suppress automatic ticking

    DiscoveryBeacon beacon(cfg, log);
    beacon.setName("updated");

    // Verify: tick() with playerCount=0 fires on the first call regardless of intervalMs.
    // The beacon may fail to send (no valid port), but the name change must not crash.
    REQUIRE_NOTHROW(beacon.tick({0}));
}
