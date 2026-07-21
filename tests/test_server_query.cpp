// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "mock_hal.h"
#include "net/GameProtocol.h"
#include "net/ServerQueryClient.h"
#include "net/ServerQueryResponder.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <thread>

#if defined(_WIN32)
using SockLen = int;
#else
using SockLen = socklen_t;
#endif

using namespace fl;

namespace {

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

// Ask the OS for an unused UDP port (bind 0, read back, close) — the test_lan_discovery idiom, so
// concurrent ctest processes never share a port.
uint16_t freeUdpPort() {
#if defined(_WIN32)
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    REQUIRE(s != INVALID_SOCKET);
#else
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    REQUIRE(s >= 0);
#endif
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    REQUIRE(::bind(s, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) == 0);
    sockaddr_in got{};
    SockLen len = sizeof(got);
    REQUIRE(::getsockname(s, reinterpret_cast<sockaddr*>(&got), &len) == 0);
    const uint16_t port = ntohs(got.sin_port);
#if defined(_WIN32)
    closesocket(s);
#else
    ::close(s);
#endif
    REQUIRE(port != 0);
    return port;
}

} // namespace

TEST_CASE("Server query protocol: wire layout", "[server_query][protocol]") {
    CHECK(sizeof(fl::MsgServerQuery) == 192u);
    CHECK(sizeof(fl::MsgServerInfo) == 184u);
    CHECK(offsetof(fl::MsgServerInfo, name) == 24u);
    CHECK(offsetof(fl::MsgServerInfo, modeId) == 88u);
    CHECK(offsetof(fl::MsgServerInfo, mission) == 120u);
    // The request is at least as large as the response (anti-amplification).
    CHECK(sizeof(fl::MsgServerQuery) >= sizeof(fl::MsgServerInfo));
}

TEST_CASE("Server query protocol: happy path over loopback with RTT", "[server_query]") {
    [[maybe_unused]] WsaInit wsa;
    MockLogger log;
    const uint16_t port = freeUdpPort();

    ServerQueryResponder responder(port, log);
    REQUIRE(responder.start());
    ServerQueryResponder::StaticInfo si;
    si.name = "Test Server";
    si.modeId = "builtin:tdm";
    si.mission = "Fjord";
    si.gamePort = 4778;
    si.maxPlayers = 32;
    responder.setStaticInfo(std::move(si));
    responder.setDynamicInfo({7, false, 0});

    ServerQueryClient client(log, 3000);
    REQUIRE(client.isOpen());
    const uint32_t nonce = client.query("127.0.0.1", port);
    REQUIRE(nonce != 0u);

    // Poll for the reply (bounded).
    std::vector<ServerQueryClient::Result> results;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    while (std::chrono::steady_clock::now() < deadline) {
        client.poll();
        results = client.results();
        if (!results.empty())
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(results.size() == 1u);
    CHECK(std::string(results[0].info.name) == "Test Server");
    CHECK(std::string(results[0].info.modeId) == "builtin:tdm");
    CHECK(std::string(results[0].info.mission) == "Fjord");
    CHECK(results[0].info.playerCount == 7u);
    CHECK(results[0].info.gamePort == 4778u);
    CHECK(results[0].rttMs >= 0.f);
    CHECK(results[0].info.nonce == nonce); // echoed

    responder.stop();
}

TEST_CASE("Server query protocol: short datagram is dropped (anti-amplification)", "[server_query]") {
    [[maybe_unused]] WsaInit wsa;
    MockLogger log;
    const uint16_t port = freeUdpPort();
    ServerQueryResponder responder(port, log);
    REQUIRE(responder.start());
    responder.setStaticInfo({"S", "", "", 4778, 8, 0});

    // Send a too-short datagram directly; the responder must not reply.
#if defined(_WIN32)
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
#else
    int s = socket(AF_INET, SOCK_DGRAM, 0);
#endif
    sockaddr_in d{};
    d.sin_family = AF_INET;
    d.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &d.sin_addr);
    uint8_t tiny[8]{static_cast<uint8_t>(fl::MsgId::ServerQuery)};
    sendto(s, reinterpret_cast<const char*>(tiny), sizeof(tiny), 0, reinterpret_cast<const sockaddr*>(&d), sizeof(d));

    // A proper query from the client must still succeed (proving the responder is alive and only the
    // short one was dropped).
    ServerQueryClient client(log, 3000);
    REQUIRE(client.query("127.0.0.1", port) != 0u);
    std::vector<ServerQueryClient::Result> results;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    while (std::chrono::steady_clock::now() < deadline) {
        client.poll();
        results = client.results();
        if (!results.empty())
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(results.size() == 1u);

#if defined(_WIN32)
    closesocket(s);
#else
    ::close(s);
#endif
    responder.stop();
}
