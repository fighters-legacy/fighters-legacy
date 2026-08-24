// SPDX-License-Identifier: GPL-3.0-or-later
// Celestial-frame + Moon-ephemeris tests (#484): sidereal time, the equatorial -> local-horizon
// transform, and the truncated Meeus Moon position. These pin the sign conventions the star/Moon
// sky shader depends on.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "flight/Geodetic.h" // geodeticToWorld, kEarthRadiusM
#include "math/Angles.h"     // fl::kPi<double> / kDegToRad -- the one pi (#1246)
#include "weather/CelestialFrame.h"
#include "weather/LunarPosition.h"
#include "weather/SolarPosition.h" // julianDay

#include <cmath>

using namespace fl;

namespace {
// The shared constants (#1246), not a local re-declaration of them.
constexpr double kDeg = fl::kDegToRad<double>;

// World position of a geodetic (lat, lon) point at sea level.
glm::dvec3 worldAt(double latRad, double lonRad, double R = kEarthRadiusM) {
    double x, y, z;
    geodeticToWorld({latRad, lonRad, 0.0}, x, y, z, R);
    return {x, y, z};
}
} // namespace

TEST_CASE("GMST at J2000 matches the standard 18.697h", "[celestial]") {
    // 2000-01-01 12:00 UTC = JD 2451545.0; GMST ~= 18.697374558 hours (280.46 deg).
    const double gmst = greenwichMeanSiderealTimeRad(2451545.0);
    const double hours = gmst * 12.0 / fl::kPi<double>;
    CHECK(hours == Catch::Approx(18.697374558).margin(1e-3));
}

TEST_CASE("celestial pole sits at altitude = latitude", "[celestial]") {
    // A point at declination +90 (the north celestial pole, ~Polaris) has an altitude equal to the
    // observer's latitude, independent of sidereal time.
    for (double latDeg : {0.0, 30.0, 45.0, 60.0}) {
        const double lat = latDeg * kDeg;
        for (double lst : {0.0, 1.7, 3.9, 5.5}) {
            const glm::vec3 enu = equatorialToEnu(/*ra*/ 2.0, fl::kPi<double> / 2.0, lat, lst);
            // Up component = sin(altitude); altitude should equal the latitude.
            CHECK(std::asin(enu.z) == Catch::Approx(lat).margin(1e-4));
        }
    }
}

TEST_CASE("a star at Dec=lat, RA=LST culminates at the zenith", "[celestial]") {
    const double lat = 40.0 * kDeg;
    const double lst = 3.3;
    const glm::vec3 enu = equatorialToEnu(/*ra=lst -> H=0*/ lst, lat, lat, lst);
    CHECK(enu.z == Catch::Approx(1.0).margin(1e-4)); // straight up
}

TEST_CASE("worldToCelestial is an orthonormal rotation", "[celestial]") {
    const double lat = 51.5 * kDeg, lon = -0.1 * kDeg;
    const glm::dvec3 pos = worldAt(lat, lon);
    const double lst = localSiderealTimeRad(julianDay(2025, 6, 21, 3.0), lon);
    const glm::mat3 M = worldToCelestial(lat, lst, pos, kEarthRadiusM);
    // Orthonormal: M * M^T == I, det == +1.
    const glm::mat3 shouldBeI = M * glm::transpose(M);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            CHECK(shouldBeI[i][j] == Catch::Approx(i == j ? 1.0f : 0.0f).margin(1e-4));
    CHECK(glm::determinant(M) == Catch::Approx(1.0f).margin(1e-4));
}

TEST_CASE("Moon equatorial position is physically bounded", "[celestial][moon]") {
    // Across a lunar month the declination stays within ~+-28.6 deg and RA spans the full circle.
    double minDec = 1e9, maxDec = -1e9, minRa = 1e9, maxRa = -1e9;
    for (int day = 1; day <= 28; ++day) {
        const MoonEquatorial m = moonEquatorial(julianDay(2025, 3, day, 0.0));
        CHECK(m.raRad >= 0.0);
        CHECK(m.raRad < 2.0 * fl::kPi<double> + 1e-9);
        CHECK(m.illuminatedFraction >= 0.0);
        CHECK(m.illuminatedFraction <= 1.0);
        CHECK(m.distanceKm > 350000.0);
        CHECK(m.distanceKm < 410000.0);
        minDec = std::min(minDec, m.decRad);
        maxDec = std::max(maxDec, m.decRad);
        minRa = std::min(minRa, m.raRad);
        maxRa = std::max(maxRa, m.raRad);
    }
    CHECK(maxDec < 29.0 * kDeg);
    CHECK(minDec > -29.0 * kDeg);
    CHECK((maxRa - minRa) > fl::kPi<double>); // sweeps a large arc over the month
}

TEST_CASE("Moon illuminated fraction tracks new and full phases", "[celestial][moon]") {
    // New moon 2000-01-06 ~18:14 UTC; full moon 2000-01-21 ~04:40 UTC.
    const MoonEquatorial neu = moonEquatorial(julianDay(2000, 1, 6, 18.2));
    const MoonEquatorial full = moonEquatorial(julianDay(2000, 1, 21, 4.7));
    CHECK(neu.illuminatedFraction < 0.05);
    CHECK(full.illuminatedFraction > 0.95);
}

TEST_CASE("Moon angular radius is about half a degree", "[celestial][moon]") {
    const double r = moonAngularRadiusRad(385000.0);
    CHECK(r * 180.0 / fl::kPi<double> == Catch::Approx(0.2585).margin(0.02)); // ~0.26 deg radius
}
