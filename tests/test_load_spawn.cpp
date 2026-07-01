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
