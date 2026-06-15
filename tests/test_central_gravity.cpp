// SPDX-License-Identifier: GPL-3.0-or-later
#include "flight/CentralGravityField.h"
#include "flight/IGravityField.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using Catch::Approx;

TEST_CASE("CentralGravityField: gravity at origin matches surface g", "[gravity]") {
    fl::CentralGravityField g(6'371'000.f, 9.80665f);
    float pos[3] = {0.f, 0.f, 0.f};
    auto a = g.accelWorld(pos);
    CHECK(a[0] == Approx(0.f).margin(1e-4f));
    CHECK(a[1] == Approx(-9.80665f).epsilon(1e-4));
    CHECK(a[2] == Approx(0.f).margin(1e-4f));
}

TEST_CASE("CentralGravityField: gravity weaker at altitude", "[gravity]") {
    const float R = 6'371'000.f;
    fl::CentralGravityField g(R, 9.80665f);
    float pos[3] = {0.f, 1000.f, 0.f}; // 1 km above surface
    auto a = g.accelWorld(pos);
    const float mag = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    CHECK(mag < 9.80665f);
    // 1/r² ratio: g * R² / (R+h)²
    const float expected = 9.80665f * R * R / ((R + 1000.f) * (R + 1000.f));
    CHECK(mag == Approx(expected).epsilon(1e-4));
}

TEST_CASE("CentralGravityField: gravity tilts toward planet centre at lateral position", "[gravity]") {
    fl::CentralGravityField g(6'371'000.f, 9.80665f);
    float pos[3] = {1e5f, 0.f, 0.f}; // 100 km from Z-axis
    auto a = g.accelWorld(pos);
    CHECK(a[0] < 0.f); // gravity has negative X component (toward origin)
    CHECK(a[1] < 0.f); // still has downward (negative Y) component
    CHECK(a[2] == Approx(0.f).margin(1e-4f));
}

TEST_CASE("CentralGravityField: geodeticAltitude returns distance from surface", "[gravity]") {
    const float R = 6'371'000.f;
    fl::CentralGravityField g(R, 9.80665f);

    float atSurface[3] = {0.f, 0.f, 0.f};
    CHECK(g.geodeticAltitude(atSurface) == Approx(0.f).margin(1.f));

    float at1km[3] = {0.f, 1000.f, 0.f};
    CHECK(g.geodeticAltitude(at1km) == Approx(1000.f).epsilon(1e-4));

    // Lateral position: world-Y does not equal geodetic altitude
    float lateral[3] = {1e5f, 0.f, 0.f};
    const float r = std::sqrt(1e5f * 1e5f + R * R);
    CHECK(g.geodeticAltitude(lateral) == Approx(r - R).epsilon(1e-4));
    CHECK(g.geodeticAltitude(lateral) != Approx(lateral[1]).epsilon(1e-2));
}

TEST_CASE("CentralGravityField: earthInstance singleton matches explicit construction", "[gravity]") {
    const auto& e = fl::CentralGravityField::earthInstance();
    fl::CentralGravityField explicit_{6'371'000.f, 9.80665f};
    float pos[3] = {0.f, 0.f, 0.f};
    auto a1 = e.accelWorld(pos);
    auto a2 = explicit_.accelWorld(pos);
    CHECK(a1[1] == Approx(a2[1]).epsilon(1e-6));
}

TEST_CASE("CentralGravityField: guard at planet centre returns zero", "[gravity]") {
    const float R = 6'371'000.f;
    fl::CentralGravityField g(R, 9.80665f);
    // Planet centre is at {0, -R, 0}
    float centre[3] = {0.f, -R, 0.f};
    auto a = g.accelWorld(centre);
    CHECK(a[0] == 0.f);
    CHECK(a[1] == 0.f);
    CHECK(a[2] == 0.f);
}

TEST_CASE("IGravityField default: geodeticAltitude returns world-Y", "[gravity]") {
    fl::FlatGravityField flat;
    float p[3] = {1e5f, 500.f, 2e4f};
    CHECK(flat.geodeticAltitude(p) == Approx(500.f).margin(1e-4f));
}
