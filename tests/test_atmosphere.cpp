// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "flight/Atmosphere.h"

#include <algorithm>
#include <cmath>

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
    CHECK_THAT(s_neg.density_kg_m3, WithinRel(s_sl.density_kg_m3, 0.0001f));
    // Above 20 km is no longer a clamp (#354): the 1976 layers continue, so the air keeps
    // thinning instead of freezing at the old ceiling.
    CHECK(computeAtmosphere(25000.f).density_kg_m3 < computeAtmosphere(20000.f).density_kg_m3);
}

// ---------------------------------------------------------------------------
// US Standard Atmosphere 1976 extension (#354)
// ---------------------------------------------------------------------------

// The pre-#354 implementation, verbatim: the 0-20 km path must be BIT-IDENTICAL to it, because
// fm-trim's CI gates, the generated manual, and client prediction all quote numbers from it.
namespace {
fl::AtmosphereState legacyIsa(float altitude_m) {
    constexpr float kT0 = 288.15f;
    constexpr float kP0 = 101325.f;
    constexpr float kLapseRate = 0.0065f;
    constexpr float kTropopause = 11000.f;
    constexpr float kTtrop = kT0 - kLapseRate * kTropopause;
    constexpr float kGamma = 1.4f;
    constexpr float kR = 287.05f;
    constexpr float kG = 9.80665f;
    constexpr float kExponent = kG / (kLapseRate * kR);

    altitude_m = std::clamp(altitude_m, 0.f, 20000.f);
    float T, P;
    if (altitude_m <= kTropopause) {
        T = kT0 - kLapseRate * altitude_m;
        P = kP0 * std::pow(T / kT0, kExponent);
    } else {
        float Ptrop = kP0 * std::pow(kTtrop / kT0, kExponent);
        T = kTtrop;
        P = Ptrop * std::exp(-kG * (altitude_m - kTropopause) / (kR * kTtrop));
    }
    fl::AtmosphereState s;
    s.pressure_pa = P;
    s.density_kg_m3 = P / (kR * T);
    s.speed_of_sound_m_s = std::sqrt(kGamma * kR * T);
    return s;
}
} // namespace

TEST_CASE("atmosphere: below 20 km is BIT-IDENTICAL to the pre-#354 implementation", "[atmosphere]") {
    for (float h = -100.f; h <= 20000.f; h += 137.f) {
        const auto now = fl::computeAtmosphere(h);
        const auto then = legacyIsa(h);
        REQUIRE(now.pressure_pa == then.pressure_pa);     // exact, not approximate
        REQUIRE(now.density_kg_m3 == then.density_kg_m3); //
        REQUIRE(now.speed_of_sound_m_s == then.speed_of_sound_m_s);
    }
    // The boundary itself, exactly.
    REQUIRE(fl::computeAtmosphere(20000.f).pressure_pa == legacyIsa(20000.f).pressure_pa);
}

TEST_CASE("atmosphere: continuous across every 1976 layer boundary", "[atmosphere]") {
    const float boundaries[] = {20000.f, 32000.f, 47000.f, 51000.f, 71000.f};
    for (float b : boundaries) {
        const auto below = fl::computeAtmosphere(b - 1.f);
        const auto above = fl::computeAtmosphere(b + 1.f);
        CHECK(std::abs(above.pressure_pa - below.pressure_pa) / below.pressure_pa < 0.005f);
        CHECK(std::abs(above.density_kg_m3 - below.density_kg_m3) / below.density_kg_m3 < 0.005f);
    }
}

TEST_CASE("atmosphere: pressure decreases monotonically to the vacuum line", "[atmosphere]") {
    float prev = fl::computeAtmosphere(0.f).pressure_pa;
    for (float h = 500.f; h < 86000.f; h += 500.f) {
        const float p = fl::computeAtmosphere(h).pressure_pa;
        REQUIRE(p < prev);
        REQUIRE(p > 0.f);
        prev = p;
    }
}

TEST_CASE("atmosphere: sane reference values in the new layers, vacuum above 86 km", "[atmosphere]") {
    // US Standard Atmosphere 1976 tabulated densities (kg/m^3), loose bands.
    const float rho32 = fl::computeAtmosphere(32000.f).density_kg_m3;
    CHECK(rho32 > 0.010f); // table: ~0.0132
    CHECK(rho32 < 0.017f);
    const float rho50 = fl::computeAtmosphere(50000.f).density_kg_m3;
    CHECK(rho50 > 0.0007f); // table: ~0.00103
    CHECK(rho50 < 0.0014f);
    const float rho70 = fl::computeAtmosphere(70000.f).density_kg_m3;
    CHECK(rho70 > 0.00004f); // table: ~0.00008
    CHECK(rho70 < 0.00012f);

    const auto vac = fl::computeAtmosphere(100000.f);
    CHECK(vac.density_kg_m3 == 0.f);
    CHECK(vac.pressure_pa == 0.f);
    CHECK(vac.speed_of_sound_m_s > 0.f); // Mach stays finite (and meaningless, which is honest)
}
