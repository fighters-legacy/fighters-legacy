// SPDX-License-Identifier: GPL-3.0-or-later
#include "flight/Geodetic.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>

using Catch::Approx;

static constexpr double kPi = std::numbers::pi;

// In the engine's coordinate system, planet centre is at {0, -R, 0}.
// The world origin {0,0,0} is on the sphere surface directly "above" the centre
// along +Y, which corresponds to lat=+π/2 (north pole) in this convention.
TEST_CASE("Geodetic: round-trip at world origin", "[geodetic]") {
    auto lla = fl::worldToGeodetic(0.0, 0.0, 0.0);
    CHECK(lla.lat_rad == Approx(kPi / 2.0).epsilon(1e-9));
    CHECK(lla.lon_rad == Approx(0.0).margin(1e-9));
    CHECK(lla.alt_m == Approx(0.0).margin(1e-3));

    // Round-trip: geodeticToWorld must invert worldToGeodetic
    double rx = 0.0, ry = 0.0, rz = 0.0;
    fl::geodeticToWorld(lla, rx, ry, rz);
    CHECK(rx == Approx(0.0).margin(1e-3));
    CHECK(ry == Approx(0.0).margin(1e-3));
    CHECK(rz == Approx(0.0).margin(1e-3));
}

TEST_CASE("Geodetic: altitude round-trip along Y axis", "[geodetic]") {
    // At {0, 1000, 0}: 1 km above the world origin surface point.
    // The "up" direction from the planet centre is still +Y, so lat stays at π/2.
    constexpr double alt = 1000.0;
    double x = 0.0, y = alt, z = 0.0;
    auto lla = fl::worldToGeodetic(x, y, z);
    CHECK(lla.alt_m == Approx(alt).epsilon(1e-6));
    CHECK(lla.lat_rad == Approx(kPi / 2.0).epsilon(1e-9));

    double rx = 0.0, ry = 0.0, rz = 0.0;
    fl::geodeticToWorld(lla, rx, ry, rz);
    CHECK(rx == Approx(x).margin(1e-3));
    CHECK(ry == Approx(y).margin(1e-3));
    CHECK(rz == Approx(z).margin(1e-3));
}

TEST_CASE("Geodetic: 45-degree latitude", "[geodetic]") {
    constexpr double R = fl::kEarthRadiusM;
    constexpr double lat = kPi / 4.0;
    // At lon=0, lat=45°, alt=0: use geodeticToWorld to get world coords
    const double z = R * std::cos(lat);     // R * cos(45°)
    const double y = R * std::sin(lat) - R; // R*(sin(45°) - 1)
    auto lla = fl::worldToGeodetic(0.0, y, z);
    CHECK(lla.lat_rad == Approx(lat).epsilon(1e-6));
    CHECK(lla.lon_rad == Approx(0.0).margin(1e-9));
    CHECK(lla.alt_m == Approx(0.0).margin(1e-1));
}

TEST_CASE("Geodetic: 30-degree longitude round-trip", "[geodetic]") {
    // Use geodeticToWorld to generate a canonical world position for lat=0, lon=30°,
    // then verify worldToGeodetic recovers the original coordinates.
    constexpr double lon = kPi / 6.0; // 30 degrees
    fl::LatLonAlt orig{0.0, lon, 0.0};
    double wx = 0.0, wy = 0.0, wz = 0.0;
    fl::geodeticToWorld(orig, wx, wy, wz);
    auto lla = fl::worldToGeodetic(wx, wy, wz);
    CHECK(lla.lat_rad == Approx(0.0).margin(1e-6));
    CHECK(lla.lon_rad == Approx(lon).epsilon(1e-6));
    CHECK(lla.alt_m == Approx(0.0).margin(1e-1));
}

TEST_CASE("Geodetic: full round-trip at arbitrary position", "[geodetic]") {
    fl::LatLonAlt orig{0.5, 1.2, 8000.0}; // lat=0.5 rad, lon=1.2 rad, alt=8 km
    double x = 0.0, y = 0.0, z = 0.0;
    fl::geodeticToWorld(orig, x, y, z);
    auto back = fl::worldToGeodetic(x, y, z);
    CHECK(back.lat_rad == Approx(orig.lat_rad).epsilon(1e-9));
    CHECK(back.lon_rad == Approx(orig.lon_rad).epsilon(1e-9));
    CHECK(back.alt_m == Approx(orig.alt_m).margin(1e-3));
}

TEST_CASE("Geodetic: geodeticAltitude convenience function", "[geodetic]") {
    constexpr double R = fl::kEarthRadiusM;
    // At origin, altitude should be 0
    CHECK(fl::geodeticAltitude(0.0, 0.0, 0.0) == Approx(0.0).margin(1e-3));
    // 1 km above surface on Y axis
    CHECK(fl::geodeticAltitude(0.0, 1000.0, 0.0) == Approx(1000.0).epsilon(1e-6));
    // 100 km lateral offset — world-Y is 0 but geodetic altitude is non-zero
    const double D = 1e5;
    const double expected = std::sqrt(D * D + R * R) - R;
    CHECK(fl::geodeticAltitude(D, 0.0, 0.0) == Approx(expected).epsilon(1e-6));
    // Must differ significantly from world-Y at lateral position
    CHECK_FALSE((fl::geodeticAltitude(D, 0.0, 0.0) == Approx(0.0).margin(1.0)));
}
