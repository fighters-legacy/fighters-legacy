// SPDX-License-Identifier: GPL-3.0-or-later
#include "IClock.h"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "ServerNotice.h"

using namespace fl;

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

TEST_CASE("ServerNotice: notice with timeout is visible before expiry") {
    fl::ManualClock fakeTime;
    ServerNotice n;
    n.setClock(fakeTime);
    n.setNotice("Welcome!", 0, 15);
    CHECK(n.buildElements().size() == 1);
}

TEST_CASE("ServerNotice: notice with timeout dismisses after expiry") {
    fl::ManualClock fakeTime;
    ServerNotice n;
    n.setClock(fakeTime);
    n.setNotice("Welcome!", 0, 15);
    fakeTime.advance(std::chrono::seconds(16));
    CHECK(n.buildElements().empty());
}

TEST_CASE("ServerNotice: buildElements remains empty on repeated calls after expiry") {
    fl::ManualClock fakeTime;
    ServerNotice n;
    n.setClock(fakeTime);
    n.setNotice("Welcome!", 0, 15);
    fakeTime.advance(std::chrono::seconds(16));
    CHECK(n.buildElements().empty());
    CHECK(n.buildElements().empty());
}

TEST_CASE("ServerNotice: zero timeout keeps banner permanently visible") {
    ServerNotice n;
    fl::ManualClock farFuture{std::chrono::steady_clock::now() + std::chrono::hours(24 * 365)};
    n.setClock(farFuture);
    n.setNotice("Persistent", 0, 0);
    CHECK(n.buildElements().size() == 1);
}

TEST_CASE("ServerNotice: persistent notice replaces an expired timed notice") {
    fl::ManualClock fakeTime;
    ServerNotice n;
    n.setClock(fakeTime);
    n.setNotice("MOTD", 0, 15);
    fakeTime.advance(std::chrono::seconds(16));
    REQUIRE(n.buildElements().empty());
    n.setNotice("Server shutting down in 5 minutes", 300);
    CHECK(n.buildElements().size() == 1);
}

TEST_CASE("ServerNotice: alpha is 1.0 before fade window") {
    fl::ManualClock fakeTime;
    ServerNotice n;
    n.setClock(fakeTime);
    n.setNotice("Welcome!", 0, 15);
    fakeTime.advance(std::chrono::seconds(12));
    auto elems = n.buildElements();
    REQUIRE(elems.size() == 1);
    CHECK(elems[0].a == Catch::Approx(1.f));
}

TEST_CASE("ServerNotice: alpha fades linearly in fade window") {
    fl::ManualClock fakeTime;
    ServerNotice n;
    n.setClock(fakeTime);
    n.setNotice("Welcome!", 0, 15);
    fakeTime.advance(std::chrono::milliseconds(14000));
    auto elems = n.buildElements();
    REQUIRE(elems.size() == 1);
    CHECK(elems[0].a == Catch::Approx(0.5f));
}

TEST_CASE("ServerNotice: alpha near zero at expiry boundary") {
    fl::ManualClock fakeTime;
    ServerNotice n;
    n.setClock(fakeTime);
    n.setNotice("Welcome!", 0, 15);
    fakeTime.advance(std::chrono::milliseconds(14900));
    auto elems = n.buildElements();
    REQUIRE(elems.size() == 1);
    CHECK(elems[0].a < 0.1f);
}

TEST_CASE("ServerNotice: persistent notice alpha stays 1.0") {
    fl::ManualClock fakeTime{std::chrono::steady_clock::now() + std::chrono::hours(24 * 365)};
    ServerNotice n;
    n.setClock(fakeTime);
    n.setNotice("Persistent", 0, 0);
    auto elems = n.buildElements();
    REQUIRE(elems.size() == 1);
    CHECK(elems[0].a == Catch::Approx(1.f));
}

TEST_CASE("ServerNotice: setNotice during fade resets alpha to 1.0") {
    fl::ManualClock fakeTime;
    ServerNotice n;
    n.setClock(fakeTime);
    n.setNotice("First", 0, 15);
    fakeTime.advance(std::chrono::milliseconds(14000));
    REQUIRE(n.buildElements().size() == 1);
    n.setNotice("Second", 0, 15);
    auto elems = n.buildElements();
    REQUIRE(elems.size() == 1);
    CHECK(elems[0].a == Catch::Approx(1.f));
}
