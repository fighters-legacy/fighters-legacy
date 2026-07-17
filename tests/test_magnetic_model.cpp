// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "nav/MagneticModel.h"

#include <cmath>

using Catch::Matchers::WithinAbs;
using namespace fl;

static constexpr double kDeg = 0.017453292519943295;

// Validation against NOAA's OFFICIAL "Test Values for WMM2025" table (independent of our synthesis —
// a transcription error in a coefficient or a recursion bug would fail these). Declination D (deg):
//   date    alt(km)  lat   lon    D
//   2025.0    0       80     0    1.28
//   2025.0    0        0   120   -0.16
//   2025.0    0      -80   240   68.78
//   2025.0  100       80     0    0.85
//   2025.0  100        0   120   -0.15
//   2025.0  100      -80   240   68.21
//   2027.5    0       80     0    2.59
//   2027.5    0        0   120   -0.24
//   2027.5    0      -80   240   68.49
TEST_CASE("MagneticModel: WMM2025 declination matches NOAA test values", "[magnetic]") {
    const MagneticModel& m = MagneticModel::wmm2025();
    REQUIRE(m.valid());
    CHECK(m.epochYear() == 2025.0);

    auto D = [&](double year, double altKm, double latDeg, double lonDeg) {
        return m.declinationDeg(latDeg * kDeg, lonDeg * kDeg, altKm * 1000.0, year);
    };

    // NOAA computes with double precision; the report notes single precision can differ ~0.1 nT.
    // A 0.05° declination tolerance is well within transcription/algorithm fidelity.
    CHECK_THAT(D(2025.0, 0, 80, 0), WithinAbs(1.28, 0.05));
    CHECK_THAT(D(2025.0, 0, 0, 120), WithinAbs(-0.16, 0.05));
    CHECK_THAT(D(2025.0, 0, -80, 240), WithinAbs(68.78, 0.05));
    CHECK_THAT(D(2025.0, 100, 80, 0), WithinAbs(0.85, 0.05));
    CHECK_THAT(D(2025.0, 100, 0, 120), WithinAbs(-0.15, 0.05));
    CHECK_THAT(D(2025.0, 100, -80, 240), WithinAbs(68.21, 0.05));
    CHECK_THAT(D(2027.5, 0, 80, 0), WithinAbs(2.59, 0.05));
    CHECK_THAT(D(2027.5, 0, 0, 120), WithinAbs(-0.24, 0.05));
    CHECK_THAT(D(2027.5, 0, -80, 240), WithinAbs(68.49, 0.05));
}

TEST_CASE("MagneticModel: full field components match NOAA (H, F, I)", "[magnetic]") {
    const MagneticModel& m = MagneticModel::wmm2025();
    // 2025.0, 0 km, 80N 0E: X=6521.6 Y=145.9 Z=54791.5 H=6523.2 F=55178.5 I=83.21
    const GeoMagField f = m.field(80 * kDeg, 0 * kDeg, 0.0, 2025.0);
    CHECK_THAT(f.northNt, WithinAbs(6521.6, 2.0));
    CHECK_THAT(f.eastNt, WithinAbs(145.9, 2.0));
    CHECK_THAT(f.downNt, WithinAbs(54791.5, 5.0));
    CHECK_THAT(f.horizontalNt, WithinAbs(6523.2, 2.0));
    CHECK_THAT(f.totalNt, WithinAbs(55178.5, 5.0));
    CHECK_THAT(f.inclinationDeg, WithinAbs(83.21, 0.05));
}

TEST_CASE("MagneticModel: an invalid COF yields an inert model", "[magnetic]") {
    const MagneticModel bad = MagneticModel::parseCof("not a header\n");
    CHECK_FALSE(bad.valid());
    CHECK(bad.declinationDeg(45 * kDeg, 10 * kDeg, 0.0, 2025.0) == 0.0); // graceful: true north
}

TEST_CASE("MagneticModel: declination varies widely by location (globally non-trivial)", "[magnetic]") {
    const MagneticModel& m = MagneticModel::wmm2025();
    // Two far-apart points should give materially different declinations (the whole point of the model).
    const double dEast = m.declinationDeg(0 * kDeg, 120 * kDeg, 0.0, 2025.0);
    const double dWest = m.declinationDeg(-80 * kDeg, 240 * kDeg, 0.0, 2025.0);
    CHECK(std::fabs(dWest - dEast) > 10.0);
}
