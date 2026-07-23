// SPDX-License-Identifier: GPL-3.0-or-later
#include "GmMapView.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace fl;

TEST_CASE("GmMapView: world centre maps to screen centre", "[gm_map_view]") {
    GmMapView v;
    v.centerX = 1000.0;
    v.centerZ = -2000.0;
    v.spanMetresY = 10000.0;
    v.aspect = 16.f / 9.f;

    const glm::vec2 c = v.worldToMap(1000.0, -2000.0);
    CHECK(c.x == Catch::Approx(0.5f));
    CHECK(c.y == Catch::Approx(0.5f));
}

TEST_CASE("GmMapView: worldToMap and mapToWorld round-trip", "[gm_map_view]") {
    GmMapView v;
    v.centerX = 500.0;
    v.centerZ = 700.0;
    v.spanMetresY = 20000.0;
    v.aspect = 4.f / 3.f;

    for (const auto& p : std::vector<std::pair<double, double>>{
             {500.0, 700.0}, {2500.0, 700.0}, {500.0, -1300.0}, {12345.0, -6789.0}}) {
        const glm::vec2 m = v.worldToMap(p.first, p.second);
        const glm::dvec2 w = v.mapToWorld(m.x, m.y);
        CHECK(w.x == Catch::Approx(p.first).margin(1.0));
        CHECK(w.y == Catch::Approx(p.second).margin(1.0));
    }
}

TEST_CASE("GmMapView: X axis is aspect-scaled so metres are isotropic", "[gm_map_view]") {
    GmMapView v;
    v.centerX = 0.0;
    v.centerZ = 0.0;
    v.spanMetresY = 1000.0; // 1000 m across the height
    v.aspect = 2.0f;        // width is twice the height

    // 500 m up (dz) reaches the top edge (ny=0); 500 m in dz is half the height.
    const glm::vec2 up = v.worldToMap(0.0, -500.0);
    CHECK(up.y == Catch::Approx(0.0f));
    // The same 500 m in X spans only a quarter of the normalized width (aspect 2 -> full width = 2000 m).
    const glm::vec2 right = v.worldToMap(500.0, 0.0);
    CHECK(right.x == Catch::Approx(0.75f));
}

TEST_CASE("GmMapView: zoom clamps to the sane span range", "[gm_map_view]") {
    GmMapView v;
    v.spanMetresY = 10000.0;
    v.zoom(0.0001); // very far in
    CHECK(v.spanMetresY == Catch::Approx(GmMapView::kMinSpanMetres));
    v.zoom(1e9); // very far out
    CHECK(v.spanMetresY == Catch::Approx(GmMapView::kMaxSpanMetres));
}

TEST_CASE("GmMapView: pan moves the world centre", "[gm_map_view]") {
    GmMapView v;
    v.centerX = 100.0;
    v.centerZ = 200.0;
    v.pan(50.0, -25.0);
    CHECK(v.centerX == Catch::Approx(150.0));
    CHECK(v.centerZ == Catch::Approx(175.0));
}

static GmEntityRecord rec(uint32_t idx, float x, float z) {
    GmEntityRecord r;
    r.entityIdx = idx;
    r.pos[0] = x;
    r.pos[2] = z;
    return r;
}

TEST_CASE("GmMapView: pick returns the nearest entity within the radius", "[gm_map_view]") {
    GmMapView v;
    v.centerX = 0.0;
    v.centerZ = 0.0;
    v.spanMetresY = 2000.0; // 2 km across height
    v.aspect = 1.0f;        // square for simple math

    std::vector<GmEntityRecord> recs = {
        rec(10, 0.f, 0.f),      // at centre
        rec(11, 500.f, 500.f),  // off-centre
        rec(12, -800.f, 100.f), // elsewhere
    };

    // Click at screen centre -> nearest is entity 10.
    const int hit = v.pick(recs, glm::vec2{0.5f, 0.5f}, 0.05f);
    REQUIRE(hit >= 0);
    CHECK(recs[static_cast<std::size_t>(hit)].entityIdx == 10u);

    // Click near entity 11's screen position (500 m right/down of centre => +0.25 each on a 2 km/1:1).
    const glm::vec2 p11 = v.worldToMap(500.f, 500.f);
    const int hit11 = v.pick(recs, p11, 0.05f);
    REQUIRE(hit11 >= 0);
    CHECK(recs[static_cast<std::size_t>(hit11)].entityIdx == 11u);

    // Click far from everything -> no hit.
    const int miss = v.pick(recs, glm::vec2{0.99f, 0.01f}, 0.02f);
    CHECK(miss == -1);
}
