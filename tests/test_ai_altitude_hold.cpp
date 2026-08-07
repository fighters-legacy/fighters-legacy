// SPDX-License-Identifier: GPL-3.0-or-later
//
// Closed-loop altitude-hold behaviour of the AI guidance (#1141): a LoiterController flown through
// the real FlightIntegrator, asserting that its altitude excursion stays bounded.
//
// This is the test that was missing. The AI's altitude→pitch→elevator chain was proportional-only,
// so a loitering entity oscillated by hundreds of metres and eventually flew itself into the ground:
// measured on fl-server with 8 well-separated entities spawned at an exact 500 m clearance, the
// population decayed 8 → 6 → 5 within 70 s while altitude swung between ~1050 m and ~519 m over
// 506–605 m of terrain. Nothing asserted altitude hold, so the load harness had been paying for it
// with 4000 m of spawn margin instead.

#include "ai/Guidance.h"
#include "ai/LoiterController.h"
#include "entity/EntityState.h"
#include "flight/BuiltinFlightModel.h"
#include "flight/FlightIntegrator.h"
#include "flight/Geodetic.h"
#include "flight/LocalFrame.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>

using namespace fl;

namespace {

constexpr double kR = kEarthRadiusM;
constexpr float kDt = 1.f / 60.f;

// Result of flying a controller for a while: what its altitude did.
struct FlightTrace {
    double startAltM{0.0};
    double minAltM{0.0};
    double maxAltM{0.0};
    double endAltM{0.0};
    double maxAbsErrM{0.0}; // worst deviation from the commanded altitude
    float maxAbsBankDeg{0.f};
};

FlightState levelState(double x, double altM, double z, float speedMps) {
    FlightState s;
    s.pos_world[0] = x;
    s.pos_world[1] = altM;
    s.pos_world[2] = z;
    s.vel_body[0] = speedMps;
    s.quat[3] = 1.f; // identity = wings level, nose along +X
    s.throttle_actual = 0.5f;
    return s;
}

// Fly `seconds` of a controller through the integrator, closing the loop exactly the way
// WorldBroadcaster does: sample() sees an EntityState built from the integrator's world state
// (position, orientation AND world velocity), and its ControlInput drives the next step.
FlightTrace flyLoiter(fl::ai::LoiterController& ctrl, FlightState init, double commandedAltM, int seconds) {
    FlightIntegrator integ(BuiltinFlightModel::get());
    integ.reset(init);
    PayloadEffect payload{};

    auto altOf = [&]() {
        const auto& p = integ.state().pos_world;
        return fl::localAltitude(glm::dvec3(p[0], p[1], p[2]), kR);
    };

    FlightTrace t{};
    t.startAltM = altOf();
    t.minAltM = t.startAltM;
    t.maxAltM = t.startAltM;

    for (int i = 0; i < 60 * seconds; ++i) {
        const FlightState& fs = integ.state();
        fl::EntityState es{};
        es.id = {1, 1};
        for (int k = 0; k < 3; ++k)
            es.transform.pos[k] = fs.pos_world[k];
        for (int k = 0; k < 4; ++k)
            es.transform.quat[k] = fs.quat[k];
        // World velocity, as EntityManager carries it — the vertical component is what the
        // damping term reads.
        const glm::quat q(fs.quat[3], fs.quat[0], fs.quat[1], fs.quat[2]);
        const glm::vec3 velWorld = q * glm::vec3(static_cast<float>(fs.vel_body[0]), static_cast<float>(fs.vel_body[1]),
                                                 static_cast<float>(fs.vel_body[2]));
        es.transform.vel[0] = velWorld.x;
        es.transform.vel[1] = velWorld.y;
        es.transform.vel[2] = velWorld.z;

        const ControlInput ci = ctrl.sample(es, static_cast<uint64_t>(i), kDt);
        integ.step(kDt, ci, payload);

        const double a = altOf();
        const auto& pw = integ.state().pos_world;
        t.maxAbsBankDeg = std::max(
            t.maxAbsBankDeg, std::abs(fl::bankOf(integ.state().quat, glm::dvec3(pw[0], pw[1], pw[2]), kR)) * 57.2958f);
        t.minAltM = std::min(t.minAltM, a);
        t.maxAltM = std::max(t.maxAltM, a);
        t.maxAbsErrM = std::max(t.maxAbsErrM, std::abs(a - commandedAltM));
    }
    t.endAltM = altOf();
    return t;
}

} // namespace

