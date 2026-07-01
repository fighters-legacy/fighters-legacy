// SPDX-License-Identifier: GPL-3.0-or-later
#include "spatial/SpatialIndex.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static double dist2D(const double a[3], const double b[3]) {
    double dx = a[0] - b[0];
    double dz = a[2] - b[2];
    return std::sqrt(dx * dx + dz * dz);
}

// ---------------------------------------------------------------------------
// Basic correctness
// ---------------------------------------------------------------------------

TEST_CASE("SpatialIndex - empty index yields no callbacks") {
    fl::SpatialIndex idx;
    double center[3]{0.0, 0.0, 0.0};
    int count = 0;
    idx.queryRadius(center, 1e6, [&](uint32_t, const double*) { ++count; });
    CHECK(count == 0);
    CHECK(idx.entityCount() == 0u);
}

TEST_CASE("SpatialIndex - single entity within radius is found") {
    fl::SpatialIndex idx;
    double pos[3]{100.0, 50.0, 200.0};
    idx.insert(7u, pos);

    double center[3]{0.0, 0.0, 0.0};
    std::vector<uint32_t> found;
    idx.queryRadius(center, 1000.0, [&](uint32_t id, const double*) { found.push_back(id); });

    REQUIRE(found.size() == 1u);
    CHECK(found[0] == 7u);
}

TEST_CASE("SpatialIndex - entity two cells beyond radius is not visited") {
    // 1 km cell size. Entity at x=2001 is in cell floor(2001/1000)=2.
    // Query radius=1000 m from origin: x1 = floor(1000/1000) = 1. Cell 2 > x1=1 → not visited.
    fl::SpatialIndex idx{1000.0};
    double pos[3]{2001.0, 0.0, 0.0};
    idx.insert(1u, pos);

    double center[3]{0.0, 0.0, 0.0};
    int count = 0;
    idx.queryRadius(center, 1000.0, [&](uint32_t, const double*) { ++count; });
    CHECK(count == 0);
}

TEST_CASE("SpatialIndex - range query returns correct subset of multiple entities") {
    fl::SpatialIndex idx{1000.0}; // 1 km cells
    // Near: within the bounding square around origin ±500 m
    double p0[3]{0.0, 0.0, 0.0};
    double p1[3]{400.0, 0.0, 400.0};
    // Far: definitely outside
    double p2[3]{5000.0, 0.0, 5000.0};
    idx.insert(0u, p0);
    idx.insert(1u, p1);
    idx.insert(2u, p2);

    double center[3]{0.0, 0.0, 0.0};
    std::vector<uint32_t> found;
    idx.queryRadius(center, 500.0, [&](uint32_t id, const double*) { found.push_back(id); });

    // 0 and 1 are in cells overlapping the query square; 2 is not
    CHECK(found.size() == 2u);
    CHECK((found[0] == 0u || found[0] == 1u));
    CHECK((found[1] == 0u || found[1] == 1u));
}

TEST_CASE("SpatialIndex - negative world coordinates work correctly") {
    fl::SpatialIndex idx; // 10 km default
    double pos[3]{-5000.0, 200.0, -5000.0};
    idx.insert(3u, pos);

    double center[3]{-5000.0, 0.0, -5000.0};
    std::vector<uint32_t> found;
    idx.queryRadius(center, 1.0, [&](uint32_t id, const double*) { found.push_back(id); });

    REQUIRE(found.size() == 1u);
    CHECK(found[0] == 3u);
}

TEST_CASE("SpatialIndex - entity exactly on cell boundary lands in correct cell") {
    fl::SpatialIndex idx{1000.0};
    // pos.x == 1000.0 → cell floor(1000/1000) = 1
    double pos[3]{1000.0, 0.0, 0.0};
    idx.insert(5u, pos);

    // Query from (1000, 0, 0) with tiny radius: cell (1,0) is included
    double center[3]{1000.0, 0.0, 0.0};
    std::vector<uint32_t> found;
    idx.queryRadius(center, 0.1, [&](uint32_t id, const double*) { found.push_back(id); });

    REQUIRE(found.size() == 1u);
    CHECK(found[0] == 5u);
}

TEST_CASE("SpatialIndex - zero radius finds only entity at center cell") {
    fl::SpatialIndex idx{1000.0};
    // Both at (0,0,0) cell
    double p0[3]{0.0, 0.0, 0.0};
    double p1[3]{500.0, 0.0, 500.0};
    // Different cell
    double p2[3]{1500.0, 0.0, 0.0};
    idx.insert(0u, p0);
    idx.insert(1u, p1);
    idx.insert(2u, p2);

    double center[3]{0.0, 0.0, 0.0};
    std::vector<uint32_t> found;
    idx.queryRadius(center, 0.0, [&](uint32_t id, const double*) { found.push_back(id); });

    // Only cell (0,0) is queried; p0 and p1 are in it, p2 is not
    CHECK(found.size() == 2u);
}

