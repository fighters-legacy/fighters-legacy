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
    const auto model = BuiltinFlightModel::get();
    // Sync mass/fuel to the model like the production spawn (and tests/ai_flight_harness.h) — a
    // verbatim reset() otherwise flies FlightState's 10,000 kg default, not the builtin (#1336).
    init.fuel_kg = model->geometry.fuel_kg;
    init.mass_kg = model->geometry.mass_kg + init.fuel_kg;
    FlightIntegrator integ(model);
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
    // 180 s, re-measured for the builtin trainer (#1334): the guidance AoA bound trims ~4 deg of
    // alpha on it, which banked in the orbit yields a steady ~15 m/s climb — the 1000 m step plus
    // the capped-command settle tail no longer fits the UFO-era 120 s window.
    constexpr double kStart = 1000.0;
    constexpr double kTarget = 2000.0;
    fl::ai::LoiterController ctrl(glm::dvec3(0.0, kTarget, 0.0), 3000.f, static_cast<float>(kTarget));
    const FlightTrace t = flyLoiter(ctrl, levelState(3000.0, kStart, 0.0, 150.f), kTarget, 180);

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
//
// ⚑ [wing_sweep] IS PART OF THE AIRFRAME, not decoration (#1196). This block was missing, and the
// omission made the bench a DIFFERENT AEROPLANE from the one the numbers came from: without it the
// wing carries spread-planform lift at every Mach, while the real aircraft is at 55-67.5 deg of
// sweep anywhere near its cruise, where it has cl_scale 0.666 and k_scale 1.715 — a quarter less
// lift slope and 70% more induced drag. That raises trim alpha, which is precisely the quantity
// this file's whole subject (the AoA bound) is measured against. Restoring it deepened the measured
// default-bound sag from ~2,530 m to 4,229 m: the defect was worse than the bench could show.
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

[wing_sweep]
ref_sweep_deg   = 15.0
min_deg         = 15.0
max_deg         = 67.5
slew_rate_deg_s = 5.0

# The four published B-1B sweep detents laid onto the Mach numbers each is flown at.
[wing_sweep.schedule]
mach  = [0.00, 0.70, 0.85, 0.95, 1.05, 1.25]
sweep = [15.0, 15.0, 25.0, 55.0, 67.5, 67.5]

[wing_sweep.spread]
cl_scale  = 1.000
k_scale   = 1.000
cd0_delta = +0.0000

[wing_sweep.swept]
cl_scale  = 0.666
k_scale   = 1.715
cd0_delta = -0.0020

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
// Mid-mission fuel, set EXPLICITLY (the harness leaves a non-default state alone, #1336): these
// cases fly the bomber at 87,090 empty + 40,000 kg of gas = 127 t. Until #1336 the harness silently
// flew a 10,000 kg phantom wearing this airframe's aero, and at the fixture's true 207 t max-gross
// the sized-bound doctrine below does not hold at all — at 260 m/s / 8 km the trim alpha needed is
// ~7-9 deg while the cascade's 2/pi P-gain caps the equilibrium elevator near
// 0.64*(bound - trim alpha) of travel, which trims ~4 deg on this airframe AT ANY BOUND. The bound
// buys back authority the gearing has; it cannot mint more. Mid-mission weight is the regime where
// the #1186/#1196 mechanism (sized bound holds, default bound sags) is true of the real airframe.
fl::FlightState heavyCruiseState(double altM, float speedMps) {
    fl::FlightState s = fl::test::levelStateAt(0.0, altM, 0.0, speedMps);
    s.fuel_kg = 40000.f;
    s.mass_kg = 87090.f + s.fuel_kg;
    return s;
}

constexpr double kHeavyAltM = 8000.0;
constexpr float kHeavyCruiseMps =
    260.f; // its published M0.84 cruise at 8 km — the speed the real 127 t airframe trims at (#1336)

