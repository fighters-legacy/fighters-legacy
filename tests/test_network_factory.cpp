// SPDX-License-Identifier: GPL-3.0-or-later
#include "NetworkFactory.h"

#include <catch2/catch_test_macros.hpp>
#include <cstring>

using namespace fl;

TEST_CASE("parseTransportKind maps strings", "[network_factory]") {
    CHECK(parseTransportKind("enet", TransportKind::Gns) == TransportKind::Enet);
    CHECK(parseTransportKind("enet6", TransportKind::Gns) == TransportKind::Enet);
    CHECK(parseTransportKind("gns", TransportKind::Enet) == TransportKind::Gns);
    CHECK(parseTransportKind("GNS", TransportKind::Enet) == TransportKind::Gns); // case-insensitive
    CHECK(parseTransportKind("Enet", TransportKind::Gns) == TransportKind::Enet);
}

TEST_CASE("parseTransportKind returns fallback for unknown", "[network_factory]") {
    CHECK(parseTransportKind("", TransportKind::Enet) == TransportKind::Enet);
    CHECK(parseTransportKind("udp", TransportKind::Gns) == TransportKind::Gns);
    CHECK(parseTransportKind("quic", TransportKind::Enet) == TransportKind::Enet);
}

TEST_CASE("createNetwork(Enet) returns a working backend", "[network_factory]") {
    auto net = createNetwork(TransportKind::Enet);
    REQUIRE(net != nullptr);
    REQUIRE(net->init());
    net->shutdown();
}

TEST_CASE("createNetwork(Gns) returns a usable backend", "[network_factory]") {
    // GNS in a GNS-enabled build; the enet6 fallback in an enet6-only build. Either way non-null.
    auto net = createNetwork(TransportKind::Gns);
    REQUIRE(net != nullptr);
    REQUIRE(net->init());
    net->shutdown();
}

TEST_CASE("networkBackendVersion returns a string for both kinds", "[network_factory]") {
    CHECK(networkBackendVersion(TransportKind::Enet) != nullptr);
    CHECK(std::strlen(networkBackendVersion(TransportKind::Enet)) > 0);
    CHECK(networkBackendVersion(TransportKind::Gns) != nullptr);
    CHECK(std::strlen(networkBackendVersion(TransportKind::Gns)) > 0);
}
