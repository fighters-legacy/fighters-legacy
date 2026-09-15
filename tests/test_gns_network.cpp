// SPDX-License-Identifier: GPL-3.0-or-later
// Runtime contract tests for the GameNetworkingSockets backend (#507). Mirrors test_network.cpp's
// structure and holds GnsNetwork to the same INetwork contract as ENetNetwork. Uses real loopback
// UDP sockets, so cases are tagged [gns][integration].
//
// No case names a port. Each Catch2 TEST_CASE runs as its own process under ctest, so a fixed port
// (this file once used a 28100+ band to stay clear of the enet suite) is shared with whatever
// sibling case — or whatever other ctest invocation on the same machine — happens to be running
// (#787, #1329).
//
// The enet suite binds port 0 and reads the OS's pick back through ENetNetwork::boundPort(). GNS
// cannot do the first half: CSteamNetworkListenSocketDirectUDP::BInit refuses a zero port outright
// ("Must specify local port."), and INetwork::bind never promised that 0 means ephemeral — only
// enet6 happens to honour it. So here the OS is asked for a free port FIRST (freeUdpPort(), the
// test_lan_discovery idiom), GNS is bound to that, and GnsNetwork::boundPort() then confirms the
// socket really sits where it was asked to.
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "GnsNetwork.h"

#include "mock_network.h"

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string>
#include <vector>

using namespace fl;

namespace {

#if defined(_WIN32)
using SockLen = int;
#else
using SockLen = socklen_t;
#endif

// Ask the OS for an unused loopback UDP port (bind 0, read back, close). There is a theoretical
// TOCTOU window before GNS binds it; in practice the OS does not hand the same ephemeral port
// straight back out, and it is enormously safer than a constant every case shares. Call after
// GnsNetwork::init() — on Windows that is what brings Winsock up.
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

// Binds the server on a free loopback port and returns the port the client should connect to.
uint16_t bindEphemeral(GnsNetwork& server, int maxClients) {
    const uint16_t port = freeUdpPort();
    REQUIRE(server.bind("127.0.0.1", port, maxClients));
    REQUIRE(server.boundPort() == port); // GNS reports the socket it actually holds
    return port;
}

// Establishes a loopback connection; returns the server-side peerId of the connected client.
uint32_t connectLoopback(GnsNetwork& server, EventSink& serverSink, GnsNetwork& client, EventSink& clientSink) {
    const uint16_t port = bindEphemeral(server, 16);
    server.setEventHandler(&serverSink);
    REQUIRE(client.connect("127.0.0.1", port));
    client.setEventHandler(&clientSink);
    // Pump until BOTH ends have seen the connect. The server's Connect fires when it accepts; the
    // client's fires when its own state reaches Connected, which can be one service() later -- waiting
    // on the server alone and then requiring the client's count was a race the Windows leg lost.
    for (int i = 0;
         i < 60 && (serverSink.countType(Event::Type::Connect) == 0 || clientSink.countType(Event::Type::Connect) == 0);
         ++i)
        pump(server, client, 1, 15);
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
    a->shutdown();              // b still alive — global GNS interface must stay valid
    CHECK(b->boundPort() == 0); // not bound yet
    bindEphemeral(*b, 4);
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
    connectLoopback(server, ss, client, cs);
    CHECK(server.getPeerCount() == 1);
    client.shutdown();
    server.shutdown();
}

TEST_CASE("gns getPeerAddress returns ip:port", "[gns][integration]") {
    GnsNetwork server, client;
    REQUIRE(server.init());
    REQUIRE(client.init());
    EventSink ss, cs;
    const uint32_t peer = connectLoopback(server, ss, client, cs);
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
    connectLoopback(server, ss, client, cs);
    const std::vector<uint8_t> payload{1, 2, 3, 4, 5};
    REQUIRE(client.send(0, payload.data(), payload.size(), true));
    for (int i = 0; i < 40 && ss.countType(Event::Type::Receive) == 0; ++i)
        pump(server, client, 1, 15);
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
    const uint32_t peer = connectLoopback(server, ss, client, cs);
    const std::vector<uint8_t> payload{9, 8, 7};
    // Retry a few times — unreliable packets may be dropped during handshake settling.
    bool got = false;
    for (int attempt = 0; attempt < 10 && !got; ++attempt) {
        server.send(peer, payload.data(), payload.size(), false);
        for (int i = 0; i < 10 && cs.countType(Event::Type::Receive) == 0; ++i)
            pump(server, client, 1, 15);
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
    const uint16_t port = bindEphemeral(server, 16);
    server.setEventHandler(&ss);
    REQUIRE(c1.connect("127.0.0.1", port));
    c1.setEventHandler(&cs1);
    REQUIRE(c2.connect("127.0.0.1", port));
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
    connectLoopback(server, ss, client, cs);
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
    const uint32_t peer = connectLoopback(server, ss, client, cs);
    // Exchange a few packets so GNS has RTT samples, then read stats (should not crash / return sane).
    for (int i = 0; i < 20; ++i) {
        const uint8_t b = static_cast<uint8_t>(i);
        server.send(peer, &b, 1, true);
        pump(server, client, 1, 15);
    }
    const PeerLinkStats s = server.getPeerLinkStats(peer);
    CHECK(s.packetLoss >= 0.f);
    CHECK(s.packetLoss <= 1.f);
    client.shutdown();
    server.shutdown();
}
