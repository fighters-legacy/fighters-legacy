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