// ── the manoeuvring case (#1196) ────────────────────────────────────────────────────────────────
//
// AltitudeHoldOnly deliberately has nothing else in the loop, which makes it a clean read on the
// primitive and a poor model of the regime content actually flies. Two things it cannot show:
//
//  1. A HELD AIRSPEED. On a fixed throttle this bomber accelerates to 366-444 m/s at 8 km — M1.19
//     to M1.44 against its own declared max_mach of 1.25 — and the acceleration is what drops trim
//     alpha far enough to hand the loop its authority back. A bench whose pass depends on flying
//     outside the model's envelope is measuring the throttle, not the pitch law. A route-following
//     controller holds a cruise speed, so these cases do too.
//  2. TURNS. Rolling into a bank costs lift, and the aircraft has to find the extra somewhere.
//
// Everything here beyond the pitch axis is what any route-following controller already has: a bank
// held on attitude feedback (the #1143 law) and a rudder that coordinates it. Nothing added rescues
// the pitch axis; the manoeuvre is the only variable.
class HeavyManoeuvring final : public fl::IEntityController {
  public:
    // A route is a square circuit `legM` on a side. Passing bankRad instead flies one sustained
    // banked turn, which isolates "the aircraft is turning" from "the aircraft is navigating".
    static HeavyManoeuvring route(float targetAltM, float maxAoaRad, float targetSpeedMps, double legM) {
        HeavyManoeuvring c(targetAltM, maxAoaRad, targetSpeedMps);
        c.m_legM = legM;
        return c;
    }
    static HeavyManoeuvring sustainedBank(float targetAltM, float maxAoaRad, float targetSpeedMps, float bankRad) {
        HeavyManoeuvring c(targetAltM, maxAoaRad, targetSpeedMps);
        c.m_bankRad = bankRad;
        return c;
    }

    fl::ControlInput sample(const fl::EntityState& state, uint64_t, double dt, const fl::AiTickContext&) override {
        fl::ControlInput ctrl{};
        const glm::dvec3 pos(state.transform.pos[0], state.transform.pos[1], state.transform.pos[2]);
        const float spd = std::sqrt(state.transform.vel[0] * state.transform.vel[0] +
                                    state.transform.vel[1] * state.transform.vel[1] +
                                    state.transform.vel[2] * state.transform.vel[2]);
        ctrl.throttle = fl::ai::throttleForSpeed(spd, m_targetSpeedMps, kHeavyTrimThrottle);

        if (m_legM > 0.0) {
            // Chase the current waypoint; advance when inside the capture radius. The heading error
            // jumps at each corner, which is what makes the bank swing rather than settle.
            const glm::dvec3 wps[4] = {{m_legM, 0.0, 0.0}, {m_legM, 0.0, m_legM}, {0.0, 0.0, m_legM}, {0.0, 0.0, 0.0}};
            glm::dvec3 wp = wps[m_wp % 4];
            wp.y = pos.y; // the waypoint is a ground track; altitude is the pitch axis's business
            const double dx = wp.x - pos.x, dz = wp.z - pos.z;
            if (std::sqrt(dx * dx + dz * dz) < kWaypointCaptureM)
                ++m_wp;
            const double wpArr[3] = {wp.x, wp.y, wp.z};
            const float headErr = fl::ai::horizontalHeadingError(state.transform.quat, state.transform.pos, wpArr, kR);
            ctrl.aileron =
                fl::ai::bankToTurnAileron(state.transform.quat, state.transform.pos, headErr, kR, fl::ai::kNavBankRad);
        } else {
            ctrl.aileron = fl::ai::aileronFromBankError(m_bankRad - fl::bankOf(state.transform.quat, pos, kR));
        }
        ctrl.rudder = fl::ai::rudderToCoordinate(fl::ai::sideslipOf(state.transform.quat, state.transform.vel));

        // EntityState carries no body rates, so differentiate pitch across our own sample interval —
        // the same thing every station-keeping controller does.
        const float curPitch = fl::pitchOf(state.transform.quat, pos, kR);
        float pitchRate = 0.f;
        if (m_havePrevPitch && dt > 1e-6)
            pitchRate = static_cast<float>((curPitch - m_prevPitchRad) / dt);
        m_prevPitchRad = curPitch;
        m_havePrevPitch = true;

        ctrl.elevator = fl::ai::elevatorForAltitudeHold(state.transform.quat, state.transform.pos, state.transform.vel,
                                                        m_targetAltM, kR, pitchRate, m_maxAoaRad);
        return ctrl;
    }

