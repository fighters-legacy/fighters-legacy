// SPDX-License-Identifier: GPL-3.0-or-later
//
// fl::aoaRad (#1251).
//
// Three copies of this two-line formula existed -- the HUD's, and the FFB and rumble paths inside
// HapticController, twice in one file. Each carried its own low-speed guard, and they were not even
// spelled the same: one rotated with `conjugate`, the others with `inverse`, which for a unit
// quaternion is the same rotation reached through a redundant divide.
//
// What is worth pinning is the guard and the sign convention, because both are load-bearing for the
// stall cue: get the sign backwards and the buffet fires in a dive, and drop the guard and it fires
// on a parked aircraft.

#include "flight/AirAngles.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <glm/gtc/quaternion.hpp>
#include <numbers>

using Catch::Matchers::WithinAbs;

TEST_CASE("level flight along the nose is zero angle of attack", "[air_angles]") {
    const glm::quat level = glm::quat(1.f, 0.f, 0.f, 0.f); // identity: nose along +X
    CHECK_THAT(fl::aoaRad(level, glm::vec3(200.f, 0.f, 0.f)), WithinAbs(0.f, 1e-6f));
}

TEST_CASE("wind from below is positive angle of attack", "[air_angles]") {
    // Body frame is nose = +X, up = +Y. An aircraft descending along its own -Y while pointing
    // level has the relative wind arriving from below, which is positive AoA -- the sign the stall
    // cue depends on.
    const glm::quat level = glm::quat(1.f, 0.f, 0.f, 0.f);
    const float aoa = fl::aoaRad(level, glm::vec3(200.f, -20.f, 0.f));
    CHECK(aoa > 0.f);
    CHECK_THAT(aoa, WithinAbs(std::atan2(20.f, 200.f), 1e-6f));

    // ...and climbing along +Y is negative.
    CHECK(fl::aoaRad(level, glm::vec3(200.f, 20.f, 0.f)) < 0.f);
}

TEST_CASE("the angle is taken in the body frame, not the world frame", "[air_angles]") {
    // Pitched 10 degrees nose-up but travelling horizontally: the wind arrives 10 degrees below the
    // nose, so AoA is +10 degrees even though the velocity vector is dead level in the world.
    const float tenDeg = 10.f * std::numbers::pi_v<float> / 180.f;
    const glm::quat pitchedUp = glm::angleAxis(tenDeg, glm::vec3(0.f, 0.f, 1.f));
    CHECK_THAT(fl::aoaRad(pitchedUp, glm::vec3(200.f, 0.f, 0.f)), WithinAbs(tenDeg, 1e-5f));
}

TEST_CASE("below the speed guard the angle reads zero, not noise", "[air_angles]") {
    // atan2 of two near-zero components is numerical noise. Ungated, a parked aircraft rocking on
    // its gear would flap the stall cue on and off.
    const glm::quat level = glm::quat(1.f, 0.f, 0.f, 0.f);
    CHECK(fl::aoaRad(level, glm::vec3(0.f, 0.f, 0.f)) == 0.f);
    CHECK(fl::aoaRad(level, glm::vec3(0.01f, -0.01f, 0.f)) == 0.f);

    // The guard is on SPEED, so a slow descent straight down is still gated...
    CHECK(fl::aoaRad(level, glm::vec3(0.f, -0.5f, 0.f)) == 0.f);
    // ...and just above it, the angle is live again.
    CHECK(fl::aoaRad(level, glm::vec3(0.f, -2.f, 0.f)) != 0.f);

    // The threshold is a parameter, so a caller that wants a different one does not re-roll the
    // formula to get it.
    CHECK(fl::aoaRad(level, glm::vec3(0.f, -0.5f, 0.f), 0.1f) != 0.f);
}

TEST_CASE("rotating the velocity with the aircraft leaves the angle unchanged", "[air_angles]") {
    // AoA is a body-relative quantity: yawing the whole picture must not move it.
    const glm::vec3 vel(200.f, -20.f, 0.f);
    const glm::quat level = glm::quat(1.f, 0.f, 0.f, 0.f);
    const float base = fl::aoaRad(level, vel);

    const glm::quat yawed = glm::angleAxis(1.2f, glm::vec3(0.f, 1.f, 0.f));
    CHECK_THAT(fl::aoaRad(yawed, yawed * vel), WithinAbs(base, 1e-5f));
}
