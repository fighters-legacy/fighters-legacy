// SPDX-License-Identifier: GPL-3.0-or-later
#include "ai/LandingController.h"
#include "entity/EntityState.h"

#include <catch2/catch_test_macros.hpp>

using namespace fl;
using fl::ai::LandingController;

// State at (px, alt, pz) with world velocity (vx, vy, vz), identity attitude (nose = +X world).
static fl::EntityState mkState(double px, double alt, double pz, float vx, float vy, float vz) {
    fl::EntityState s{};
    s.id = {1, 1};
    s.transform.pos[0] = px;
    s.transform.pos[1] = alt;
    s.transform.pos[2] = pz;
    s.transform.vel[0] = vx;
    s.transform.vel[1] = vy;
    s.transform.vel[2] = vz;
    s.transform.quat[3] = 1.f;
    return s;
}

// Runway threshold at the origin, heading 90 (approach along +X toward the threshold from -X).
static LandingController makeLc() {
    return LandingController(glm::dvec3{0, 0, 0}, /*headingDeg=*/90.f, /*runwayElevM=*/0.f, /*glideslopeDeg=*/3.5f,
                             /*approachSpeedMps=*/75.f);
}

TEST_CASE("LandingController: tracks the glidepath sign-correctly on final", "[ai][landing]") {
    constexpr double dt = 1.0 / 60.0;
    // 1000 m out, a 3.5 deg glidepath sits at ~61 m. Above it the controller must command nose-down;
    // below it, nose-up.
    {
        LandingController lc = makeLc();
        auto c = lc.sample(mkState(-1000, 120, 0, 75.f, 0, 0), 0, dt); // high
        CHECK(lc.phase() == LandingController::Phase::Final);
        CHECK(c.elevator < 0.f);
    }
    {
        LandingController lc = makeLc();
        auto c = lc.sample(mkState(-1000, 20, 0, 75.f, 0, 0), 0, dt); // low
        CHECK(lc.phase() == LandingController::Phase::Final);
        CHECK(c.elevator > 0.f);
    }
}

TEST_CASE("LandingController: holds the approach speed on final", "[ai][landing]") {
    constexpr double dt = 1.0 / 60.0;
    // Slow on final -> more power; fast on final -> less power.
    LandingController slow = makeLc();
    auto cSlow = slow.sample(mkState(-1000, 61, 0, 40.f, 0, 0), 0, dt);
    LandingController fast = makeLc();
    auto cFast = fast.sample(mkState(-1000, 61, 0, 100.f, 0, 0), 0, dt);
    CHECK(cSlow.throttle > cFast.throttle);
}

TEST_CASE("LandingController: flares, touches down, brakes, and stops", "[ai][landing]") {
    constexpr double dt = 1.0 / 60.0;
    LandingController lc = makeLc();

    // Final at altitude.
    lc.sample(mkState(-1000, 80, 0, 75.f, -3.f, 0), 0, dt);
    CHECK(lc.phase() == LandingController::Phase::Final);

    // Below the flare gate: idle power, nose-up to arrest the sink.
    auto cFlare = lc.sample(mkState(-50, 8, 0, 70.f, -2.f, 0), 1, dt);
    CHECK(lc.phase() == LandingController::Phase::Flare);
    CHECK(cFlare.elevator > 0.f);
    CHECK(cFlare.throttle == 0.f);

    // Touchdown: rollout with full brakes.
    auto cRoll = lc.sample(mkState(0, 0.3, 0, 45.f, 0, 0), 2, dt);
    CHECK(lc.phase() == LandingController::Phase::Rollout);
    CHECK(cRoll.wheelBrake == 1.f);
    CHECK(cRoll.throttle == 0.f);

    // Stopped: done, neutral output.
    auto cDone = lc.sample(mkState(0, 0.0, 0, 1.f, 0, 0), 3, dt);
    CHECK(lc.phase() == LandingController::Phase::Done);
    CHECK(cDone.wheelBrake == 0.f);
    CHECK(cDone.throttle == 0.f);
}
