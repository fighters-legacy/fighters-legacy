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
#include "net/GameProtocol.h" // kDiscoveryPort (#1071)

#include <catch2/catch_test_macros.hpp>
#include <cstdint>

using namespace fl;

TEST_CASE("embedded single-player server does not share the standard game port", "[localserver][1054]") {
    const uint16_t gamePort = DiscoveryBeacon::Config{}.gamePort;
    REQUIRE(gamePort == 4778);

    // The embedded server must not sit on the game port: a dedicated fl-server on the same machine
    // may already hold it, and enet6 sets no SO_REUSEADDR.
    CHECK(LocalServer::kLocalServerPort != gamePort);

    // Nor on the server-query responder's port. fl-server auto-assigns that as game port + 1, so
    // "one above 4778" is already spoken for -- a local dedicated server would own 4779 and the
    // embedded server would collide with its query socket instead of its game socket.
    CHECK(LocalServer::kLocalServerPort != static_cast<uint16_t>(gamePort + 1));

    // And the embedded server's OWN derived query port must not land on anything claimed.
    CHECK(static_cast<uint16_t>(LocalServer::kLocalServerPort + 1) != gamePort);
    CHECK(static_cast<uint16_t>(LocalServer::kLocalServerPort + 1) != fl::kDiscoveryPort);
}

TEST_CASE("LAN discovery no longer aliases the game port (#1071)", "[localserver][1071]") {
    // The #1054 root cause: the beacon broadcast TO the game port, so the browser's listener had to
    // bind the game port, so it fought the embedded server for it. Discovery now has its own port and
    // the beacon advertises the connect port in the packet instead.
    DiscoveryBeacon::Config cfg;
    CHECK(cfg.discoveryPort == fl::kDiscoveryPort);
    CHECK(cfg.discoveryPort != cfg.gamePort);

    // Nothing derives the discovery port from another port in use, in either direction. This is the
    // property the old 4776 rationale spent fifteen lines establishing for the game port's
    // neighbours; the fix is that the question no longer needs asking.
    CHECK(fl::kDiscoveryPort != cfg.gamePort);
    CHECK(fl::kDiscoveryPort != static_cast<uint16_t>(cfg.gamePort + 1)); // the derived query port
    CHECK(fl::kDiscoveryPort != LocalServer::kLocalServerPort);
    CHECK(fl::kDiscoveryPort != static_cast<uint16_t>(LocalServer::kLocalServerPort + 1));
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

TEST_CASE("a browser listener and a dedicated server coexist on one host (#1071)", "[localserver][1071]") {
    // The user-visible #1054 regression, as a socket test: the client's server browser must be able
    // to listen for beacons while a dedicated fl-server holds the game port on the same machine, and
    // the embedded single-player server must still be able to bind its own port with both running.
    // Before the de-alias the first two contended for one port and the second one lost.
    SockGuard browser;
    uint16_t discovery = 0;
    REQUIRE(openWildcardReuse(browser, discovery)); // stands in for the DiscoveryListener

    // A dedicated server's ENet host on a DIFFERENT port: no SO_REUSEADDR, exactly like enet6.
    SockGuard dedicated;
    CHECK(bindPlainLoopback(dedicated, 0));

    // And the embedded single-player server, likewise plain. All three sockets live at once.
    SockGuard embedded;
    CHECK(bindPlainLoopback(embedded, 0));

    CHECK(browser.valid());
    CHECK(dedicated.valid());
    CHECK(embedded.valid());
}

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
