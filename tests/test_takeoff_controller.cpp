// SPDX-License-Identifier: GPL-3.0-or-later
#include "ai/HoldShortController.h"
#include "ai/TakeoffController.h"
#include "entity/EntityState.h"

#include "entity_fixture.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numbers>

using namespace fl;
using fl::ai::TakeoffController;

TEST_CASE("TakeoffController: sequences LineUp -> Roll -> Rotate -> Climb -> Done", "[ai][takeoff]") {
    // Runway heading 90 (East = +X at the origin), so the identity-attitude aircraft is aligned.
    TakeoffController tc(glm::dvec3{0, 0, 0}, /*headingDeg=*/90.f, /*runwayElevM=*/0.f, /*Vr=*/70.f,
                         /*climboutAgl=*/300.f);
    constexpr double dt = 1.0 / 60.0;

    // LineUp: stationary, throttle commanded to full.
    auto c = tc.sample(fl::mkState(0, 0, 0, 0, 0, 0), 0, dt);
    CHECK(tc.phase() == TakeoffController::Phase::LineUp);
    CHECK(c.throttle == 1.f);

    // Rolling: past the LineUp gate, still full power, elevator neutral (no rotation before Vr).
    c = tc.sample(fl::mkState(0, 0, 0, 30.f, 0, 0), 1, dt);
    CHECK(tc.phase() == TakeoffController::Phase::Roll);
    CHECK(c.throttle == 1.f);
    CHECK(c.elevator == 0.f);

    // Vr reached: rotate the nose up — a MEASURED pull since #1334 (0.25 of travel trims ~11 deg
    // of alpha on the builtin trainer; the old 0.6 left the runway stalled at 38), with the gear
    // commanded down: ControlInput defaults it false, and an uncommanded gear retracting mid-roll
    // is the belly-scrape bug #1334 fixed.
    c = tc.sample(fl::mkState(0, 0, 0, 80.f, 0, 0), 2, dt);
    CHECK(tc.phase() == TakeoffController::Phase::Rotate);
    CHECK(c.elevator > 0.2f);
    CHECK(c.elevator < 0.4f);
    CHECK(c.gear_down);

    // Airborne: climb phase, full power held.
    c = tc.sample(fl::mkState(0, 5.0, 0, 90.f, 5.f, 0), 3, dt);
    CHECK(tc.phase() == TakeoffController::Phase::Climb);
    CHECK(c.throttle == 1.f);

    // Climb-out complete: neutral, so the outer state machine takes over.
    c = tc.sample(fl::mkState(0, 350.0, 0, 120.f, 0, 0), 4, dt);
    CHECK(tc.phase() == TakeoffController::Phase::Done);
    CHECK(c.throttle == 0.f);
    CHECK(c.elevator == 0.f);
    CHECK(c.aileron == 0.f);
}

TEST_CASE("TakeoffController: steers back toward the runway heading when yawed off", "[ai][takeoff]") {
    TakeoffController tc(glm::dvec3{0, 0, 0}, /*headingDeg=*/90.f, /*runwayElevM=*/0.f, /*Vr=*/70.f, 300.f);
    constexpr double dt = 1.0 / 60.0;

    // Yaw the nose 20 deg about +Y (up) so it no longer points down the runway.
    auto yawed = fl::mkState(0, 0, 0, 30.f, 0, 0);
    const float half = 20.f * 0.5f * static_cast<float>(std::numbers::pi_v<double> / 180.0);
    yawed.transform.quat[1] = std::sin(half); // y
    yawed.transform.quat[3] = std::cos(half); // w

    tc.sample(fl::mkState(0, 0, 0, 30.f, 0, 0), 0, dt); // enter Roll aligned
    auto c = tc.sample(yawed, 1, dt);                   // now yawed off heading
    CHECK(tc.phase() == TakeoffController::Phase::Roll);
    CHECK(std::abs(c.rudder) > 0.05f); // actively steering the nosewheel back to centreline
}

TEST_CASE("HoldShortController: outputs neutral surfaces and idle throttle", "[ai][takeoff]") {
    fl::ai::HoldShortController hs;
    auto c = hs.sample(fl::mkState(0, 0, 0, 0, 0, 0), 0, 1.0 / 60.0);
    CHECK(c.throttle == 0.f);
    CHECK(c.elevator == 0.f);
    CHECK(c.aileron == 0.f);
    CHECK(c.rudder == 0.f);
    CHECK(c.wheelBrake == 0.f);
}
