// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for the multiplayer kill feed (#647): a ring of recent "X destroyed Y" lines rendered
// top-right, with a deterministic (injected) clock driving the dwell + fade lifecycle.

#include "KillFeed.h"

#include "IClock.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace fl;

namespace {
std::string textOf(const HudElement& e) {
    return std::string(e.text);
}
} // namespace

TEST_CASE("KillFeed: newest line renders first, top-right and right-aligned", "[kill_feed]") {
    ManualClock clock;
    KillFeed feed;
    feed.setClock(clock);

    feed.push("Alpha destroyed Bravo");
    feed.push("Charlie destroyed Delta");

    auto els = feed.buildElements();
    REQUIRE(els.size() == 2u);
    // Newest push is at the top of the stack.
    CHECK(textOf(els[0]) == "Charlie destroyed Delta");
    CHECK(textOf(els[1]) == "Alpha destroyed Bravo");
    for (const auto& e : els) {
        CHECK(e.type == HudElement::Type::Text);
        CHECK(e.align == HudAlign::Right);
        CHECK(e.x == Catch::Approx(0.98f));
        CHECK(e.a == Catch::Approx(1.f)); // fresh: fully opaque
    }
    // Stacked downward.
    CHECK(els[1].y > els[0].y);
}

TEST_CASE("KillFeed: a line fades over its final seconds then expires", "[kill_feed]") {
    ManualClock clock;
    KillFeed feed;
    feed.setClock(clock);
    feed.push("Alpha destroyed Bravo");

    // Still opaque through the dwell window.
    clock.advance(std::chrono::milliseconds(static_cast<long long>(KillFeed::kDwellSecs * 1000.f) - 100));
    CHECK(feed.buildElements()[0].a == Catch::Approx(1.f));

    // Half a fade-second in: alpha ~0.75.
    clock.advance(std::chrono::milliseconds(static_cast<long long>((KillFeed::kFadeSecs * 0.25f) * 1000.f) + 100));
    {
        auto els = feed.buildElements();
        REQUIRE(els.size() == 1u);
        CHECK(els[0].a < 1.f);
        CHECK(els[0].a > 0.f);
    }

    // Past the full lifetime: gone.
    clock.advance(std::chrono::seconds(5));
    CHECK(feed.buildElements().empty());
}

TEST_CASE("KillFeed: ring caps at kMaxLines, oldest dropped", "[kill_feed]") {
    ManualClock clock;
    KillFeed feed;
    feed.setClock(clock);
    for (std::size_t i = 0; i < KillFeed::kMaxLines + 3; ++i)
        feed.push("line " + std::to_string(i));

    auto els = feed.buildElements();
    CHECK(els.size() == KillFeed::kMaxLines);
    // Newest first: the last pushed line is on top.
    CHECK(textOf(els[0]) == "line " + std::to_string(KillFeed::kMaxLines + 2));
    // The three oldest lines were overwritten.
    for (const auto& e : els)
        CHECK(textOf(e) != "line 0");
}

TEST_CASE("KillFeed: tint is carried through", "[kill_feed]") {
    ManualClock clock;
    KillFeed feed;
    feed.setClock(clock);
    feed.push("you destroyed Bandit", 0.4f, 1.0f, 0.4f);
    auto els = feed.buildElements();
    REQUIRE(els.size() == 1u);
    CHECK(els[0].g == Catch::Approx(1.0f));
    CHECK(els[0].r == Catch::Approx(0.4f));
}

TEST_CASE("KillFeed: clear drops all lines", "[kill_feed]") {
    ManualClock clock;
    KillFeed feed;
    feed.setClock(clock);
    feed.push("a destroyed b");
    feed.clear();
    CHECK(feed.buildElements().empty());
}
