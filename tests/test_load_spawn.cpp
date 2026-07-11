// SPDX-License-Identifier: GPL-3.0-or-later
#include "TestSpawn.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <set>

using fl::testSpawnPositions;

namespace {
double radiusM(const std::array<double, 3>& p) {
    return std::sqrt(p[0] * p[0] + p[2] * p[2]);
}
} // namespace

TEST_CASE("testSpawnPositions: count is exact", "[load_spawn]") {
    CHECK(testSpawnPositions(0u, 50'000.0, 500.0, 100.0).empty());
    CHECK(testSpawnPositions(1u, 50'000.0, 500.0, 100.0).size() == 1u);
    CHECK(testSpawnPositions(5000u, 50'000.0, 500.0, 100.0).size() == 5000u);
}

TEST_CASE("testSpawnPositions: all points within the spread radius", "[load_spawn]") {
    const double spread = 50'000.0;
    for (const auto& p : testSpawnPositions(5000u, spread, 500.0, 0.0))
        CHECK(radiusM(p) <= spread + 1e-6);
}

TEST_CASE("testSpawnPositions: altitude is baseElev + agl for every entity", "[load_spawn]") {
    const auto pts = testSpawnPositions(1000u, 50'000.0, 500.0, 120.0);
    for (const auto& p : pts)
        CHECK(p[1] == Catch::Approx(620.0));
}

TEST_CASE("testSpawnPositions: deterministic across calls", "[load_spawn]") {
    const auto a = testSpawnPositions(2000u, 40'000.0, 500.0, 50.0);
    const auto b = testSpawnPositions(2000u, 40'000.0, 500.0, 50.0);
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i][0] == b[i][0]);
        CHECK(a[i][1] == b[i][1]);
        CHECK(a[i][2] == b[i][2]);
    }
}

TEST_CASE("testSpawnPositions: distributes across many spatial cells (not clustered)", "[load_spawn]") {
    // With a 10 km cell over a 50 km spread, the fill should occupy many distinct cells rather than
    // collapsing into one — the whole point of the phyllotaxis spread for #573.
    const auto pts = testSpawnPositions(4000u, 50'000.0, 500.0, 0.0);
    std::set<std::pair<int64_t, int64_t>> cells;
    for (const auto& p : pts) {
        cells.insert(
            {static_cast<int64_t>(std::floor(p[0] / 10'000.0)), static_cast<int64_t>(std::floor(p[2] / 10'000.0))});
    }
    CHECK(cells.size() > 20u);
}

// ---------------------------------------------------------------------------
// Controller mix (#580)
// ---------------------------------------------------------------------------

TEST_CASE("parseTestSpawnMix parses valid specs and rejects malformed ones", "[load_spawn][mix]") {
    std::vector<fl::TestSpawnMixEntry> mix;
    std::string err;

    REQUIRE(fl::parseTestSpawnMix("loiter:60,pursuit:25,patrol:15", mix, err));
    REQUIRE(mix.size() == 3);
    CHECK(mix[0].behavior == "loiter");
    CHECK(mix[0].weight == 60);
    CHECK(mix[2].behavior == "patrol");
    CHECK(mix[2].weight == 15);

    CHECK(fl::parseTestSpawnMix("pursuit:100", mix, err));

    CHECK_FALSE(fl::parseTestSpawnMix("loiter", mix, err));     // missing :weight
    CHECK_FALSE(fl::parseTestSpawnMix("evade:50", mix, err));   // unknown behavior (not offered)
    CHECK_FALSE(fl::parseTestSpawnMix("loiter:0", mix, err));   // non-positive weight
    CHECK_FALSE(fl::parseTestSpawnMix("loiter:-1", mix, err));  // negative weight
    CHECK_FALSE(fl::parseTestSpawnMix("loiter:50,", mix, err)); // trailing empty entry
    CHECK_FALSE(fl::parseTestSpawnMix("loiter:1.5", mix, err)); // non-integer weight
    CHECK_FALSE(fl::parseTestSpawnMix("", mix, err));           // empty spec
}

TEST_CASE("assignTestSpawnBehavior distributes deterministically by weight", "[load_spawn][mix]") {
    std::vector<fl::TestSpawnMixEntry> mix;
    std::string err;
    REQUIRE(fl::parseTestSpawnMix("loiter:60,pursuit:25,patrol:15", mix, err));

    const uint32_t n = 1000;
    int loiter = 0, pursuit = 0, patrol = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const std::string& b = fl::assignTestSpawnBehavior(mix, i, n);
        if (b == "loiter")
            ++loiter;
        else if (b == "pursuit")
            ++pursuit;
        else
            ++patrol;
    }
    // Proportional assignment: counts match the weight fractions exactly at n=1000.
    CHECK(loiter == 600);
    CHECK(pursuit == 250);
    CHECK(patrol == 150);
    // Deterministic on repeat.
    CHECK(fl::assignTestSpawnBehavior(mix, 42u, n) == fl::assignTestSpawnBehavior(mix, 42u, n));
}

// ---------------------------------------------------------------------------
// Projectile churn (#580)
// ---------------------------------------------------------------------------

TEST_CASE("churnSpawnCount paces spawns with a fractional accumulator", "[load_spawn][churn]") {
    // 120/s at 60 Hz = exactly 2 per tick.
    double accum = 0.0;
    for (int i = 0; i < 10; ++i)
        CHECK(fl::churnSpawnCount(accum, 120.0, 1.0 / 60.0) == 2u);

    // 30/s at 60 Hz = 0.5 per tick: alternates 0/1 and totals 30 over 60 ticks.
    accum = 0.0;
    uint32_t total = 0;
    for (int i = 0; i < 60; ++i)
        total += fl::churnSpawnCount(accum, 30.0, 1.0 / 60.0);
    CHECK(total == 30u);

    // Disabled / degenerate inputs spawn nothing and leave the accumulator alone.
    accum = 0.25;
    CHECK(fl::churnSpawnCount(accum, 0.0, 1.0 / 60.0) == 0u);
    CHECK(fl::churnSpawnCount(accum, -5.0, 1.0 / 60.0) == 0u);
    CHECK(accum == Catch::Approx(0.25));
}

TEST_CASE("testProjectilePosition walks the spread deterministically", "[load_spawn][churn]") {
    const double spread = 50'000.0;
    // Deterministic per counter value.
    const auto a = fl::testProjectilePosition(7u, spread, 600.0);
    const auto b = fl::testProjectilePosition(7u, spread, 600.0);
    CHECK(a[0] == b[0]);
    CHECK(a[2] == b[2]);
    CHECK(a[1] == Catch::Approx(600.0));

    // All positions stay within the spread radius, and consecutive spawns land in many distinct
    // 10 km cells (fresh SpatialIndex traffic, not one hot cell).
    std::set<std::pair<int64_t, int64_t>> cells;
    for (uint64_t k = 0; k < 2000; ++k) {
        const auto p = fl::testProjectilePosition(k, spread, 600.0);
        CHECK(radiusM(p) <= spread + 1e-6);
        cells.insert(
            {static_cast<int64_t>(std::floor(p[0] / 10'000.0)), static_cast<int64_t>(std::floor(p[2] / 10'000.0))});
    }
    CHECK(cells.size() > 20u);
}
