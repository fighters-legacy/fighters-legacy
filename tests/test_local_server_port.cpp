// SPDX-License-Identifier: GPL-3.0-or-later
//
// #1054 regression: the client-provisioned single-player server must not contend for the standard
// game port.
//
// The bug: the server browser's LAN DiscoveryListener bound the wildcard game port 4778 for the whole
// process lifetime (it was created at startup, single-player included). The embedded fl-server then
// could not bind 127.0.0.1:4778, because enet6's enet_host_create sets no SO_REUSEADDR — so Instant
// Action and Free Flight failed with "Port already in use." before a session ever started.
//
// Two things are locked here: the port ROLES stay distinct (every platform), and the socket MECHANISM
// that makes a shared port fatal still behaves as the fix assumes (POSIX only — see below).

#include "LocalServer.h"
#include "net/DiscoveryBeacon.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>

using namespace fl;

TEST_CASE("embedded single-player server does not share the standard game port", "[localserver][1054]") {
    // The port the LAN discovery beacon/listener use -- which is the game port, by design: the
    // beacon broadcasts *to* it, so the browser's listener binds it.
    const uint16_t gamePort = DiscoveryBeacon::Config{}.port;
    REQUIRE(gamePort == 4778);

    // The embedded server must not sit on the game port. This is the regression: it used to default
    // to exactly this port, so the browser's listener and the embedded server fought over it.
    CHECK(LocalServer::kLocalServerPort != gamePort);

    // Nor on the server-query responder's port. fl-server auto-assigns that as game port + 1, so
    // "one above 4778" is already spoken for -- a local dedicated server would own 4779 and the
    // embedded server would collide with its query socket instead of its game socket.
    CHECK(LocalServer::kLocalServerPort != static_cast<uint16_t>(gamePort + 1));

    // And the embedded server's OWN derived query port must not land back on the game port. This is
    // what rules out 4777: fl-server derives an unconfigured query port as game port + 1, so an
    // embedded server on 4777 would bind 4778 for queries and reintroduce #1054 through the other
    // socket. The server is spawned with --no-discovery so no responder exists today, but the port
    // choice must survive that flag being dropped rather than depend on it.
    CHECK(static_cast<uint16_t>(LocalServer::kLocalServerPort + 1) != gamePort);
}

// ---------------------------------------------------------------------------
// Socket mechanism — POSIX only.
//
// The refusal asserted below is a BSD-socket property, not a universal one. Windows gives
// SO_REUSEADDR different semantics (there is no SO_REUSEPORT, and exclusivity is opt-in via
// SO_EXCLUSIVEADDRUSE), so a wildcard holder there does NOT refuse a later specific-address bind --
// it quietly permits the overlap, and which socket receives a given datagram is unspecified.
//
// That difference is a reason to separate the ports by design rather than an exception to it: on
// Linux/macOS a shared port fails loudly at bind (which is how #1054 presented), and on Windows the
// same clash would fail silently, delivering beacons to whichever socket the stack picked. Neither is
// acceptable, so the port separation above is unconditional.
// ---------------------------------------------------------------------------
#if !defined(_WIN32)

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

struct SockGuard {
    int fd{-1};
    ~SockGuard() {
        if (fd >= 0)
            ::close(fd);
    }
    SockGuard() = default;
    SockGuard(const SockGuard&) = delete;
    SockGuard& operator=(const SockGuard&) = delete;
    [[nodiscard]] bool valid() const noexcept {
        return fd >= 0;
    }
};

// A DiscoveryListener-style UDP socket: wildcard address, SO_REUSEADDR (+SO_REUSEPORT where it
// exists). Binds port 0 so the kernel hands out a free port -- this test can never collide with a
// developer's running fl-server or with a parallel ctest job.
bool openWildcardReuse(SockGuard& g, uint16_t& assignedPort) {
    g.fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (!g.valid())
        return false;
    int reuse = 1;
    setsockopt(g.fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
    setsockopt(g.fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(0);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(g.fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0)
        return false;

    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    if (getsockname(g.fd, reinterpret_cast<sockaddr*>(&bound), &len) != 0)
        return false;
    assignedPort = ntohs(bound.sin_port);
    return assignedPort != 0;
}

// An enet6-style UDP bind: a specific address, NO socket options. This mirrors what
// enet_host_create does (IPV6ONLY/NONBLOCK/BROADCAST/buffer sizes, but never SO_REUSEADDR),
// which is precisely why it loses a contested port.
bool bindPlainLoopback(SockGuard& g, uint16_t port) {
    g.fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (!g.valid())
        return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return bind(g.fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0;
}

} // namespace

TEST_CASE("a wildcard REUSEADDR holder blocks an enet-style bind on the same port", "[localserver][1054]") {
    // This is the mechanism behind #1054 on the platform it was reported on. If this ever stops
    // holding, the reasoning in LocalServer.h should be revisited.
    SockGuard listener;
    uint16_t port = 0;
    REQUIRE(openWildcardReuse(listener, port));

    SECTION("the contested port is refused") {
        SockGuard embedded;
        CHECK_FALSE(bindPlainLoopback(embedded, port));
    }

    SECTION("a different port is unaffected") {
        // The fix in one line: ask for a port nobody is holding and the same bind succeeds.
        SockGuard embedded;
        CHECK(bindPlainLoopback(embedded, 0));
    }
}

#endif // !_WIN32
