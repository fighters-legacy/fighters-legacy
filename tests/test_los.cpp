// SPDX-License-Identifier: GPL-3.0-or-later
//
// Terrain line-of-sight tests (#687): the shared engine/spatial segment query over analytic
// heightfields — ridges, valleys, bowls, grazing tangents, step-size independence, and the
// Unknown propagation that keeps unloaded terrain from ever false-blocking a padlock lock.

#include "spatial/LineOfSight.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace fl;

namespace {

// The tests work near the world origin (the north pole, lat=90). There, a point at world (x, h, z)
// with |x|,|z| << R has radial altitude ~= h, so "height above the datum" reads straight off world
// Y for gentle terrain — which keeps the analytic heightfields easy to reason about.
constexpr double kR = 6'371'000.0; // Earth radius; engine-spatial is glm-/Geodetic-free by design

// Flat ground at a fixed datum-relative elevation.
struct FlatGround {
    double elevation;
    double operator()(double, double, double) const {
        return elevation;
    }
};

// A single Gaussian ridge centred on the world X axis (peak at x=cx), returning radial elevation.
struct GaussianRidge {
    double cx, peak, sigma, base;
    double operator()(double x, double /*y*/, double /*z*/) const {
        const double d = x - cx;
        return base + peak * std::exp(-(d * d) / (2.0 * sigma * sigma));
    }
};

} // namespace

TEST_CASE("terrainLos: clear line over flat ground (#687)", "[los]") {
    const double a[3] = {-1000.0, 500.0, 0.0};
    const double b[3] = {1000.0, 500.0, 0.0};
    CHECK(terrainLos(a, b, FlatGround{0.0}, {}, kR) == LosResult::Clear);
}

TEST_CASE("terrainLos: a ridge between the endpoints blocks the segment (#687)", "[los]") {
    // Both endpoints at 200 m, a 400 m ridge at the midpoint (x=0) masks the line.
    const double a[3] = {-2000.0, 200.0, 0.0};
    const double b[3] = {2000.0, 200.0, 0.0};
    CHECK(terrainLos(a, b, GaussianRidge{0.0, 400.0, 300.0, 0.0}, {}, kR) == LosResult::Blocked);
}

TEST_CASE("terrainLos: a line that clears the ridge crest stays Clear (#687)", "[los]") {
    // Same ridge (peak 400 m at x=0), but the segment flies at 600 m — well above the crest.
    const double a[3] = {-2000.0, 600.0, 0.0};
    const double b[3] = {2000.0, 600.0, 0.0};
    CHECK(terrainLos(a, b, GaussianRidge{0.0, 400.0, 300.0, 0.0}, {}, kR) == LosResult::Clear);
}

TEST_CASE("terrainLos: a line dipping into a valley floor still sees across it (#687)", "[los]") {
    // Endpoints on hilltops at 500 m, valley floor at 0 m between them: the straight line stays
    // above the floor the whole way, so it is Clear (a valley never blocks a line over it).
    const double a[3] = {-1500.0, 500.0, 0.0};
    const double b[3] = {1500.0, 500.0, 0.0};
    // Inverted Gaussian: shoulders at 480 m (endpoints clear them by 20 m), low centre at 0 m.
    auto valley = [](double x, double, double) { return 480.0 - 480.0 * std::exp(-(x * x) / (2.0 * 400.0 * 400.0)); };
    CHECK(terrainLos(a, b, valley, {}, kR) == LosResult::Clear);
}

TEST_CASE("terrainLos: endpoints below terrain report Blocked (#687)", "[los]") {
    const double a[3] = {-100.0, 50.0, 0.0}; // below the 300 m flat ground
    const double b[3] = {100.0, 400.0, 0.0};
    CHECK(terrainLos(a, b, FlatGround{300.0}, {}, kR) == LosResult::Blocked);
}

TEST_CASE("terrainLos: grazing tangent just above the crest is Clear, just below is Blocked (#687)", "[los]") {
    const GaussianRidge ridge{0.0, 300.0, 200.0, 0.0};
    // 2 m clearance band around the 300 m crest.
    const double just_above[3] = {-1000.0, 303.0, 0.0};
    const double above_b[3] = {1000.0, 303.0, 0.0};
    CHECK(terrainLos(just_above, above_b, ridge, {}, kR, 50.0, 1.0) == LosResult::Clear);

    const double just_below[3] = {-1000.0, 298.0, 0.0};
    const double below_b[3] = {1000.0, 298.0, 0.0};
    CHECK(terrainLos(just_below, below_b, ridge, {}, kR, 50.0, 1.0) == LosResult::Blocked);
}

TEST_CASE("terrainLos: result is step-size independent within tolerance (#687)", "[los]") {
    const GaussianRidge ridge{0.0, 350.0, 150.0, 0.0};
    const double a[3] = {-2000.0, 360.0, 0.0}; // skims 10 m over the crest
    const double b[3] = {2000.0, 360.0, 0.0};
    // A fine and a coarse nominal step must agree: the margin-bounded adaptive stride densifies
    // near the crest regardless of the nominal stepM.
    const auto fine = terrainLos(a, b, ridge, {}, kR, 10.0, 1.0);
    const auto coarse = terrainLos(a, b, ridge, {}, kR, 200.0, 1.0);
    CHECK(fine == coarse);
    CHECK(fine == LosResult::Clear);
}

TEST_CASE("terrainLos: Unknown surfaces when readiness fails mid-segment (#687)", "[los]") {
    // Flat clear ground, but the terrain in the middle third of the span is not loaded.
    const double a[3] = {-3000.0, 500.0, 0.0};
    const double b[3] = {3000.0, 500.0, 0.0};
    auto readyExceptMiddle = [](double x, double, double) { return std::abs(x) > 1000.0; };
    CHECK(terrainLos(a, b, FlatGround{0.0}, readyExceptMiddle, kR) == LosResult::Unknown);

    // A definite crossing on a READY sample still wins over Unknown: a ridge in the loaded region.
    const GaussianRidge nearRidge{-2000.0, 900.0, 200.0, 0.0}; // tall ridge at x=-2000 (loaded)
    CHECK(terrainLos(a, b, nearRidge, readyExceptMiddle, kR) == LosResult::Blocked);
}

TEST_CASE("terrainLos: zero-length and vertical segments are safe (#687)", "[los]") {
    // Zero-length above terrain -> Clear; below -> Blocked.
    const double p[3] = {0.0, 500.0, 0.0};
    CHECK(terrainLos(p, p, FlatGround{0.0}, {}, kR) == LosResult::Clear);
    const double low[3] = {0.0, 100.0, 0.0};
    CHECK(terrainLos(low, low, FlatGround{300.0}, {}, kR) == LosResult::Blocked);

    // A near-vertical climb straight up from just above the ground stays Clear.
    const double base[3] = {0.0, 10.0, 0.0};
    const double top[3] = {0.0, 5000.0, 0.0};
    CHECK(terrainLos(base, top, FlatGround{0.0}, {}, kR) == LosResult::Clear);
}
