// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "weapon/Turret.h"

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>

using Catch::Approx;
using Catch::Matchers::WithinAbs;

namespace {
constexpr float kPi = 3.14159265358979323846f;

fl::TurretLimits fullRing() {
    return fl::TurretLimits{-kPi, kPi, -kPi / 2.f, kPi / 2.f, glm::radians(60.f)};
}

// A limited defensive turret: +/-60 az, -10..+80 el, 45 deg/s slew.
fl::TurretLimits limited() {
    return fl::TurretLimits{glm::radians(-60.f), glm::radians(60.f), glm::radians(-10.f), glm::radians(80.f),
                            glm::radians(45.f)};
}
} // namespace

TEST_CASE("turretBoreMount: az=el=0 is the forward bore", "[turret]") {
    const glm::vec3 b = fl::turretBoreMount(0.f, 0.f);
    CHECK_THAT(b.x, WithinAbs(1.f, 1e-5f));
    CHECK_THAT(b.y, WithinAbs(0.f, 1e-5f));
    CHECK_THAT(b.z, WithinAbs(0.f, 1e-5f));
}

TEST_CASE("turretBoreMount: elevation raises the bore toward +Y", "[turret]") {
    const glm::vec3 b = fl::turretBoreMount(0.f, kPi / 2.f);
    CHECK_THAT(b.y, WithinAbs(1.f, 1e-5f));
}

TEST_CASE("turretBoreMount/turretAimToAzEl round-trip", "[turret]") {
    for (float az : {-1.2f, -0.3f, 0.f, 0.5f, 1.1f}) {
        for (float el : {-0.8f, 0.f, 0.4f, 1.0f}) {
            const glm::vec3 b = fl::turretBoreMount(az, el);
            float raz = 0.f;
            float rel = 0.f;
            fl::turretAimToAzEl(b, raz, rel);
            CHECK_THAT(raz, WithinAbs(az, 1e-4f));
            CHECK_THAT(rel, WithinAbs(el, 1e-4f));
        }
    }
}

TEST_CASE("turretAimToAzEl: a degenerate direction yields (0,0)", "[turret]") {
    float az = 9.f;
    float el = 9.f;
    fl::turretAimToAzEl(glm::vec3{0.f, 0.f, 0.f}, az, el);
    CHECK(az == 0.f);
    CHECK(el == 0.f);
}

TEST_CASE("commandTurretMount: clamps the command to the traverse limits", "[turret]") {
    const fl::TurretLimits lim = limited();
    fl::TurretState t;
    // Aim hard right (past the +60 az limit) and steeply down (past -10 el): both clamp.
    fl::commandTurretMount(t, lim, fl::turretBoreMount(glm::radians(120.f), glm::radians(-45.f)));
    CHECK_THAT(t.cmdAzRad, WithinAbs(lim.azMaxRad, 1e-4f));
    CHECK_THAT(t.cmdElRad, WithinAbs(lim.elMinRad, 1e-4f));
}

TEST_CASE("stepTurret: slews at the servo rate and reaches the target", "[turret]") {
    const fl::TurretLimits lim = limited(); // 45 deg/s
    fl::TurretState t;
    fl::commandTurretMount(t, lim, fl::turretBoreMount(glm::radians(45.f), 0.f));

    // One 0.1 s step advances at most 4.5 deg.
    fl::stepTurret(t, lim, 0.1f);
    CHECK_THAT(glm::degrees(t.azRad), WithinAbs(4.5f, 1e-3f));
    CHECK(t.azRad < t.cmdAzRad);

    // Enough steps to converge; then it holds exactly on target (no overshoot / chatter).
    for (int i = 0; i < 200; ++i)
        fl::stepTurret(t, lim, 0.1f);
    CHECK_THAT(t.azRad, WithinAbs(glm::radians(45.f), 1e-4f));
    fl::stepTurret(t, lim, 0.1f);
    CHECK_THAT(t.azRad, WithinAbs(glm::radians(45.f), 1e-4f));
}

TEST_CASE("stepTurret: never leaves the traverse limits even for an out-of-range command", "[turret]") {
    const fl::TurretLimits lim = limited();
    fl::TurretState t;
    fl::commandTurretMount(t, lim, fl::turretBoreMount(glm::radians(200.f), glm::radians(200.f)));
    for (int i = 0; i < 500; ++i)
        fl::stepTurret(t, lim, 0.1f);
    CHECK(t.azRad <= lim.azMaxRad + 1e-4f);
    CHECK(t.azRad >= lim.azMinRad - 1e-4f);
    CHECK(t.elRad <= lim.elMaxRad + 1e-4f);
    CHECK(t.elRad >= lim.elMinRad - 1e-4f);
}

TEST_CASE("stepTurret: a full-circle ring takes the shortest arc across +/-pi", "[turret]") {
    const fl::TurretLimits lim = fullRing(); // 60 deg/s
    fl::TurretState t;
    t.azRad = glm::radians(170.f);
    // Command +/-180-ish -> -170 deg: the shortest arc is +20 deg (through 180), not -340.
    t.cmdAzRad = glm::radians(-170.f);
    fl::stepTurret(t, lim, 0.1f); // 6 deg step toward the target the SHORT way (+6 deg -> 176 deg)
    CHECK_THAT(glm::degrees(t.azRad), WithinAbs(176.f, 0.5f));
    CHECK(std::abs(glm::degrees(t.azRad)) > 170.f); // it went toward pi, not back through 0
}

TEST_CASE("turretWorldDir: identity mount + airframe yaw rotates the bore", "[turret]") {
    fl::TurretState t; // az=el=0 -> bore +X in the mount frame
    const glm::quat identity{1.f, 0.f, 0.f, 0.f};
    // Airframe yawed -90 deg about +Y maps +X -> +Z (a right yaw, per the guidance convention).
    const glm::quat yawRight = glm::angleAxis(glm::radians(-90.f), glm::vec3{0.f, 1.f, 0.f});
    const glm::vec3 dir = fl::turretWorldDir(t, identity, yawRight);
    CHECK_THAT(dir.z, WithinAbs(1.f, 1e-4f));
    CHECK_THAT(dir.x, WithinAbs(0.f, 1e-4f));
}

TEST_CASE("commandTurretWorld: a world aim behind a rear turret resolves within limits", "[turret]") {
    // A tail turret whose rest orientation points aft (180 deg about +Y): its bore is -X in body.
    const fl::TurretLimits lim = limited();
    const glm::quat mountAft = glm::angleAxis(kPi, glm::vec3{0.f, 1.f, 0.f});
    const glm::quat airframeIdentity{1.f, 0.f, 0.f, 0.f};
    fl::TurretState t;
    // Aim straight aft in the world (-X): dead ahead for an aft-facing mount, so az=el=0.
    fl::commandTurretWorld(t, lim, mountAft, airframeIdentity, glm::vec3{-1.f, 0.f, 0.f});
    CHECK_THAT(t.cmdAzRad, WithinAbs(0.f, 1e-4f));
    CHECK_THAT(t.cmdElRad, WithinAbs(0.f, 1e-4f));

    // Aim forward in the world (+X) is behind the aft mount -> az pins to a traverse limit.
    fl::commandTurretWorld(t, lim, mountAft, airframeIdentity, glm::vec3{1.f, 0.f, 0.f});
    CHECK(std::abs(t.cmdAzRad) == Approx(lim.azMaxRad).margin(1e-4f));
}