  private:
    HeavyManoeuvring(float targetAltM, float maxAoaRad, float targetSpeedMps)
        : m_targetAltM(targetAltM), m_maxAoaRad(maxAoaRad), m_targetSpeedMps(targetSpeedMps) {}

    static constexpr float kHeavyTrimThrottle = 0.85f;
    static constexpr double kWaypointCaptureM = 4000.0; // a 207 t aircraft does not hit a point

    float m_targetAltM;
    float m_maxAoaRad;
    float m_targetSpeedMps;
    double m_legM{0.0};
    float m_bankRad{0.f};
    float m_prevPitchRad{0.f};
    bool m_havePrevPitch{false};
    int m_wp{0};
};

// The cruise these cases fly: M0.84 at 8 km, which is the bomber's own published cruise and well
// clear of its ~180 m/s 1 g stall there.
constexpr float kHeavyRouteMps = 260.f;
constexpr double kHeavyLegM = 80000.0;
constexpr int kHeavyRouteSeconds = 480;

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
    // Held, not merely still airborne. Measured excursion at the real 127 t is 146 m (#1336 — the
    // pre-#1336 "292 m" was the ten-tonne phantom); 400 m is the operational bound that makes a
    // spawn AGL mean something, the same standard #1141 set for the fighter.
    //
    // ⚑ THE FIXED THROTTLE IS STILL DOING PART OF THE WORK: over these 180 s the aircraft
    // accelerates to ~361 m/s (M1.17 at 8 km, near its declared max_mach 1.25), and the speed is
    // what keeps trim alpha low. That is the honest limit of a wings-level fixed-throttle bench and
    // the reason #1196 added the manoeuvring cases below, which hold a cruise speed instead.
    CHECK(t.minAltM > kHeavyAltM - 400.0);
    CHECK(std::abs(t.endAltM - kHeavyAltM) < 400.0);
}

