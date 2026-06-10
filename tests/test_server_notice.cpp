// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "ServerNotice.h"

TEST_CASE("ServerNotice: inactive produces no elements") {
    ServerNotice n;
    CHECK(n.buildElements().empty());
}

TEST_CASE("ServerNotice: notice text appears after setNotice") {
    ServerNotice n;
    n.setNotice("Server shutting down", 120);
    auto elems = n.buildElements();
    REQUIRE(elems.size() == 1);
    CHECK(elems[0].text.find("Server shutting down") != std::string_view::npos);
}

TEST_CASE("ServerNotice: element type is Text") {
    ServerNotice n;
    n.setNotice("hello", 0);
    auto elems = n.buildElements();
    REQUIRE(elems.size() == 1);
    CHECK(elems[0].type == HudElement::Type::Text);
}

TEST_CASE("ServerNotice: element is horizontally centered") {
    ServerNotice n;
    n.setNotice("centered", 60);
    auto elems = n.buildElements();
    REQUIRE(elems.size() == 1);
    CHECK(elems[0].x == Catch::Approx(0.5f));
}

TEST_CASE("ServerNotice: subsequent setNotice replaces previous") {
    ServerNotice n;
    n.setNotice("first", 30);
    n.setNotice("second notice", 10);
    auto elems = n.buildElements();
    REQUIRE(elems.size() == 1);
    CHECK(elems[0].text.find("second notice") != std::string_view::npos);
    CHECK(elems[0].text.find("first") == std::string_view::npos);
}
