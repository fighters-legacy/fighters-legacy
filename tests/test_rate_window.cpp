// SPDX-License-Identifier: GPL-3.0-or-later
//
// engine/net/RateWindow.h (#1264).
//
// Eight per-peer channels hand-rolled the same 1-second window. What made them worth unifying is
// also what makes them risky to unify: they do NOT share what happens once the budget is spent, and
// two of the eight test the predicate in the opposite direction (heartbeat and seat requests count
// the packet either way and suppress only the REPLY). A shared body that quietly changed one site's
// direction, or dropped a rollover, would be a silent protocol change rather than a build break.
//
// So this pins the contract itself -- the boundary (limit is inclusive), the rollover, and that the
// once-per-window reply flag is cleared BY the rollover -- rather than pinning any one caller.

#include "net/RateWindow.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>

using namespace std::chrono_literals;
using fl::RateWindow;

namespace {
constexpr std::chrono::steady_clock::time_point kT0{};
} // namespace

TEST_CASE("RateWindow admits exactly limit requests per window", "[net][ratelimit]") {
    RateWindow w;
    // The limit is inclusive: a limit of 3 admits the 3rd request and refuses the 4th. Every caller
    // spells this as `count > limit` today, so an off-by-one here would move every channel's budget.
    CHECK(w.allow(kT0, 3));
    CHECK(w.allow(kT0, 3));
    CHECK(w.allow(kT0, 3));
    CHECK_FALSE(w.allow(kT0, 3));
    CHECK_FALSE(w.allow(kT0, 3));
    CHECK(w.count == 5); // still ACCOUNTED past the limit -- heartbeat liveness depends on this
}

TEST_CASE("RateWindow of zero refuses everything", "[net][ratelimit]") {
    RateWindow w;
    CHECK_FALSE(w.allow(kT0, 0));
    CHECK_FALSE(w.allow(kT0 + 5s, 0));
}

TEST_CASE("RateWindow rolls over at one second, not before", "[net][ratelimit]") {
    RateWindow w;
    CHECK(w.allow(kT0, 1));
    CHECK_FALSE(w.allow(kT0 + 999ms, 1)); // same window
    CHECK(w.allow(kT0 + 1000ms, 1));      // exactly 1 s opens a new one
    CHECK_FALSE(w.allow(kT0 + 1500ms, 1));
    CHECK(w.allow(kT0 + 2000ms, 1));
}

TEST_CASE("RateWindow counts the request that opens a window in that window", "[net][ratelimit]") {
    RateWindow w;
    CHECK(w.allow(kT0, 2));
    CHECK(w.allow(kT0, 2));
    CHECK_FALSE(w.allow(kT0, 2));
    // The rollover resets to zero and THEN counts, so the first request of the new window is 1 of 2
    // -- not 0 of 2, which would hand every channel one free packet per second.
    CHECK(w.allow(kT0 + 1s, 2));
    CHECK(w.count == 1);
    CHECK(w.allow(kT0 + 1s, 2));
    CHECK_FALSE(w.allow(kT0 + 1s, 2));
}

TEST_CASE("RateWindow warned is caller-owned within a window and cleared by the rollover", "[net][ratelimit]") {
    RateWindow w;
    // The chat and wingman shape: warn once, then stay silent for the rest of the window.
    CHECK(w.allow(kT0, 1));
    CHECK_FALSE(w.warned);
    CHECK_FALSE(w.allow(kT0, 1));
    w.warned = true; // the caller sends its one notice here
    CHECK_FALSE(w.allow(kT0 + 500ms, 1));
    CHECK(w.warned); // still suppressed -- one reply per window, not one per packet
    CHECK(w.allow(kT0 + 1s, 1));
    CHECK_FALSE(w.warned); // a new window earns a new notice
}

TEST_CASE("RateWindow starts fresh at its default-constructed epoch", "[net][ratelimit]") {
    // Every peer's windows are default-constructed on connect, and the first packet must be admitted
    // rather than judged against a zero time_point from an unrelated epoch.
    RateWindow w;
    CHECK(w.count == 0);
    CHECK_FALSE(w.warned);
    CHECK(w.allow(std::chrono::steady_clock::now(), 1));
}