TEST_CASE("the default AoA bound is fighter-sized, and a heavy aircraft sags kilometres on it (#1186)",
          "[guidance][altitude][heavy]") {
    // The reason the parameter exists, pinned so nobody "simplifies" it away. The same aircraft in
    // the same scenario on the fighter-sized default drops to ~3,772 m — a 4,229 m excursion, well
    // past the 150 m #1141 called a defect for the fighter, and fatal at any realistic AGL. It
    // eventually recovers here only because a fixed 0.85 throttle accelerates it to ~416 m/s (M1.35,
    // past its own max_mach), which drops trim alpha far enough to give the loop its authority back;
    // that rescue is the throttle's doing, not the altitude loop's.
    //
    // ⚑ The excursion was measured at 2,530 m before #1196 gave this model its [wing_sweep] block.
    // The defect was worse than the bench could show, because the bench was flying a fixed-geometry
    // aeroplane with the B-1B's mass.
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

TEST_CASE("elevatorForAltitudeHold holds a HEAVY aircraft through a route of hard turns (#1196)",
          "[guidance][altitude][heavy]") {
    // THE COVERAGE GAP #1196 IS ABOUT. #1186's cases fly wings level on a fixed throttle, which is
    // neither the regime content flies nor a condition this aircraft can legally sustain (see
    // HeavyManoeuvring's note on the runaway). Put the same airframe on a route with 45 deg-limited
    // turns, holding its published cruise.
    //
    // ⚑ Re-measured at the aircraft's real weight (#1336 — the pre-#1336 "105 m excursion" was the
    // ten-tonne phantom): at 127 t each hard turn entry costs a ~1.2 km transient before the loop
    // recovers it, and NO AoA bound does better, because past ~0.44 rad the cascade's 25-deg
    // attitude ceiling (kMaxPitchRad) becomes the binding cap on the equilibrium elevator (~0.22 of
    // travel — ~4 deg of trim alpha on this gearing) while a 1.41 g turn at cruise needs ~5. That
    // is the honest answer to the fork the issue posed ("hold it, or state plainly that it does
    // not"): the sized bound RECOVERS the turn transient and re-holds — measured min 6,782 m, end
    // within 200 m of the commanded 8,000 m — where the fighter default under the same turns goes
    // to within 400 m of the GROUND (the contrast case below). A heavy flying this guidance keeps
    // altitude between manoeuvres, not through them; mission content plans AGL accordingly.
    const auto model = std::make_shared<const fl::FlightModelData>(fl::parseFlightModel(kHeavyBomber));

    HeavyManoeuvring ctrl =
        HeavyManoeuvring::route(static_cast<float>(kHeavyAltM), /*maxAoaRad=*/0.65f, kHeavyRouteMps, kHeavyLegM);
    const fl::test::FlightTrace t =
        fl::test::flyController(ctrl, heavyCruiseState(kHeavyAltM, kHeavyRouteMps), kHeavyRouteSeconds, {}, {}, model);

    INFO("alt: min " << t.minAltM << " max " << t.maxAltM << " end " << t.endAltM << " bank " << t.maxAbsBankDeg
                     << " slip " << t.maxAbsSideslipDeg << " endSpeed " << t.endSpeedMps);
    CHECK_FALSE(t.crashed());
    CHECK(t.secondsFlown == Catch::Approx(static_cast<double>(kHeavyRouteSeconds)));
    // Bounded through the turn transients (measured 1,218 m at 127 t), and RE-HELD by the end —
    // the recovery is the claim, per the case comment; the fighter's 400 m standard belongs to the
    // fighter.
    CHECK(t.minAltM > kHeavyAltM - 1600.0);
    CHECK(std::abs(t.endAltM - kHeavyAltM) < 400.0);
    // It really did turn, and it turned coordinated — otherwise this is the wings-level case again
    // under a longer name.
    CHECK(t.maxAbsBankDeg > 40.f);
    CHECK(t.maxAbsSideslipDeg < 5.f);
}

TEST_CASE("the manoeuvring heavy case stays inside the model's own envelope (#1196)", "[guidance][altitude][heavy]") {
    // A HARNESS-INTEGRITY GUARD, not a behaviour claim. #1186's fixed-throttle bench accelerates
    // this bomber to 366-444 m/s at 8 km — M1.19 to M1.44 against a declared max_mach of 1.25 — and
    // the acceleration is what drops trim alpha far enough to rescue the loop. So "the primitive
    // holds" was partly "the throttle bailed it out", in a regime the aircraft is not cleared for.
    //
    // Speed is held here instead, and this pins that it stays held. Without it a future tweak to the
    // scenario could quietly go supersonic again and nobody would read the result differently.
    const auto model = std::make_shared<const fl::FlightModelData>(fl::parseFlightModel(kHeavyBomber));
    REQUIRE(model->limits.max_mach == Catch::Approx(1.25f));

    HeavyManoeuvring ctrl =
        HeavyManoeuvring::route(static_cast<float>(kHeavyAltM), /*maxAoaRad=*/0.65f, kHeavyRouteMps, kHeavyLegM);
    const fl::test::FlightTrace t =
        fl::test::flyController(ctrl, heavyCruiseState(kHeavyAltM, kHeavyRouteMps), kHeavyRouteSeconds, {}, {}, model);

    // Speed of sound at 8,000 m ISA. The aircraft ends near 300 m/s (M0.98) on the throttle loop.
    constexpr float kSpeedOfSoundAt8kmMps = 308.1f;
    const float endMach = t.endSpeedMps / kSpeedOfSoundAt8kmMps;
    INFO("end speed " << t.endSpeedMps << " m/s = M" << endMach);
    CHECK(endMach < model->limits.max_mach);
}

TEST_CASE("the fighter-sized default sags kilometres on the same route (#1196)", "[guidance][altitude][heavy]") {
    // The other half of the pair, and the reason the parameter exists at all. Identical scenario,
    // identical aircraft, the default bound: measured min 2,398 m — 5.6 km below the commanded
    // altitude, against the 105 m the sized bound holds to.
    //
    // Note this is DEEPER than the 2,530 m #1186's wings-level bench reports, for two compounding
    // reasons this file previously could not show: the model now carries its [wing_sweep] block, and
    // the aircraft is turning rather than accelerating away from the problem.
    const auto model = std::make_shared<const fl::FlightModelData>(fl::parseFlightModel(kHeavyBomber));

    HeavyManoeuvring ctrl =
        HeavyManoeuvring::route(static_cast<float>(kHeavyAltM), fl::ai::kDefaultMaxAoaRad, kHeavyRouteMps, kHeavyLegM);
    const fl::test::FlightTrace t =
        fl::test::flyController(ctrl, heavyCruiseState(kHeavyAltM, kHeavyRouteMps), kHeavyRouteSeconds, {}, {}, model);

    INFO("alt: min " << t.minAltM << " end " << t.endAltM << " endSpeed " << t.endSpeedMps);
    CHECK(t.minAltM < kHeavyAltM - 1500.0);
}

TEST_CASE("a sustained banked turn costs the sized bound almost nothing (#1196)", "[guidance][altitude][heavy]") {
    // Isolates "turning" from "navigating": one 35 deg bank held for the whole run, which is the
    // bank fl-base-pack's bomber-stream mission flies. Re-measured at real weight (#1336): the
    // sized bound gives up ~1.1 km entering the bank and then holds; the default still gives up
    // kilometres and never re-holds. (The pre-#1336 numbers here — 91 m vs 5.6 km, and the
    // 0/10/20/35-deg sweep table — were measured on the ten-tonne phantom and described it, not
    // this aircraft.)
    const auto model = std::make_shared<const fl::FlightModelData>(fl::parseFlightModel(kHeavyBomber));
    constexpr float kBank35Rad = 0.611f;

    HeavyManoeuvring sized = HeavyManoeuvring::sustainedBank(static_cast<float>(kHeavyAltM), /*maxAoaRad=*/0.65f,
                                                             kHeavyRouteMps, kBank35Rad);
    const fl::test::FlightTrace ts =
        fl::test::flyController(sized, heavyCruiseState(kHeavyAltM, kHeavyRouteMps), kHeavyRouteSeconds, {}, {}, model);
    INFO("sized: min " << ts.minAltM << " bank " << ts.maxAbsBankDeg);
    CHECK_FALSE(ts.crashed());
    // Measured 1,084 m of bank-entry transient at 127 t; the claim is bounded-and-held, not the
    // fighter's 400 m (see the route case above for why no bound can buy that here).
    CHECK(ts.minAltM > kHeavyAltM - 1600.0);
    CHECK(ts.maxAbsBankDeg > 30.f);

    HeavyManoeuvring dflt = HeavyManoeuvring::sustainedBank(static_cast<float>(kHeavyAltM), fl::ai::kDefaultMaxAoaRad,
                                                            kHeavyRouteMps, kBank35Rad);
    const fl::test::FlightTrace td =
        fl::test::flyController(dflt, heavyCruiseState(kHeavyAltM, kHeavyRouteMps), kHeavyRouteSeconds, {}, {}, model);
    INFO("default: min " << td.minAltM);
    CHECK(td.minAltM < kHeavyAltM - 1500.0);
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