// ---------------------------------------------------------------------------
// The regression the issue is about
// ---------------------------------------------------------------------------

TEST_CASE("LoiterController holds its commanded altitude within a bounded excursion (#1141)", "[loiter][altitude]") {
    // Started ON the commanded altitude: this is the load-spawn case, where an entity is placed at
    // its loiter altitude and asked to stay there. Any excursion here is the controller's own doing.
    constexpr double kAlt = 1000.0;
    fl::ai::LoiterController ctrl(glm::dvec3(0.0, kAlt, 0.0), 3000.f, static_cast<float>(kAlt));
    const FlightTrace t = flyLoiter(ctrl, levelState(3000.0, kAlt, 0.0, 150.f), kAlt, 120);

    INFO("alt: start " << t.startAltM << " min " << t.minAltM << " max " << t.maxAltM << " end " << t.endAltM
                       << " maxAbsErr " << t.maxAbsErrM << " maxBank " << t.maxAbsBankDeg);
    // 150 m is the bound that matters operationally: it is what makes a spawn AGL mean something.
    // The pre-#1141 controller blew past 500 m here.
    CHECK(t.maxAbsErrM < 150.0);
    // Still flying, and still level-ish at the end — not merely bounded on the way to the ground.
    CHECK(std::abs(t.endAltM - kAlt) < 150.0);
    CHECK(std::isfinite(t.endAltM));
}

TEST_CASE("LoiterController does not descend into the ground at a modest AGL (#1141)", "[loiter][altitude]") {
    // The failure this reproduces: 500 m of clearance is not survivable if the controller oscillates
    // by more than 500 m. Terrain here is the datum, so "below 0" is the crash.
    constexpr double kAlt = 500.0;
    fl::ai::LoiterController ctrl(glm::dvec3(0.0, kAlt, 0.0), 3000.f, static_cast<float>(kAlt));
    const FlightTrace t = flyLoiter(ctrl, levelState(3000.0, kAlt, 0.0, 150.f), kAlt, 120);

    INFO("alt: start " << t.startAltM << " min " << t.minAltM << " max " << t.maxAltM);
    CHECK(t.minAltM > 100.0);
}

TEST_CASE("LoiterController climbs to a higher commanded altitude and settles (#1141)", "[loiter][altitude]") {
    // Damping must not cost it the ability to actually change altitude.
    constexpr double kStart = 1000.0;
    constexpr double kTarget = 2000.0;
    fl::ai::LoiterController ctrl(glm::dvec3(0.0, kTarget, 0.0), 3000.f, static_cast<float>(kTarget));
    const FlightTrace t = flyLoiter(ctrl, levelState(3000.0, kStart, 0.0, 150.f), kTarget, 120);

    INFO("alt: start " << t.startAltM << " min " << t.minAltM << " max " << t.maxAltM << " end " << t.endAltM);
    CHECK(t.endAltM > kStart + 500.0);            // it got most of the way there
    CHECK(std::abs(t.endAltM - kTarget) < 200.0); // and settled near it
    CHECK(t.maxAltM < kTarget + 400.0);           // without a wild overshoot
}

// ---------------------------------------------------------------------------
// The guidance primitive itself
// ---------------------------------------------------------------------------

