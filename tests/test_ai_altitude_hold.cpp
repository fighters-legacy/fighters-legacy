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

#include "ai_flight_harness.h"

#include "ai/Guidance.h"
#include "ai/LoiterController.h"
#include "entity/EntityState.h"
#include "flight/BuiltinFlightModel.h"
#include "flight/FlightIntegrator.h"
#include "flight/FlightModelParser.h"
#include "flight/Geodetic.h"
#include "flight/LocalFrame.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>
#include <memory>

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
// Heavy airframes: the AoA bound has to be sized to the aircraft (#1186)
// ---------------------------------------------------------------------------

namespace {

// A B-1B-class heavy bomber: ~207 t all-up on a 181 m^2 wing, with a pitch inertia of 1.06e7 kg m^2
// — about 150x the light fighter above. The numbers are fl-base-pack's B-1B, which is the aircraft
// that found this defect; what matters to the test is the CLASS (heavy, low stall margin, slow in
// pitch), not the identity.
const char* kHeavyBomber = R"(
[aircraft]
name         = "Test Heavy Bomber"
type         = "bomber"
engine_type  = "turbofan"
has_fbw      = false
cruise_alt_m = 12000.0

[flight_model]
mass_kg      = 87090.0
wing_area_m2 = 181.16
wingspan_m   = 41.758
mac_m        = 4.758
fuel_kg      = 120326.0
ixx_kg_m2    = 7451784.0
iyy_kg_m2    = 10571333.0
izz_kg_m2    = 13313039.0

[aero.cl_table]
alpha  = [-4.0, 0.0, 4.0, 8.0, 11.0, 13.0, 17.0]
mach   = [0.30, 0.60, 0.85, 0.95, 1.25]
values = [
    -0.2850, -0.3317, -0.3449, -0.3309, -0.2316,
     0.0600,  0.0600,  0.0600,  0.0600,  0.0600,
     0.4050,  0.4517,  0.4649,  0.4509,  0.3516,
     0.7501,  0.8434,  0.8698,  0.8417,  0.6431,
     1.0089,  1.1372,  1.1735,  1.1349,  0.8618,
     1.1814,  1.3330,  1.3760,  1.3303,  1.0076,
     0.9687,  1.0931,  1.1283,  1.0908,  0.8262,
]

[aero.drag_polar]
cd0           = 0.0175
k             = 0.0413
speedbrake_cd = 0.0400
gear_cd       = 0.0250

[aero.moments]
cm_alpha = -2.0631
cm_q     = -22.6828
cm_de    = -1.2696
cl_beta  = -0.2857
cl_p     = -0.6834
cl_da    =  0.3740
cn_beta  =  0.1543
cn_r     = -0.1565
cn_dr    = -0.0919

[aero.limits]
alpha_stall_deg  = 13.0
max_g_structural =  2.50
min_g_structural = -1.00
max_mach         =  1.25

[aero.controls]
max_elevator_deg = 25.0
max_aileron_deg  = 20.0
max_rudder_deg   = 25.0

[engine]
fuel_flow_idle_kg_s = 0.900
fuel_flow_mil_kg_s  = 4.910
fuel_flow_ab_kg_s   = 29.500
spool_time_s        = 5.500

[engine.mil_thrust]
mach   = [0.00, 0.60, 0.90, 1.25, 1.60]
alt_km = [0.00, 5.00, 11.00, 15.24, 18.29]
values = [
    309.40, 200.68, 110.27,  62.46,  41.50,
    322.39, 209.11, 114.90,  65.09,  43.25,
    366.48, 237.71, 130.61,  73.99,  49.16,
    428.01, 277.61, 152.54,  86.41,  57.41,
    374.65, 243.00, 133.52,  75.64,  50.26,
]

[engine.ab_thrust]
mach   = [0.00, 0.60, 0.90, 1.25, 1.60]
alt_km = [0.00, 5.00, 11.00, 15.24, 18.29]
values = [
    547.70, 355.24, 195.19, 110.57,  73.47,
    570.70, 370.16, 203.39, 115.22,  76.56,
    648.75, 420.79, 231.21, 130.97,  87.02,
    757.67, 491.43, 270.02, 152.96, 101.63,
    663.00, 430.00, 236.00, 133.00,  88.00,
]
)";

// Altitude hold and NOTHING else: wings level, fixed throttle, elevator straight off the primitive
// under test. No other loop can mask the result or rescue the aircraft.
class AltitudeHoldOnly final : public fl::IEntityController {
  public:
    AltitudeHoldOnly(float targetAltM, float throttle, float maxAoaRad)
        : m_targetAltM(targetAltM), m_throttle(throttle), m_maxAoaRad(maxAoaRad) {}

    fl::ControlInput sample(const fl::EntityState& state, uint64_t, double dt, const fl::AiTickContext&) override {
        fl::ControlInput ctrl{};
        ctrl.throttle = m_throttle;

        // EntityState carries no body rates, so differentiate pitch across our own sample interval —
        // the same thing every station-keeping controller does.
        const glm::dvec3 pos(state.transform.pos[0], state.transform.pos[1], state.transform.pos[2]);
        const float curPitch = fl::pitchOf(state.transform.quat, pos, kR);
        float pitchRate = 0.f;
        if (m_havePrevPitch && dt > 1e-6)
            pitchRate = static_cast<float>((curPitch - m_prevPitchRad) / dt);
        m_prevPitchRad = curPitch;
        m_havePrevPitch = true;

        ctrl.elevator = fl::ai::elevatorForAltitudeHold(state.transform.quat, state.transform.pos, state.transform.vel,
                                                        m_targetAltM, kR, pitchRate, m_maxAoaRad);
        m_lastElevator = ctrl.elevator;
        return ctrl;
    }

