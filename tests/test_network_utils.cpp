// SPDX-License-Identifier: GPL-3.0-or-later
#include "net/NetworkUtils.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("normalizeIp passes plain IPv4 through unchanged", "[network_utils]") {
    REQUIRE(fl::normalizeIp("1.2.3.4") == "1.2.3.4");
    REQUIRE(fl::normalizeIp("127.0.0.1") == "127.0.0.1");
    REQUIRE(fl::normalizeIp("192.168.1.100") == "192.168.1.100");
}

TEST_CASE("normalizeIp strips IPv6-mapped IPv4 prefix", "[network_utils]") {
    REQUIRE(fl::normalizeIp("::ffff:1.2.3.4") == "1.2.3.4");
    REQUIRE(fl::normalizeIp("::ffff:10.0.0.1") == "10.0.0.1");
    REQUIRE(fl::normalizeIp("::ffff:192.168.0.1") == "192.168.0.1");
}

TEST_CASE("normalizeIp strips brackets from IPv6", "[network_utils]") {
    REQUIRE(fl::normalizeIp("[2001:db8::1]") == "2001:db8::1");
    REQUIRE(fl::normalizeIp("[::1]") == "::1");
}

TEST_CASE("normalizeIp strips brackets and ::ffff: prefix combined", "[network_utils]") {
    REQUIRE(fl::normalizeIp("[::ffff:10.0.0.1]") == "10.0.0.1");
}

TEST_CASE("normalizeIp passes plain IPv6 without brackets through", "[network_utils]") {
    REQUIRE(fl::normalizeIp("::1") == "::1");
    REQUIRE(fl::normalizeIp("2001:db8::1") == "2001:db8::1");
}

TEST_CASE("normalizeIp returns empty string for empty input", "[network_utils]") {
    REQUIRE(fl::normalizeIp("") == "");
}

// ---------------------------------------------------------------------------
// extractIp (#1243) — promoted out of PeerAdmission so the admission path and the admin commands
// match peers by IP the SAME way. Both had a private copy; ban/kick/unban/lockout all key on this,
// so two implementations were two chances to disagree about which peer an operator just banned.
// ---------------------------------------------------------------------------

TEST_CASE("extractIp takes the address off an ip:port pair", "[network_utils]") {
    REQUIRE(fl::extractIp("1.2.3.4:5678") == "1.2.3.4");
    REQUIRE(fl::extractIp("127.0.0.1:4793") == "127.0.0.1");
}

TEST_CASE("extractIp handles a bracketed IPv6 address with a port", "[network_utils]") {
    REQUIRE(fl::extractIp("[2001:db8::1]:4793") == "2001:db8::1");
    REQUIRE(fl::extractIp("[::1]:4793") == "::1");
}

TEST_CASE("extractIp normalizes an IPv6-mapped IPv4 peer", "[network_utils]") {
    // What an enet6 socket actually reports for an IPv4 client — a ban entered as "1.2.3.4" must
    // match it, which is only true because extractIp normalizes rather than just splitting.
    REQUIRE(fl::extractIp("[::ffff:1.2.3.4]:4793") == "1.2.3.4");
    REQUIRE(fl::extractIp("::ffff:1.2.3.4:4793") == "1.2.3.4");
}

TEST_CASE("extractIp accepts an address with no port", "[network_utils]") {
    REQUIRE(fl::extractIp("1.2.3.4") == "1.2.3.4");
    REQUIRE(fl::extractIp("[2001:db8::1]") == "2001:db8::1");
}

TEST_CASE("extractIp truncates an unbracketed port-less IPv6 literal, deliberately", "[network_utils]") {
    // Documented limit, pinned so it is found rather than discovered: the string is genuinely
    // ambiguous (is the last group a port?). getPeerAddress() always supplies "ip:port", so no
    // production caller reaches this; a future caller that would should bracket the address.
    REQUIRE(fl::extractIp("2001:db8::1") == "2001:db8:");
}

TEST_CASE("extractIp treats null and empty addresses as no IP", "[network_utils]") {
    REQUIRE(fl::extractIp(static_cast<const char*>(nullptr)) == "");
    REQUIRE(fl::extractIp("") == "");
}
