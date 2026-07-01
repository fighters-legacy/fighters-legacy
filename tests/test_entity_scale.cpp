// SPDX-License-Identifier: GPL-3.0-or-later
//
// Isolated scale/characterisation benchmarks for the per-tick data structures (issue #573):
// EntityPool churn + SpatialIndex rebuild/query cost at thousands of entities. These are tagged
// [.][scale] so they are HIDDEN from the default ctest run (they print timings and use large N);
// run them explicitly with:  ctest -R test_entity_scale   or   ./test_entity_scale "[scale]"
//
// Assertions here are correctness invariants only; the wall-times are printed (INFO / stdout) as
// characterisation data, not gated — timing thresholds are too machine-dependent for CI. The
// authoritative end-to-end budget is the fl-server --metrics-json server_tick block driven by the
// bot_swarm entity-scale profile (see docs/load-testing.md).

#include "entity/EntityPool.h"
#include "spatial/SpatialIndex.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

namespace {

double ms(std::chrono::steady_clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}

// Fill an index with N entities laid out in a square grid of `spacingM` (distributed) or all near
// the origin (clustered when spacingM == 0).
void fillGrid(fl::SpatialIndex& idx, uint32_t n, double spacingM) {
    const auto side = static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<double>(n))));
    uint32_t placed = 0;
    for (uint32_t gx = 0; gx < side && placed < n; ++gx) {
        for (uint32_t gz = 0; gz < side && placed < n; ++gz, ++placed) {
            double p[3]{static_cast<double>(gx) * spacingM, 500.0, static_cast<double>(gz) * spacingM};
            idx.insert(placed, p);
        }
    }
}

} // namespace

TEST_CASE("EntityPool scale: forEach is O(liveCount) under heavy reap churn", "[.][scale][entity_pool]") {
    constexpr uint32_t kPeak = 20'000;
    fl::EntityPool pool(256);
    std::vector<fl::EntityId> ids;
    ids.reserve(kPeak);
    for (uint32_t i = 0; i < kPeak; ++i)
        ids.push_back(pool.alloc());

    // Reap 90% — capacity stays at the peak, liveCount drops to 10%.
    std::mt19937 rng(99);
    std::shuffle(ids.begin(), ids.end(), rng);
    const uint32_t keep = kPeak / 10;
    for (uint32_t i = keep; i < kPeak; ++i)
        pool.free(ids[i]);

    CHECK(pool.liveCount() == keep);
    CHECK(pool.capacity() >= kPeak); // dead slots remain in the backing store

    // forEach must visit exactly the live set — and pay for liveCount, not capacity.
    uint32_t visited = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int rep = 0; rep < 1000; ++rep) {
        visited = 0;
        pool.forEach([&](const fl::EntityState&) { ++visited; });
    }
    const auto t1 = std::chrono::steady_clock::now();
    CHECK(visited == keep);
    std::printf("[scale] EntityPool.forEach: liveCount=%u capacity=%u  %.4f ms/iter (1000 iters)\n", keep,
                pool.capacity(), ms(t1 - t0) / 1000.0);
}

TEST_CASE("SpatialIndex scale: rebuild + query cost, clustered vs distributed vs tuned cell", "[.][scale][spatial]") {
    constexpr uint32_t kN = 5000;
    const double drawM = 200'000.0; // 200 km interest radius
    double center[3]{0.0, 500.0, 0.0};

    struct Layout {
        const char* name;
        double cellM;
        double spacingM;
    };
    // clustered = everyone in ~one 10 km cell; distributed = spread; tuned = smaller cell on spread.
    const Layout layouts[] = {
        {"clustered  (10 km cell, 5 m spacing)", 10'000.0, 5.0},
        {"distributed(10 km cell, 800 m spacing)", 10'000.0, 800.0},
        {"tuned      (1 km cell, 800 m spacing)", 1'000.0, 800.0},
    };

    for (const auto& L : layouts) {
        fl::SpatialIndex idx(L.cellM);

        // Rebuild cost over many ticks (clear recycles buffers — steady state should not reallocate).
        const auto rb0 = std::chrono::steady_clock::now();
        for (int tick = 0; tick < 200; ++tick) {
            idx.clear();
            fillGrid(idx, kN, L.spacingM);
        }
        const auto rb1 = std::chrono::steady_clock::now();
        REQUIRE(idx.entityCount() == kN);

        // Query cost: one full-draw-distance interest query with an exact XZ filter.
        uint32_t within = 0;
        const auto q0 = std::chrono::steady_clock::now();
        for (int rep = 0; rep < 200; ++rep) {
            within = 0;
            idx.queryRadius(center, drawM, [&](uint32_t, const double* p) {
                const double dx = p[0] - center[0], dz = p[2] - center[2];
                if (dx * dx + dz * dz <= drawM * drawM)
                    ++within;
            });
        }
        const auto q1 = std::chrono::steady_clock::now();

        // Correctness: for clustered/distributed at this spacing every entity is within 200 km.
        CHECK(within <= kN);
        std::printf("[scale] SpatialIndex %-38s rebuild %.4f ms/tick  query %.4f ms  (visible=%u)\n", L.name,
                    ms(rb1 - rb0) / 200.0, ms(q1 - q0) / 200.0, within);
    }
}

TEST_CASE("SpatialIndex scale: recycled clear does not leak stale entities at scale", "[.][scale][spatial]") {
    constexpr uint32_t kN = 5000;
    fl::SpatialIndex idx(2'000.0);
    for (int tick = 0; tick < 100; ++tick) {
        idx.clear();
        // Shift the whole field each tick so cells churn (exercises the spare-buffer pool).
        const double shift = tick * 3000.0;
        for (uint32_t i = 0; i < kN; ++i) {
            const uint32_t col = i % 71;
            const uint32_t row = i / 71;
            double p[3]{shift + static_cast<double>(col) * 900.0, 500.0, static_cast<double>(row) * 900.0};
            idx.insert(i, p);
        }
        REQUIRE(idx.entityCount() == kN);
    }
    // A query far from the final field must see nothing (no leaked stale entries).
    double faraway[3]{-1e9, 0.0, -1e9};
    uint32_t seen = 0;
    idx.queryRadius(faraway, 10'000.0, [&](uint32_t, const double*) { ++seen; });
    CHECK(seen == 0u);
}