TEST_CASE("elevatorForAltitudeHold commands nose-down when high, nose-up when low (#1141)", "[guidance]") {
    const float quat[4] = {0.f, 0.f, 0.f, 1.f}; // level, nose along +X
    const double pos[3] = {0.0, 1000.0, 0.0};
    const float levelVel[3] = {150.f, 0.f, 0.f};

    CHECK(fl::ai::elevatorForAltitudeHold(quat, pos, levelVel, /*targetAltM=*/1500.f, kR) > 0.f);
    CHECK(fl::ai::elevatorForAltitudeHold(quat, pos, levelVel, /*targetAltM=*/500.f, kR) < 0.f);

    // On altitude and level: nothing to do.
    CHECK(std::abs(fl::ai::elevatorForAltitudeHold(quat, pos, levelVel, 1000.f, kR)) < 0.05f);

    // Already climbing hard toward a target just above: the loop backs OFF rather than piling on —
    // the distinction the attitude-only form could not make.
    const float climbingVel[3] = {150.f, 30.f, 0.f};
    CHECK(fl::ai::elevatorForAltitudeHold(quat, pos, climbingVel, 1050.f, kR) <
          fl::ai::elevatorForAltitudeHold(quat, pos, levelVel, 1050.f, kR));
}

TEST_CASE("sideslipOf and rudderToCoordinate null a skid (#1141)", "[guidance]") {
    const float quat[4] = {0.f, 0.f, 0.f, 1.f}; // nose along +X, right wing along +Z
    // Flying forward with a component to the RIGHT of the nose: the nose is left of the flight path.
    const float skidRight[3] = {150.f, 0.f, 40.f};
    const float beta = fl::ai::sideslipOf(quat, skidRight);
    CHECK(beta > 0.f);
    CHECK(fl::ai::rudderToCoordinate(beta) > 0.f); // yaw right, toward the airflow

    const float skidLeft[3] = {150.f, 0.f, -40.f};
    CHECK(fl::ai::sideslipOf(quat, skidLeft) < 0.f);
    CHECK(fl::ai::rudderToCoordinate(fl::ai::sideslipOf(quat, skidLeft)) < 0.f);

    // Coordinated flight: no rudder.
    const float straight[3] = {150.f, 0.f, 0.f};
    CHECK(fl::ai::sideslipOf(quat, straight) == Catch::Approx(0.f).margin(1e-5f));
}

TEST_CASE("bankToTurnAileron closed on attitude stops at the commanded bank (#1141)", "[guidance]") {
    const double pos[3] = {0.0, 1000.0, 0.0};
    // Wings level, big heading error to the right: full-ish right aileron.
    const float level[4] = {0.f, 0.f, 0.f, 1.f};
    const float atLevel = fl::ai::bankToTurnAileron(level, pos, /*headingErrorRad=*/1.0f);
    CHECK(atLevel > 0.3f);

    // Already banked 45 deg right (rotation about the body forward axis) with the same heading
    // error: the command is spent, so the aileron backs off. The rate-only form would still be
    // commanding full deflection here, which is how the roll wound up to inverted.
    const float s = std::sin(0.3927f), c = std::cos(0.3927f); // 22.5 deg half-angle = 45 deg rotation
    const float banked[4] = {s, 0.f, 0.f, c};
    const float atBank = fl::ai::bankToTurnAileron(banked, pos, 1.0f);
    CHECK(atBank < atLevel);
    CHECK(std::abs(atBank) < 0.1f);
}

TEST_CASE("pitchErrorFromAlt still clamps the commanded pitch", "[guidance]") {
    const float quat[4] = {0.f, 0.f, 0.f, 1.f};
    const double pos[3] = {0.0, 1000.0, 0.0};
    // A huge error must not command more than the 30 deg limit either way.
    CHECK(fl::ai::pitchErrorFromAlt(quat, pos, 100000.f, kR) <= Catch::Approx(0.524f));
    CHECK(fl::ai::pitchErrorFromAlt(quat, pos, -100000.f, kR) >= Catch::Approx(-0.524f));
}
