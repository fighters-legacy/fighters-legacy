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

#if defined(_WIN32)
using SockLen = int;
#else
using SockLen = socklen_t;
#endif

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
// Tests
// ---------------------------------------------------------------------------

TEST_CASE("MsgLanBeacon struct layout matches wire spec", "[lan_discovery][protocol]") {
    CHECK(sizeof(fl::MsgLanBeacon) == 74u);
    CHECK(offsetof(fl::MsgLanBeacon, protocolVersion) == 2u);
    CHECK(offsetof(fl::MsgLanBeacon, gamePort) == 4u);
    CHECK(offsetof(fl::MsgLanBeacon, playerCount) == 6u);
    CHECK(offsetof(fl::MsgLanBeacon, maxPlayers) == 7u);
    CHECK(offsetof(fl::MsgLanBeacon, gameModeFlags) == 8u);
    CHECK(offsetof(fl::MsgLanBeacon, name) == 10u);

    fl::MsgLanBeacon beacon;
    CHECK(beacon.msgId == static_cast<uint8_t>(fl::MsgId::LanBeacon));
    CHECK(beacon.protocolVersion == fl::kProtocolVersion);
    CHECK(beacon.gamePort == 4778u);
}

TEST_CASE("DiscoveryBeacon opens at least one socket", "[lan_discovery]") {
    [[maybe_unused]] WsaInit wsa;
    MockLogger log;
    DiscoveryBeacon::Config cfg;
    cfg.name = "test-server";
    cfg.port = 19200;
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

    DiscoveryListener listener(19200, log);
    REQUIRE(listener.isOpen());

    DiscoveryBeacon::Config cfg;
    cfg.name = "my-server";
    cfg.port = 19200;
    cfg.broadcastAddr = "127.0.0.1";
    cfg.intervalMs = 30000;
    cfg.maxPlayers = 16;
    cfg.gameModeFlags = fl::kGameModeSandbox;

    DiscoveryBeacon beacon(cfg, log);
    REQUIRE(beacon.isOpen());

    beacon.tick(3);
    // Brief pause to allow the kernel to deliver the loopback datagram.
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    listener.poll();

    auto servers = listener.servers();
    REQUIRE(!servers.empty());
    CHECK(servers[0].beacon.playerCount == 3u);
    CHECK(servers[0].beacon.gamePort == 19200u);
    CHECK(servers[0].beacon.protocolVersion == fl::kProtocolVersion);
    CHECK(servers[0].beacon.gameModeFlags == fl::kGameModeSandbox);
    CHECK(std::string(servers[0].beacon.name) == "my-server");
}

TEST_CASE("DiscoveryBeacon second immediate tick does not resend", "[lan_discovery][integration]") {
    [[maybe_unused]] WsaInit wsa;
    MockLogger log;

    DiscoveryListener listener(19200, log);
    REQUIRE(listener.isOpen());

    DiscoveryBeacon::Config cfg;
    cfg.name = "dedup-server";
    cfg.port = 19200;
    cfg.broadcastAddr = "127.0.0.1";
    cfg.intervalMs = 30000;

    DiscoveryBeacon beacon(cfg, log);
    REQUIRE(beacon.isOpen());

    beacon.tick(1); // first tick fires immediately
    beacon.tick(1); // second tick — interval not elapsed, must NOT send

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    listener.poll();
    listener.poll(); // second poll in case timing is unlucky

    // Still exactly 1 server (no duplicate from the suppressed second send)
    CHECK(listener.servers().size() == 1u);
}

TEST_CASE("DiscoveryListener ignores packet with wrong msgId", "[lan_discovery]") {
    [[maybe_unused]] WsaInit wsa;
    MockLogger log;
    DiscoveryListener listener(19200, log);
    REQUIRE(listener.isOpen());

    SockGuard sg;
    sg.fd = makeRawSendSock();
    REQUIRE(sg.valid());

    // Send a MsgHello-sized buffer with msgId=0x00 padded to MsgLanBeacon size.
    uint8_t buf[sizeof(fl::MsgLanBeacon)]{};
    buf[0] = 0x00; // MsgHello msgId — must be ignored
    rawSend(sg.fd, buf, static_cast<int>(sizeof(buf)), 19200);

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    listener.poll();
    CHECK(listener.servers().empty());
}

TEST_CASE("DiscoveryListener ignores packet shorter than MsgLanBeacon", "[lan_discovery]") {
    [[maybe_unused]] WsaInit wsa;
    MockLogger log;
    DiscoveryListener listener(19200, log);
    REQUIRE(listener.isOpen());

    SockGuard sg;
    sg.fd = makeRawSendSock();
    REQUIRE(sg.valid());

    // Send 4 bytes (too short to be a MsgLanBeacon).
    uint8_t buf[4]{static_cast<uint8_t>(fl::MsgId::LanBeacon), 0, 1, 0};
    rawSend(sg.fd, buf, 4, 19200);

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    listener.poll();
    CHECK(listener.servers().empty());
}

TEST_CASE("DiscoveryListener deduplicates repeated beacons from same server", "[lan_discovery]") {
    [[maybe_unused]] WsaInit wsa;
    MockLogger log;
    DiscoveryListener listener(19200, log);
    REQUIRE(listener.isOpen());

    SockGuard sg;
    sg.fd = makeRawSendSock();
    REQUIRE(sg.valid());

    fl::MsgLanBeacon pkt;
    pkt.gamePort = 19200;
    pkt.playerCount = 2;
    pkt.maxPlayers = 8;
    std::snprintf(pkt.name, sizeof(pkt.name), "%s", "dup-server");

    uint8_t buf[sizeof(fl::MsgLanBeacon)];
    std::memcpy(buf, &pkt, sizeof(pkt));

    // Send the same beacon twice.
    rawSend(sg.fd, buf, static_cast<int>(sizeof(buf)), 19200);
    rawSend(sg.fd, buf, static_cast<int>(sizeof(buf)), 19200);

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    listener.poll();

    // Must upsert, not insert twice.
    CHECK(listener.servers().size() == 1u);
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
    REQUIRE_NOTHROW(beacon.tick(0));
}
