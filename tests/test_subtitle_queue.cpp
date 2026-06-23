// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/SubtitleQueue.h"
#include <catch2/catch_test_macros.hpp>

using namespace fl;

TEST_CASE("SubtitleQueue push and current", "[audio][subtitle]") {
    SubtitleQueue q;
    REQUIRE(q.current().empty());
    q.push("Hello", 5.0f);
    REQUIRE(q.current() == "Hello");
}

TEST_CASE("SubtitleQueue update expires entries", "[audio][subtitle]") {
    SubtitleQueue q;
    q.push("Expire me", 1.0f);
    REQUIRE(!q.current().empty());
    q.update(0.5f);
    REQUIRE(!q.current().empty()); // still alive
    q.update(0.6f);
    REQUIRE(q.current().empty()); // expired
}

TEST_CASE("SubtitleQueue disabled push is no-op", "[audio][subtitle]") {
    SubtitleQueue q;
    REQUIRE(q.enabled()); // starts enabled
    q.setEnabled(false);
    REQUIRE(!q.enabled());
    q.push("Should not appear", 5.0f);
    REQUIRE(q.current().empty());
    REQUIRE(q.records().empty());
    q.setEnabled(true);
    REQUIRE(q.enabled());
}

TEST_CASE("SubtitleQueue oldest evicted at kMaxActive cap", "[audio][subtitle]") {
    SubtitleQueue q;
    q.push("A", 10.0f);
    q.push("B", 10.0f);
    q.push("C", 10.0f);
    REQUIRE(q.records().size() == 3);
    // Pushing a fourth evicts the oldest (A).
    q.push("D", 10.0f);
    REQUIRE(q.records().size() == 3);
    REQUIRE(q.records().front().text == "B");
    REQUIRE(q.records().back().text == "D");
}

TEST_CASE("SubtitleQueue multiple records all advance", "[audio][subtitle]") {
    SubtitleQueue q;
    q.push("Short", 0.5f);
    q.push("Long", 5.0f);
    REQUIRE(q.records().size() == 2);
    q.update(0.6f);
    // "Short" expired; "Long" still active.
    REQUIRE(q.records().size() == 1);
    REQUIRE(q.current() == "Long");
}