TEST_CASE("SpatialIndex - large radius encompasses all entities") {
    fl::SpatialIndex idx;
    double positions[4][3]{{0, 0, 0}, {50000, 0, 0}, {0, 0, -80000}, {-30000, 0, 60000}};
    for (uint32_t i = 0; i < 4; ++i)
        idx.insert(i, positions[i]);

    double center[3]{0.0, 0.0, 0.0};
    std::vector<uint32_t> found;
    idx.queryRadius(center, 200000.0, [&](uint32_t id, const double*) { found.push_back(id); });

    CHECK(found.size() == 4u);
}

// ---------------------------------------------------------------------------
// State management
// ---------------------------------------------------------------------------

TEST_CASE("SpatialIndex - clear removes all entries") {
    fl::SpatialIndex idx;
    double pos[3]{0.0, 0.0, 0.0};
    idx.insert(1u, pos);
    idx.insert(2u, pos);
    CHECK(idx.entityCount() == 2u);

    idx.clear();
    CHECK(idx.entityCount() == 0u);

    double center[3]{0.0, 0.0, 0.0};
    int count = 0;
    idx.queryRadius(center, 1e6, [&](uint32_t, const double*) { ++count; });
    CHECK(count == 0);
}

TEST_CASE("SpatialIndex - entityCount tracks inserts and clear") {
    fl::SpatialIndex idx;
    CHECK(idx.entityCount() == 0u);
    double pos[3]{1.0, 2.0, 3.0};
    idx.insert(0u, pos);
    CHECK(idx.entityCount() == 1u);
    idx.insert(1u, pos);
    CHECK(idx.entityCount() == 2u);
    idx.clear();
    CHECK(idx.entityCount() == 0u);
}

// ---------------------------------------------------------------------------
// Callback accuracy
// ---------------------------------------------------------------------------

TEST_CASE("SpatialIndex - position triple forwarded accurately to callback") {
    fl::SpatialIndex idx;
    double pos[3]{12345.6, 789.0, -9876.5};
    idx.insert(42u, pos);

    double center[3]{12345.6, 0.0, -9876.5};
    bool called = false;
    idx.queryRadius(center, 1.0, [&](uint32_t id, const double* p) {
        called = true;
        CHECK(id == 42u);
        CHECK(p[0] == Catch::Approx(pos[0]));
        CHECK(p[1] == Catch::Approx(pos[1])); // Y altitude forwarded
        CHECK(p[2] == Catch::Approx(pos[2]));
    });
    CHECK(called);
}

// ---------------------------------------------------------------------------
// Cell size
// ---------------------------------------------------------------------------

TEST_CASE("SpatialIndex - custom cell size affects query range") {
    fl::SpatialIndex idx{500.0}; // 500 m cells
    // Entity at 600 m → cell 1; query radius=400 m → x1=floor(400/500)=0; cell 1 > 0 → not found
    double pos[3]{600.0, 0.0, 0.0};
    idx.insert(9u, pos);

    double center[3]{0.0, 0.0, 0.0};
    int count = 0;
    idx.queryRadius(center, 400.0, [&](uint32_t, const double*) { ++count; });
    CHECK(count == 0);

    // Widen to 600 m → x1=floor(600/500)=1 → cell 1 found
    idx.queryRadius(center, 600.0, [&](uint32_t, const double*) { ++count; });
    CHECK(count == 1);
}

// ---------------------------------------------------------------------------
// Planet-scale positions
// ---------------------------------------------------------------------------

TEST_CASE("SpatialIndex - planet scale positions work") {
    fl::SpatialIndex idx;
    double pos[3]{1e6, 0.0, 1e6};
    idx.insert(0u, pos);

    // Query at same position — found
    std::vector<uint32_t> found;
    idx.queryRadius(pos, 1.0, [&](uint32_t id, const double*) { found.push_back(id); });
    REQUIRE(found.size() == 1u);
    CHECK(found[0] == 0u);

    // Second entity far away — not found in first query
    double far[3]{1e6 + 20000.0, 0.0, 1e6};
    idx.insert(1u, far);
    found.clear();
    idx.queryRadius(pos, 1.0, [&](uint32_t id, const double*) { found.push_back(id); });
    CHECK(found.size() == 1u);
    CHECK(found[0] == 0u);
}

// ---------------------------------------------------------------------------
// Coverage gap tests
// ---------------------------------------------------------------------------

TEST_CASE("SpatialIndex - two entities in the same cell are both returned") {
    fl::SpatialIndex idx; // 10 km default; both positions land in cell (0,0)
    double p0[3]{0.0, 0.0, 0.0};
    double p1[3]{1.0, 0.0, 1.0};
    idx.insert(0u, p0);
    idx.insert(1u, p1);

    double center[3]{0.0, 0.0, 0.0};
    std::vector<uint32_t> found;
    idx.queryRadius(center, 100.0, [&](uint32_t id, const double*) { found.push_back(id); });

    REQUIRE(found.size() == 2u);
}