    [[nodiscard]] float lastElevator() const noexcept {
        return m_lastElevator;
    }

  private:
    float m_targetAltM;
    float m_throttle;
    float m_maxAoaRad;
    float m_prevPitchRad{0.f};
    bool m_havePrevPitch{false};
    float m_lastElevator{0.f};
};

// Level at 8 km and 200 m/s. THE CONDITION IS THE TEST: at 207 t and this altitude the wing needs
// CL ~ 1.03, which is alpha ~ 10 deg = 0.18 rad — within a whisker of the 0.20 rad default bound.
// That is where the defect lives, and why it is invisible at fighter loadings. (The stall here is
// ~176 m/s, so this is a legitimately flyable cruise, not a trick condition.)
fl::FlightState heavyCruiseState(double altM, float speedMps) {
    return fl::test::levelStateAt(0.0, altM, 0.0, speedMps);
}

constexpr double kHeavyAltM = 8000.0;
constexpr float kHeavyCruiseMps = 200.f;

} // namespace

TEST_CASE("elevatorForAltitudeHold holds a HEAVY aircraft when the AoA bound is sized to it (#1186)",
          "[guidance][altitude][heavy]") {
    // THE REGRESSION. In steady flight the cascade commands pitch = gamma + bounded AoA, so the
    // elevator it can ask for settles near (2/pi) * (bound - trim alpha): as a heavy aircraft's trim
    // alpha approaches the bound, the authority available to the loop goes to ZERO no matter how far
    // below its altitude the aircraft is. Sized to the airframe, the same primitive, the same
    // aircraft and the same scenario hold altitude.
    const auto model = std::make_shared<const fl::FlightModelData>(fl::parseFlightModel(kHeavyBomber));

    AltitudeHoldOnly ctrl(static_cast<float>(kHeavyAltM), /*throttle=*/0.85f, /*maxAoaRad=*/0.45f);
    const fl::test::FlightTrace t =
        fl::test::flyController(ctrl, heavyCruiseState(kHeavyAltM, kHeavyCruiseMps), 180, {}, {}, model);

    INFO("alt: start " << t.startAltM << " min " << t.minAltM << " max " << t.maxAltM << " end " << t.endAltM
                       << " endSpeed " << t.endSpeedMps << " crashTime " << t.crashTimeS);
    CHECK_FALSE(t.crashed());
    CHECK(t.secondsFlown == Catch::Approx(180.0));
    // Held, not merely still airborne. Measured excursion is ~222 m; 400 m is the operational bound
    // that makes a spawn AGL mean something, the same standard #1141 set for the fighter.
    CHECK(t.minAltM > kHeavyAltM - 400.0);
    CHECK(std::abs(t.endAltM - kHeavyAltM) < 400.0);
}

TEST_CASE("the default AoA bound is fighter-sized, and a heavy aircraft sags kilometres on it (#1186)",
          "[guidance][altitude][heavy]") {
    // The reason the parameter exists, pinned so nobody "simplifies" it away. The same aircraft in
    // the same scenario on the fighter-sized default drops to ~5,470 m — a 2,530 m excursion, an
    // order of magnitude past the 150 m #1141 called a defect for the fighter, and fatal at any
    // realistic AGL. It eventually recovers here only because a fixed 0.85 throttle accelerates it
    // to ~370 m/s, which drops trim alpha far enough to give the loop its authority back; that
    // rescue is the throttle's doing, not the altitude loop's.
    //
    // This pins the DEFAULT's behaviour, not a wish. A change that makes one bound serve both
    // classes should delete this test deliberately, having read why it was here.
    const auto model = std::make_shared<const fl::FlightModelData>(fl::parseFlightModel(kHeavyBomber));

    AltitudeHoldOnly ctrl(static_cast<float>(kHeavyAltM), /*throttle=*/0.85f,
                          /*maxAoaRad=*/fl::ai::kDefaultMaxAoaRad);
    const fl::test::FlightTrace t =
        fl::test::flyController(ctrl, heavyCruiseState(kHeavyAltM, kHeavyCruiseMps), 180, {}, {}, model);

    INFO("alt: start " << t.startAltM << " min " << t.minAltM << " end " << t.endAltM << " endSpeed " << t.endSpeedMps);
    CHECK(t.minAltM < kHeavyAltM - 1500.0); // and the sized-bound case above proves it need not
}

TEST_CASE("widening the AoA bound does not change what a fighter does (#1186)", "[guidance][altitude]") {
    // Every existing caller passes no bound. The default must be the pre-#1186 value exactly, and
    // the 6-argument call must be bit-identical to the 7-argument call at that value — otherwise
    // this change silently re-tunes every station-keeping controller in the engine.
    const float quat[4] = {0.f, 0.f, 0.f, 1.f};
    const double pos[3] = {0.0, 1000.0, 0.0};
    const float vel[3] = {150.f, -5.f, 0.f};

    CHECK(fl::ai::kDefaultMaxAoaRad == Catch::Approx(0.20f));
    CHECK(fl::ai::elevatorForAltitudeHold(quat, pos, vel, 1500.f, kR, 0.f) ==
          fl::ai::elevatorForAltitudeHold(quat, pos, vel, 1500.f, kR, 0.f, fl::ai::kDefaultMaxAoaRad));

    // And the bound is live in both directions: a wider one asks for more, a narrower one for less.
    const float wide = fl::ai::elevatorForAltitudeHold(quat, pos, vel, 1500.f, kR, 0.f, 0.45f);
    const float narrow = fl::ai::elevatorForAltitudeHold(quat, pos, vel, 1500.f, kR, 0.f, 0.05f);
    CHECK(wide > narrow);
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
