// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "weather/SolarPosition.h"

#include <cmath>
#include <glm/geometric.hpp> // glm::length

using Catch::Matchers::WithinAbs;
using namespace fl;

static constexpr double kDeg = 0.017453292519943295;

TEST_CASE("SolarPosition: declination tracks the seasons", "[solar]") {
    // June solstice ~ +23.44°, December solstice ~ −23.44°, equinoxes ~ 0°.
    CHECK_THAT(solarDeclinationRad(julianDay(2025, 6, 21, 0.0)), WithinAbs(23.44 * kDeg, 0.4 * kDeg));
    CHECK_THAT(solarDeclinationRad(julianDay(2025, 12, 21, 12.0)), WithinAbs(-23.44 * kDeg, 0.4 * kDeg));
    CHECK_THAT(solarDeclinationRad(julianDay(2025, 3, 20, 12.0)), WithinAbs(0.0, 1.5 * kDeg));
}

TEST_CASE("SolarPosition: Julian day round-trips the Unix epoch", "[solar]") {
    CHECK_THAT(julianDayFromUnixSeconds(0.0), WithinAbs(2440587.5, 1e-6)); // 1970-01-01 00:00 UTC
    CHECK_THAT(julianDayFromUnixSeconds(0.0), WithinAbs(julianDay(1970, 1, 1, 0.0), 1e-6));
}

TEST_CASE("SolarPosition: noon elevation equals 90 - |lat - decl|", "[solar]") {
    // At local solar noon the sun bears due south/north and sits at 90° − |lat − declination|.
    const double jd0 = julianDay(2025, 6, 21, 12.0);
    const double eot = equationOfTimeMinutes(jd0);
    const double jdNoon = julianDay(2025, 6, 21, 12.0 - eot / 60.0); // solar noon at lon 0
    const double decl = solarDeclinationRad(jdNoon);
    for (double latDeg : {0.0, 23.44, 45.0, -30.0}) {
        const double lat = latDeg * kDeg;
        const SolarAngles a = solarAngles(lat, 0.0, jdNoon);
        const double expectedEl = (kPiD / 2.0) - std::fabs(lat - decl);
        CHECK_THAT(a.elevationRad, WithinAbs(expectedEl, 0.5 * kDeg));
        // Bearing: due south (az≈180°) when north of the sub-solar latitude, due north (≈0/360) below.
        if (lat > decl + 1.0 * kDeg)
            CHECK_THAT(a.azimuthRad, WithinAbs(kPiD, 2.0 * kDeg));
        else if (lat < decl - 1.0 * kDeg)
            CHECK(std::fmod(a.azimuthRad + 2 * kPiD, 2 * kPiD) < 2.0 * kDeg);
    }
}

TEST_CASE("SolarPosition: terminator moves with longitude", "[solar]") {
    // At a fixed UTC (near noon over Greenwich) the sun is up at lon 0 and down at the antimeridian.
    const double jd = julianDay(2025, 6, 21, 12.0);
    const SolarAngles at0 = solarAngles(0.0, 0.0, jd);
    const SolarAngles at180 = solarAngles(0.0, 180.0 * kDeg, jd);
    CHECK(at0.elevationRad > 0.0);   // daytime at Greenwich
    CHECK(at180.elevationRad < 0.0); // night on the far side
}

TEST_CASE("SolarPosition: azimuth is eastern in the morning, western in the afternoon", "[solar]") {
    // Equinox, 45°N, lon 0. Morning solar time → sun in the east (az < 180°); afternoon → west.
    const double lat = 45.0 * kDeg;
    const double jdMorning = julianDay(2025, 3, 20, 9.0); // ~09:00 UTC ≈ morning solar time at lon 0
    const double jdAfternoon = julianDay(2025, 3, 20, 15.0);
    const SolarAngles am = solarAngles(lat, 0.0, jdMorning);
    const SolarAngles pm = solarAngles(lat, 0.0, jdAfternoon);
    CHECK(am.azimuthRad < kPiD); // eastern half
    CHECK(pm.azimuthRad > kPiD); // western half
}

TEST_CASE("SolarPosition: ENU sun vector points up when the sun is high", "[solar]") {
    const double jd0 = julianDay(2025, 6, 21, 12.0);
    const double eot = equationOfTimeMinutes(jd0);
    const double jdNoon = julianDay(2025, 6, 21, 12.0 - eot / 60.0);
    const glm::vec3 d = sunDirectionEnu(solarAngles(23.44 * kDeg, 0.0, jdNoon)); // sub-solar point
    CHECK(d.z > 0.99f); // Up component ~1 at the sub-solar point
    CHECK(std::fabs(glm::length(d) - 1.f) < 1e-3f);
}
