// SPDX-License-Identifier: GPL-3.0-or-later
#include "ai/HoldShortController.h"
#include "ai/TakeoffController.h"
#include "entity/EntityState.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numbers>

using namespace fl;
using fl::ai::TakeoffController;

// On-ground/airborne state at (px, alt, pz) with world velocity (vx, vy, vz) and identity attitude
// (forward = +X world). Near the world origin this is the north pole, where local up = +Y and
// localAltitude == alt, and East = +X (so runway heading 90 deg points along +X = the nose).
static fl::EntityState mkState(double px, double alt, double pz, float vx, float vy, float vz) {
    fl::EntityState s{};
    s.id = {1, 1};
    s.transform.pos[0] = px;
    s.transform.pos[1] = alt;
    s.transform.pos[2] = pz;
    s.transform.vel[0] = vx;
    s.transform.vel[1] = vy;
    s.transform.vel[2] = vz;
    s.transform.quat[3] = 1.f; // identity
    return s;
}

TEST_CASE("TakeoffController: sequences LineUp -> Roll -> Rotate -> Climb -> Done", "[ai][takeoff]") {
    // Runway heading 90 (East = +X at the origin), so the identity-attitude aircraft is aligned.
    TakeoffController tc(glm::dvec3{0, 0, 0}, /*headingDeg=*/90.f, /*runwayElevM=*/0.f, /*Vr=*/70.f,
                         /*climboutAgl=*/300.f);
    constexpr double dt = 1.0 / 60.0;

    // LineUp: stationary, throttle commanded to full.
    auto c = tc.sample(mkState(0, 0, 0, 0, 0, 0), 0, dt);
    CHECK(tc.phase() == TakeoffController::Phase::LineUp);
    CHECK(c.throttle == 1.f);

    // Rolling: past the LineUp gate, still full power, elevator neutral (no rotation before Vr).
    c = tc.sample(mkState(0, 0, 0, 30.f, 0, 0), 1, dt);
    CHECK(tc.phase() == TakeoffController::Phase::Roll);
    CHECK(c.throttle == 1.f);
    CHECK(c.elevator == 0.f);

    // Vr reached: rotate the nose up.
    c = tc.sample(mkState(0, 0, 0, 80.f, 0, 0), 2, dt);
    CHECK(tc.phase() == TakeoffController::Phase::Rotate);
    CHECK(c.elevator > 0.5f);

    // Airborne: climb phase, full power held.
    c = tc.sample(mkState(0, 5.0, 0, 90.f, 5.f, 0), 3, dt);
    CHECK(tc.phase() == TakeoffController::Phase::Climb);
    CHECK(c.throttle == 1.f);

    // Climb-out complete: neutral, so the outer state machine takes over.
    c = tc.sample(mkState(0, 350.0, 0, 120.f, 0, 0), 4, dt);
    CHECK(tc.phase() == TakeoffController::Phase::Done);
    CHECK(c.throttle == 0.f);
    CHECK(c.elevator == 0.f);
    CHECK(c.aileron == 0.f);
}

TEST_CASE("TakeoffController: steers back toward the runway heading when yawed off", "[ai][takeoff]") {
    TakeoffController tc(glm::dvec3{0, 0, 0}, /*headingDeg=*/90.f, /*runwayElevM=*/0.f, /*Vr=*/70.f, 300.f);
    constexpr double dt = 1.0 / 60.0;

    // Yaw the nose 20 deg about +Y (up) so it no longer points down the runway.
    auto yawed = mkState(0, 0, 0, 30.f, 0, 0);
    const float half = 20.f * 0.5f * static_cast<float>(std::numbers::pi_v<double> / 180.0);
    yawed.transform.quat[1] = std::sin(half); // y
    yawed.transform.quat[3] = std::cos(half); // w

    tc.sample(mkState(0, 0, 0, 30.f, 0, 0), 0, dt); // enter Roll aligned
    auto c = tc.sample(yawed, 1, dt);               // now yawed off heading
    CHECK(tc.phase() == TakeoffController::Phase::Roll);
    CHECK(std::abs(c.rudder) > 0.05f); // actively steering the nosewheel back to centreline
}

TEST_CASE("HoldShortController: outputs neutral surfaces and idle throttle", "[ai][takeoff]") {
    fl::ai::HoldShortController hs;
    auto c = hs.sample(mkState(0, 0, 0, 0, 0, 0), 0, 1.0 / 60.0);
    CHECK(c.throttle == 0.f);
    CHECK(c.elevator == 0.f);
    CHECK(c.aileron == 0.f);
    CHECK(c.rudder == 0.f);
    CHECK(c.wheelBrake == 0.f);
}
