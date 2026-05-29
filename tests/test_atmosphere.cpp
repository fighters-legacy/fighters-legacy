// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "flight/Atmosphere.h"

using Catch::Matchers::WithinRel;
using namespace fl;

TEST_CASE("Atmosphere sea level values match ISA standard", "[atmosphere]") {
    auto s = computeAtmosphere(0.f);
    CHECK_THAT(s.density_kg_m3, WithinRel(1.225f, 0.005f));
    CHECK_THAT(s.speed_of_sound_m_s, WithinRel(340.29f, 0.005f));
    CHECK_THAT(s.pressure_pa, WithinRel(101325.f, 0.005f));
}

TEST_CASE("Atmosphere density decreases with altitude", "[atmosphere]") {
    auto s0 = computeAtmosphere(0.f);
    auto s3 = computeAtmosphere(3000.f);
    auto s9 = computeAtmosphere(9000.f);
    CHECK(s3.density_kg_m3 < s0.density_kg_m3);
    CHECK(s9.density_kg_m3 < s3.density_kg_m3);
}

TEST_CASE("Atmosphere tropopause temperature is constant above 11000 m", "[atmosphere]") {
    auto s11 = computeAtmosphere(11000.f);
    auto s15 = computeAtmosphere(15000.f);
    // Speed of sound depends only on temperature; isothermal stratosphere -> same SoS
    CHECK_THAT(s15.speed_of_sound_m_s, WithinRel(s11.speed_of_sound_m_s, 0.001f));
}

TEST_CASE("Atmosphere altitude clamping at boundaries", "[atmosphere]") {
    auto s_neg = computeAtmosphere(-500.f);
    auto s_sl = computeAtmosphere(0.f);
    auto s_max = computeAtmosphere(25000.f);
    auto s_cap = computeAtmosphere(20000.f);
    CHECK_THAT(s_neg.density_kg_m3, WithinRel(s_sl.density_kg_m3, 0.0001f));
    CHECK_THAT(s_max.density_kg_m3, WithinRel(s_cap.density_kg_m3, 0.0001f));
}
