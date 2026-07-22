// SPDX-License-Identifier: GPL-3.0-or-later
//
// Player autopilot tests (#640): the client-side altitude/heading/speed hold shaping. Sign checks and
// disengage logic on hand-built FlightStates, plus a closed-loop convergence run through the real
// FlightIntegrator (the AI-controller sample() pattern the epic's acceptance criterion calls for).

#include "Autopilot.h"

#include "flight/BuiltinFlightModel.h"
#include "flight/FlightIntegrator.h"
#include "flight/Geodetic.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

using namespace fl;

namespace {
constexpr double kR = kEarthRadiusM;

FlightState levelState(double altM, float speedMps) {
    FlightState s;
    s.pos_world[0] = 0.0;
    s.pos_world[1] = altM; // near the origin, radial altitude ~= world-Y
    s.pos_world[2] = 0.0;
    s.vel_body[0] = speedMps;
    s.quat[0] = 0.f;
    s.quat[1] = 0.f;
    s.quat[2] = 0.f;
    s.quat[3] = 1.f; // identity = wings level
    s.throttle_actual = 0.5f;
    return s;
}
} // namespace

TEST_CASE("Autopilot: AltHold commands nose-up below the target, nose-down above (#640)", "[autopilot]") {
    Autopilot ap;
    auto s = levelState(2000.0, 200.f);
    ap.toggleAltHold(s, kR); // captures 2000 m

    // Drop 300 m below the captured target -> pull up (elevator > 0).
    auto low = levelState(1700.0, 200.f);
    auto cmdLow = ap.compute(low, 1.f / 60.f, kR);
    CHECK(cmdLow.hasPitch);
    CHECK(cmdLow.elevator > 0.f);

    // Climb 300 m above -> push down (elevator < 0).
    auto high = levelState(2300.0, 200.f);
    auto cmdHigh = ap.compute(high, 1.f / 60.f, kR);
    CHECK(cmdHigh.elevator < 0.f);
}

TEST_CASE("Autopilot: AltHold alone levels the wings (#640)", "[autopilot]") {
    Autopilot ap;
    auto s = levelState(2000.0, 200.f);
    ap.toggleAltHold(s, kR);
    // Roll the aircraft right (right wing down) -> the wing-leveler commands left aileron.
    auto banked = levelState(2000.0, 200.f);
    const glm::quat q = glm::angleAxis(glm::radians(30.f), glm::vec3(1.f, 0.f, 0.f)); // roll about +X
    banked.quat[0] = q.x;
    banked.quat[1] = q.y;
    banked.quat[2] = q.z;
    banked.quat[3] = q.w;
    auto cmd = ap.compute(banked, 1.f / 60.f, kR);
    CHECK(cmd.hasRoll);
    CHECK(std::abs(cmd.aileron) > 0.f); // it fights the bank
}

TEST_CASE("Autopilot: SpdHold opens the throttle when slow, closes it when fast (#640)", "[autopilot]") {
    Autopilot ap;
    auto s = levelState(2000.0, 200.f);
    ap.toggleSpdHold(s); // target 200 m/s

    auto slow = levelState(2000.0, 150.f);
    slow.throttle_actual = 0.5f;
    auto cmdSlow = ap.compute(slow, 1.f / 60.f, kR);
    CHECK(cmdSlow.hasThrottle);
    CHECK(cmdSlow.throttle > 0.5f);

    auto fast = levelState(2000.0, 260.f);
    fast.throttle_actual = 0.5f;
    auto cmdFast = ap.compute(fast, 1.f / 60.f, kR);
    CHECK(cmdFast.throttle < 0.5f);
}

TEST_CASE("Autopilot: disengage logic — stick drops attitude holds, throttle drops speed hold (#640)", "[autopilot]") {
    Autopilot ap;
    auto s = levelState(2000.0, 200.f);
    ap.toggleAltHold(s, kR);
    ap.toggleHdgHold(s, kR);
    ap.toggleSpdHold(s);
    REQUIRE(ap.modes() == (Autopilot::AltHold | Autopilot::HdgHold | Autopilot::SpdHold));

    // A rudder input alone disengages nothing.
    ap.notePlayerInput(0.f, 0.f, 0.5f, /*throttleTouched=*/false);
    CHECK(ap.modes() == (Autopilot::AltHold | Autopilot::HdgHold | Autopilot::SpdHold));

    // Elevator past threshold drops both attitude holds but leaves speed hold.
    ap.notePlayerInput(0.4f, 0.f, 0.f, false);
    CHECK((ap.modes() & Autopilot::AltHold) == 0);
    CHECK((ap.modes() & Autopilot::HdgHold) == 0);
    CHECK((ap.modes() & Autopilot::SpdHold) != 0);

    // A throttle touch drops speed hold.
    ap.notePlayerInput(0.f, 0.f, 0.f, /*throttleTouched=*/true);
    CHECK(ap.modes() == 0);

    // Toggling off is idempotent and disengageAll clears everything.
    ap.toggleAltHold(s, kR);
    ap.disengageAll();
    CHECK(ap.modes() == 0);
}

TEST_CASE("Autopilot: closed-loop AltHold climbs toward the target and stays bounded (#640)", "[autopilot]") {
    // The AI-controller sample() pattern: close the loop through the real integrator. The builtin model
    // is a very powerful UFO, so this asserts the load-bearing behaviour — the loop drives the aircraft
    // in the right direction and never diverges — rather than long-horizon settling of an unrealistic
    // airframe. Speed hold is engaged too so the throttle regulates energy instead of runaway climb.
    auto integ = FlightIntegrator(BuiltinFlightModel::get());
    FlightState init = levelState(2000.0, 150.f);
    init.throttle_actual = 0.2f;
    integ.reset(init);

    auto altOf = [&]() {
        return static_cast<float>(
            geodeticAltitude(integ.state().pos_world[0], integ.state().pos_world[1], integ.state().pos_world[2], kR));
    };
    const float startAlt = altOf();

    Autopilot ap;
    FlightState target = integ.state();
    target.pos_world[1] += 300.0; // aim 300 m higher
    ap.toggleAltHold(target, kR);
    ap.toggleSpdHold(integ.state());

    PayloadEffect payload{};
    float maxAbsErr = 0.f;
    float altAt5s = startAlt;
    for (int i = 0; i < 60 * 15; ++i) {
        AutopilotCommand cmd = ap.compute(integ.state(), 1.f / 60.f, kR);
        ControlInput ci;
        if (cmd.hasPitch)
            ci.elevator = cmd.elevator;
        if (cmd.hasRoll) {
            ci.aileron = cmd.aileron;
            ci.rudder = cmd.rudder;
        }
        ci.throttle = cmd.hasThrottle ? cmd.throttle : 0.2f;
        integ.step(1.f / 60.f, ci, payload);
        if (i == 60 * 5)
            altAt5s = altOf();
        maxAbsErr = std::max(maxAbsErr, std::abs(ap.targetAltM() - altOf()));
    }

    // It climbed toward the 300 m-higher target within the first 5 s...
    CHECK(altAt5s > startAlt + 30.f);
    // ...and the altitude error never blew past a sane bound (no divergence / loop).
    CHECK(maxAbsErr < 1500.f);
    CHECK(std::isfinite(altOf()));
}