TEST_CASE("SpatialIndex - conservative: entity in queried cell but outside exact circle is visited") {
    // 1 km cells. Entity at (800, 0, 800): XZ dist from origin ≈ 1131 m > 500 m radius.
    // Cell = (0,0). Query square from origin ±500 m covers cells (-1..0, -1..0) → includes (0,0).
    // Conservative behaviour: entity IS returned despite being outside the exact circle.
    fl::SpatialIndex idx{1000.0};
    double pos[3]{800.0, 0.0, 800.0};
    idx.insert(11u, pos);

    double center[3]{0.0, 0.0, 0.0};
    std::vector<uint32_t> found;
    idx.queryRadius(center, 500.0, [&](uint32_t id, const double*) { found.push_back(id); });

    // The entity is outside the exact 500 m radius (dist ≈ 1131 m) but inside the queried cell
    double d = dist2D(pos, center);
    CHECK(d > 500.0);          // confirm it IS outside the exact radius
    CHECK(found.size() == 1u); // yet it IS returned (conservative contract)
    if (!found.empty())
        CHECK(found[0] == 11u);
}

TEST_CASE("SpatialIndex - rebuild cycle: clear then reinsert yields only the new set") {
    fl::SpatialIndex idx;
    double origin[3]{0.0, 0.0, 0.0};

    // Insert A and B
    idx.insert(0u, origin);
    idx.insert(1u, origin);
    CHECK(idx.entityCount() == 2u);

    // Simulate a tick boundary: clear + reinsert only C
    idx.clear();
    double posC[3]{100.0, 0.0, 100.0};
    idx.insert(2u, posC);
    CHECK(idx.entityCount() == 1u);

    // Query must find only C, not A or B
    std::vector<uint32_t> found;
    idx.queryRadius(origin, 1e6, [&](uint32_t id, const double*) { found.push_back(id); });
    REQUIRE(found.size() == 1u);
    CHECK(found[0] == 2u);
}

// ---------------------------------------------------------------------------
// Recycled-buffer clear + configurable cell size (issue #573)
// ---------------------------------------------------------------------------

TEST_CASE("SpatialIndex - recycled clear across many rebuild cycles stays correct") {
    fl::SpatialIndex idx(1000.0);
    // Many clear+rebuild cycles at varying positions exercise the spare-buffer recycling path.
    for (int cycle = 0; cycle < 50; ++cycle) {
        idx.clear();
        CHECK(idx.entityCount() == 0u);
        for (int i = 0; i < 20; ++i) {
            double p[3]{static_cast<double>(cycle * 2000 + i * 1500), 0.0, static_cast<double>(i * 1500)};
            idx.insert(static_cast<uint32_t>(i), p);
        }
        CHECK(idx.entityCount() == 20u);
    }
    // After the final rebuild, a broad query still sees exactly the last cycle's 20 entities — no
    // stale entries leaked in from recycled buffers.
    double center[3]{49.0 * 2000.0, 0.0, 0.0};
    std::vector<uint32_t> found;
    idx.queryRadius(center, 1e7, [&](uint32_t id, const double*) { found.push_back(id); });
    CHECK(found.size() == 20u);
}

TEST_CASE("SpatialIndex - setCellSize drops entries and rebuckets subsequent inserts") {
    fl::SpatialIndex idx(10000.0);
    CHECK(idx.cellSizeM() == Catch::Approx(10000.0));

    double a[3]{100.0, 0.0, 100.0};
    double b[3]{5000.0, 0.0, 5000.0}; // same 10 km cell as a
    idx.insert(0u, a);
    idx.insert(1u, b);
    CHECK(idx.entityCount() == 2u);

    // Shrinking the cell size clears the index and changes bucketing.
    idx.setCellSize(1000.0);
    CHECK(idx.cellSizeM() == Catch::Approx(1000.0));
    CHECK(idx.entityCount() == 0u);

    idx.insert(0u, a);
    idx.insert(1u, b); // now a different 1 km cell than a
    // A tight query around a must find only a (b is >4 km away).
    std::vector<uint32_t> found;
    idx.queryRadius(a, 500.0, [&](uint32_t id, const double*) { found.push_back(id); });
    REQUIRE(found.size() == 1u);
    CHECK(found[0] == 0u);
}

TEST_CASE("SpatialIndex - queryRadius correct at a non-default cell size") {
    fl::SpatialIndex idx(250.0); // fine cells
    // Ring of entities at 1 km radius around origin.
    for (int i = 0; i < 16; ++i) {
        const double ang = i * 6.2831853 / 16.0;
        double p[3]{1000.0 * std::cos(ang), 0.0, 1000.0 * std::sin(ang)};
        idx.insert(static_cast<uint32_t>(i), p);
    }
    double center[3]{0.0, 0.0, 0.0};
    // Exact distance filter on top of the conservative cell query must recover all 16.
    int within = 0;
    idx.queryRadius(center, 1100.0, [&](uint32_t, const double* p) {
        if (dist2D(center, p) <= 1100.0)
            ++within;
    });
    CHECK(within == 16);
}
