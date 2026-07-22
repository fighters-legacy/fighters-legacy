// SPDX-License-Identifier: GPL-3.0-or-later
//
// Padlock tracker tests (#697): the pure aim + lock state machine. Scripted target trajectories with
// injected dt verify continuous aim across an overhead pass (no 2pi jump), frame-rate independence,
// the break-grace hysteresis, the reacquire-window expiry, and no entry pop.

#include "PadlockTracker.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>
#include <numbers>

using namespace fl;

namespace {
float angleBetween(glm::vec3 a, glm::vec3 b) {
    return std::acos(std::clamp(glm::dot(glm::normalize(a), glm::normalize(b)), -1.0f, 1.0f));
}

PadlockInputs baseInputs() {
    PadlockInputs in;
    in.dt = 1.0f / 60.0f;
    in.ownPos = {0, 0, 0};
    in.ownOrient = glm::quat(1, 0, 0, 0); // identity: forward +X
    in.worldUp = {0, 1, 0};
    in.terrainLos = LosResult::Clear;
    return in;
}
} // namespace

TEST_CASE("PadlockTracker: entering from the current view does not pop (#697)", "[padlock]") {
    PadlockTracker pt;
    const glm::vec3 fwd = glm::normalize(glm::vec3{1, 0, 0});
    pt.enter(fwd, {0, 1, 0});
    auto in = baseInputs();
    in.targetPos = {1000, 0, 0}; // dead ahead
    const PadlockPose p = pt.update(in);
    CHECK(pt.state() == PadlockState::Locked);
    CHECK(angleBetween(p.forward, fwd) < 0.02f); // first frame barely moves
}

TEST_CASE("PadlockTracker: an overhead pass keeps the aim continuous (no 2pi jump) (#697)", "[padlock]") {
    PadlockTracker pt;
    pt.enter({1, 0, 0}, {0, 1, 0});
    auto in = baseInputs();
    // The target flies from ahead, straight overhead, to behind — the classic pitch-wrap trap.
    glm::vec3 prev{1, 0, 0};
    float maxStep = 0.0f;
    for (int i = 0; i <= 200; ++i) {
        const float t = static_cast<float>(i) / 200.0f;  // 0..1
        const float ang = t * std::numbers::pi_v<float>; // sweep 0..180 deg in the X-Y plane
        in.targetPos = glm::dvec3{std::cos(ang) * 1000.0, std::sin(ang) * 1000.0, 0.0};
        const PadlockPose p = pt.update(in);
        maxStep = std::max(maxStep, angleBetween(p.forward, prev));
        prev = p.forward;
    }
    // No frame ever jumps more than the rate limit allows (240 deg/s * dt ~= 4 deg); a 2pi wrap would
    // show a ~pi step. Comfortably bounded.
    CHECK(maxStep < 0.15f); // < ~8.6 deg per frame
}

TEST_CASE("PadlockTracker: aim is frame-rate independent (60 vs 240 fps) (#697)", "[padlock]") {
    auto run = [](float dt, int steps) {
        PadlockTracker pt;
        pt.enter({1, 0, 0}, {0, 1, 0});
        PadlockInputs in = baseInputs();
        in.dt = dt;
        in.targetPos = {0, 1000, 0}; // straight up (a big slew from forward)
        PadlockPose p;
        for (int i = 0; i < steps; ++i)
            p = pt.update(in);
        return p.forward;
    };
    const glm::vec3 at60 = run(1.0f / 60.0f, 60);    // 1 second
    const glm::vec3 at240 = run(1.0f / 240.0f, 240); // 1 second
    CHECK(angleBetween(at60, at240) < 0.05f);
}

TEST_CASE("PadlockTracker: a brief mask within the grace window keeps the lock (#697)", "[padlock]") {
    PadlockTracker pt;
    pt.enter({1, 0, 0}, {0, 1, 0});
    auto in = baseInputs();
    in.targetPos = {1000, 0, 0};

    pt.update(in); // Locked
    // Terrain masks the target for 0.3 s (< 0.4 s grace) then clears.
    in.terrainLos = LosResult::Blocked;
    for (int i = 0; i < 18; ++i) // 18 frames ~= 0.3 s at 60 Hz
        pt.update(in);
    CHECK(pt.state() == PadlockState::Breaking);
    in.terrainLos = LosResult::Clear;
    pt.update(in);
    CHECK(pt.state() == PadlockState::Locked); // regained, never dropped to Reacquire
}

TEST_CASE("PadlockTracker: a sustained mask expires through Reacquire to Off (#697)", "[padlock]") {
    PadlockTracker pt;
    pt.enter({1, 0, 0}, {0, 1, 0});
    auto in = baseInputs();
    in.targetPos = {1000, 0, 0};
    pt.update(in);

    in.terrainLos = LosResult::Blocked;
    // Past the 0.4 s grace -> Reacquire.
    for (int i = 0; i < 30; ++i) // 0.5 s
        pt.update(in);
    CHECK(pt.state() == PadlockState::Reacquire);

    // Past the 4 s reacquire window -> exitToCockpit and Off.
    bool exited = false;
    for (int i = 0; i < 60 * 5; ++i) {
        const PadlockPose p = pt.update(in);
        if (p.exitToCockpit)
            exited = true;
    }
    CHECK(exited);
    CHECK(pt.state() == PadlockState::Off);
}

TEST_CASE("PadlockTracker: Reacquire slews the aim back toward the airframe forward (#697)", "[padlock]") {
    PadlockTracker pt;
    pt.enter({1, 0, 0}, {0, 1, 0});
    auto in = baseInputs();
    in.targetPos = {0, 1000, 0}; // up and to the side, so the aim starts pointed away from forward

    // Let the aim slew up toward the target while Locked.
    for (int i = 0; i < 30; ++i)
        pt.update(in);
    // Now mask it long enough to enter Reacquire.
    in.terrainLos = LosResult::Blocked;
    for (int i = 0; i < 30; ++i)
        pt.update(in);
    REQUIRE(pt.state() == PadlockState::Reacquire);

    const glm::vec3 airframeFwd{1, 0, 0};
    PadlockPose p = pt.update(in);
    const float before = angleBetween(p.forward, airframeFwd);
    for (int i = 0; i < 30; ++i)
        p = pt.update(in);
    const float after = angleBetween(p.forward, airframeFwd);
    CHECK(after < before); // returning toward boresight
}

TEST_CASE("PadlockTracker: the cockpit envelope masks a target far behind and below (#697)", "[padlock]") {
    PadlockTracker pt;
    pt.enter({1, 0, 0}, {0, 1, 0});
    auto in = baseInputs();
    // Straight behind and well below the horizon: |az| ~180, el well under -5 deg -> masked by envelope.
    in.targetPos = {-1000, -400, 0};
    pt.update(in);
    CHECK(pt.state() == PadlockState::Breaking); // visibility failed -> break started
}
