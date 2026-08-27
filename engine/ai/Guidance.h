// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/AiTickContext.h" // the per-tick world view — the terrain floor reads its ground reference
#include "flight/AeroForces.h"    // ControlInput, for the shared steering tail
#include "flight/LocalFrame.h"    // enuBasis / radialUp / pitchOf / localAltitude (+ kEarthRadiusM via Geodetic.h)
#include "math/Angles.h"          // wrapPi
#include "math/Units.h"           // kG0

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <numbers>
#include <optional>

namespace fl::ai {

// Extract world-frame forward vector (+X body axis) from EntityTransform quat [x,y,z,w].
inline glm::vec3 bodyForward(const float quat[4]) {
    // EntityTransform stores [x,y,z,w]; GLM quat constructor takes (w,x,y,z).
    glm::quat q(quat[3], quat[0], quat[1], quat[2]);
    return q * glm::vec3(1.f, 0.f, 0.f);
}

// Signed horizontal bearing error [rad] from the entity's current heading to the target,
// measured in the LOCAL tangent (ENU) plane at ownPos on a planet of radius R (m). Both the
// forward axis and the target direction are projected into the same local basis, so the result
// is frame-independent (well defined even at the pole/origin where the ENU axes are singular).
// Positive = turn right (target is clockwise of the nose); negative = turn left.
// Returns 0 when the horizontal separation in the tangent plane is < 0.1 m.
// Near the world origin (R large) this reduces to the old world-XZ heading error.
inline float horizontalHeadingError(const float quat[4], const double ownPos[3], const double targetPos[3],
                                    double R = fl::kEarthRadiusM) {
    const glm::dvec3 pos(ownPos[0], ownPos[1], ownPos[2]);
    const glm::dvec3 tgt(targetPos[0], targetPos[1], targetPos[2]);
    const glm::mat3 enu = fl::enuBasis(pos, R);
    const glm::dvec3 east(enu[0]);
    const glm::dvec3 north(enu[1]);

    // Target direction projected onto the local tangent plane.
    const glm::dvec3 d = tgt - pos;
    const double te = glm::dot(d, east);
    const double tn = glm::dot(d, north);
    if (te * te + tn * tn < 0.01) // < 0.1 m horizontal
        return 0.f;
    const float targetBearing = std::atan2(static_cast<float>(te), static_cast<float>(tn));

    // Forward axis projected onto the same tangent plane.
    const glm::vec3 fwd = bodyForward(quat);
    const float fe = glm::dot(fwd, glm::vec3(east));
    const float fn = glm::dot(fwd, glm::vec3(north));
    const float fwdBearing = std::atan2(fe, fn);

    // Signed error, wrapped into [-pi, pi]. Positive = right.
    return wrapPi(targetBearing - fwdBearing);
}

// Signed pitch error [rad] needed to null a radial altitude error of altErrorM (target radial
// altitude minus own radial altitude) at ownPos on a planet of radius R (m). The current pitch is
// taken relative to the LOCAL horizon (pitchOf), so it is correct anywhere on the sphere.
// Gain 0.002 rad/m on the desired pitch, clamped to +/-30 deg. Positive = nose-up command.
//
// ⚠ This maps altitude straight onto a pitch ATTITUDE, with no feedback on whether the aircraft is
// actually climbing. It is fine for a player autopilot in level flight (#640, which additionally
// damps on the pitch rate) and a trap for an AI holding a turn: measured on a loiter, the aircraft
// settled nose-up 30 deg with a -2.8 deg flight path, descending 11 m/s while this loop reported
// itself satisfied (#1141). For anything that must hold an altitude while manoeuvring, use
// elevatorForAltitudeHold below, which closes on climb rate.
inline float pitchErrorFromAlt(const float quat[4], const double ownPos[3], float altErrorM,
                               double R = fl::kEarthRadiusM) {
    constexpr float kGain = 0.002f;
    constexpr float kMaxPitch = 0.524f; // 30 deg in radians
    const glm::dvec3 pos(ownPos[0], ownPos[1], ownPos[2]);
    const float curPitch = fl::pitchOf(quat, pos, R);
    const float desPitch = std::clamp(altErrorM * kGain, -kMaxPitch, kMaxPitch);
    return desPitch - curPitch;
}

// Radial climb rate [m/s] of an entity with world velocity `velWorld` at `pos` (positive = climbing).
// The component of velocity along the local up axis — the quantity "vertical speed" means anywhere
// on a sphere, and what pitchErrorFromAlt damps on.
inline float radialClimbRate(const float velWorld[3], const double ownPos[3], double R = fl::kEarthRadiusM) {
    const glm::dvec3 pos(ownPos[0], ownPos[1], ownPos[2]);
    const glm::vec3 up = fl::radialUp(pos, R);
    return velWorld[0] * up.x + velWorld[1] * up.y + velWorld[2] * up.z;
}

// Elevator from pitch error.
// Gain: 2/pi so 90 deg pitch error -> full deflection.
inline float elevatorFromPitchError(float pitchErrorRad) {
    constexpr float kGain = 2.f / std::numbers::pi_v<float>;
    return std::clamp(pitchErrorRad * kGain, -1.f, 1.f);
}

// ---------------------------------------------------------------------------
// Altitude hold closed on FLIGHT PATH, not attitude (#1141)
// ---------------------------------------------------------------------------
//
// pitchErrorFromAlt maps an altitude error straight onto a pitch ATTITUDE. That is fine while the
// aircraft can convert pitch into climb, and silently catastrophic when it cannot: measured on a
// banked loiter, the aircraft settled nose-up 30 deg with a flight path of **-2.8 deg** — descending
// 11 m/s while the loop, satisfied that the nose was where it asked for, commanded a neutral
// elevator all the way into the ground. Attitude is not the controlled variable; climb rate is.
//
// The cascade below is altitude -> climb rate -> pitch attitude -> elevator, each stage clamped, so
// the outer loop keeps demanding more until the aircraft is ACTUALLY climbing, while the attitude
// clamp still prevents an absurd nose position.

// Desired climb rate [m/s] for an altitude error. 0.2 1/s = "close the gap in ~5 s", bounded.
inline float climbRateCommand(float altErrorM, float maxVsMps = 25.f, float gain = 0.2f) {
    return std::clamp(altErrorM * gain, -maxVsMps, maxVsMps);
}

// The default AoA bound is sized for a FIGHTER, and the sizing matters more than it looks (#1186):
// through the cascade below the bound caps not just the commanded AoA but the equilibrium ELEVATOR,
// at roughly (2/pi) * (bound - trim alpha). A heavy aircraft needs far more trim elevator per unit
// of AoA — the B-1B needs ~0.3 of travel to trim the alpha level flight requires, against the ~0.13
// this default can ever ask for — so a heavy caller MUST widen it or the aircraft descends into
// terrain at a tenth of available travel while the loop reports itself busy.
inline constexpr float kDefaultMaxAoaRad = 0.20f;

// Angle of attack [rad] to command for a climb-rate error, bounded well short of the stall.
// The bound is the load-bearing part: an unbounded demand is what let the loop ask for 40 deg of AoA
// and get a mushing descent instead of a climb (#1141).
inline float aoaCommandFromClimbRate(float vsErrorMps, float maxAoaRad = kDefaultMaxAoaRad, float gain = 0.02f) {
    return std::clamp(vsErrorMps * gain, -maxAoaRad, maxAoaRad);
}

// The whole cascade: elevator command holding `targetAltM` given the current state.
//
// `pitchRateRadS` is the inner-loop damping, and it is not optional in practice. Elevator commands a
// pitch ACCELERATION, so pitch is a double integrator and proportional feedback alone is marginally
// stable: a small persistent command winds the nose up degree by degree until the aircraft is
// mushing at 40 deg of angle of attack, pointing up and falling. The player autopilot has carried
// the same term since #640 (kPitchRateDamp on omega[2]); an AI controller cannot see body rates
// through EntityState, so it differentiates pitch across its own sample interval instead.
//
// `maxAoaRad` sizes the loop to the AIRFRAME (#1186). The default serves a fighter; a heavy
// aircraft needs a wider bound, because the bound caps the equilibrium elevator at roughly
// (2/pi) * (maxAoaRad - trim alpha) and a large pitch inertia buys nothing back — measured on the
// B-1B, the default held the elevator at ~0.10 while the sink rate grew -15 to -236 m/s, a tenth
// of the travel the trim actually needed. Same shape as bankToTurnAileron's maxBank: the primitive
// cannot know the airframe, so the caller says. The 25 deg attitude ceiling below still binds
// whatever the caller asks for.
inline float elevatorForAltitudeHold(const float quat[4], const double ownPos[3], const float velWorld[3],
                                     float targetAltM, double R = fl::kEarthRadiusM, float pitchRateRadS = 0.f,
                                     float maxAoaRad = kDefaultMaxAoaRad) {
    constexpr float kPitchRateDamp = 0.8f; // s — matches the player autopilot (#640)
    constexpr float kMaxPitchRad = 0.44f;  // 25 deg: a hard ceiling on the attitude, whatever else
    const glm::dvec3 pos(ownPos[0], ownPos[1], ownPos[2]);
    const float altErr = targetAltM - static_cast<float>(fl::localAltitude(pos, R));
    const float vs = radialClimbRate(velWorld, ownPos, R);
    const float speed = std::sqrt(velWorld[0] * velWorld[0] + velWorld[1] * velWorld[1] + velWorld[2] * velWorld[2]);
    // The commanded attitude is the CURRENT FLIGHT PATH plus a bounded angle of attack, not an
    // attitude picked from the altitude error alone. That is what makes a mush recoverable: an
    // aircraft descending at -6 deg gets a pitch command near -6 deg + the AoA margin, which is
    // nose-DOWN from the 20 deg it was holding, so the wing unloads, flies again, and climbs. The
    // old form saw only "we are low" and commanded more nose-up into the stall.
    const float gamma = std::asin(std::clamp(vs / std::max(1.f, speed), -1.f, 1.f));
    const float pitchCmd = std::clamp(gamma + aoaCommandFromClimbRate(climbRateCommand(altErr) - vs, maxAoaRad),
                                      -kMaxPitchRad, kMaxPitchRad);
    const float pitchErr = pitchCmd - fl::pitchOf(quat, pos, R);
    return std::clamp(elevatorFromPitchError(pitchErr) - kPitchRateDamp * pitchRateRadS, -1.f, 1.f);
}

// Airspeed [m/s] at which a level turn of `radiusM` needs exactly `bankRad` of bank:
// v = sqrt(r * g * tan(bank)). Above it, the orbit cannot be flown at that bank without pulling
// more lift than the bank provides, so the aircraft trades altitude for the turn — which is the
// other half of the loiter's descent (#1141): a fixed throttle let it accelerate 150 -> 226 m/s,
// past the speed its own 45 deg bank limit could turn.
inline float turnSpeedForRadius(float radiusM, float bankRad) {
    return std::sqrt(std::max(1.f, radiusM) * kG0<float> * std::tan(std::clamp(bankRad, 0.05f, 1.4f)));
}

// The inverse (#1340): the tightest level circle an aircraft flying at `speedMps` can close within
// `bankRad` of bank -- r = v^2 / (g tan(bank)).
//
// It exists because the two halves of the orbit are set by DIFFERENT authorities. The radius comes
// from the mission author; the speed comes from what the airframe will actually hold, which no
// controller can know in advance -- a mission can ask for a 700 m racetrack, and the trainer's
// throttle law will not fly it slower than ~100 m/s, which needs 1,250 m. Asking for the tighter
// circle anyway does not produce it; it produces a permanently saturated turn. This is the same
// honesty turnSpeedForRadius already encodes for the speed half of the problem.
[[nodiscard]] inline float flyableOrbitRadius(float speedMps, float bankRad) {
    const float v = std::max(0.f, speedMps);
    return v * v / (kG0<float> * std::tan(std::clamp(bankRad, 0.05f, 1.4f)));
}

// Throttle to hold a target airspeed, trimmed around `trimThrottle`. Proportional and clamped —
// enough to stop an unbounded accelerate-or-decay, not an autothrottle.
inline float throttleForSpeed(float speedMps, float targetSpeedMps, float trimThrottle, float gain = 0.01f) {
    return std::clamp(trimThrottle + (targetSpeedMps - speedMps) * gain, 0.05f, 1.f);
}

// Map signed heading error to aileron command.
// Gain: 2/pi so 90 deg error -> full deflection.
//
// ⚠ ATTITUDE-FREE, and that is a trap for anything that turns for a sustained period (#1141).
// Aileron commands a roll RATE, not a bank angle, so a heading error that persists — which is the
// normal condition on a circular orbit, where the target bearing keeps moving — holds the aileron
// deflected and the bank winds up without limit. Measured on a 120 s loiter: the aircraft reached
// **179.8 deg of bank** (fully inverted) and 83 deg of pitch, at which point the altitude loop's
// nose-up command pulled it into the ground.
//
// Use it only where the heading error is transient and something else bounds the roll. For anything
// holding a turn, use the attitude-closed overload below.
inline float bankToTurnAileron(float headingErrorRad, float maxAileron = 1.f) {
    constexpr float kGain = 2.f / std::numbers::pi_v<float>;
    return std::clamp(headingErrorRad * kGain, -maxAileron, maxAileron);
}

// Bank angle [rad] to command for a heading error (positive = right wing down = turn right).
// 30 deg of heading error asks for the full default 45 deg of bank — enough for a brisk turn, and
// well short of the attitudes where the pitch and roll axes start fighting each other.
inline float bankCommandFromHeading(float headingErrorRad, float maxBankRad = 0.785f) {
    constexpr float kGain = 1.5f;
    return std::clamp(headingErrorRad * kGain, -maxBankRad, maxBankRad);
}

// Aileron from a bank-ANGLE error. Gain 2/pi: 90 deg of bank error -> full deflection.
inline float aileronFromBankError(float bankErrorRad, float maxAileron = 1.f) {
    constexpr float kGain = 2.f / std::numbers::pi_v<float>;
    return std::clamp(bankErrorRad * kGain, -maxAileron, maxAileron);
}

// Bank limits by role (#1143). A turn law with attitude feedback needs a ceiling, and the right
// ceiling is not the same for a wingman holding station and a fighter tracking guns. Manoeuvre
// controllers (break turn, yo-yos, evade) deliberately use NO limit — rolling past knife-edge is the
// manoeuvre, and tests/test_ai_turn_law.cpp pins that they still can.
inline constexpr float kNavBankRad = 0.785f;       // 45 deg — waypoint navigation, swarming
inline constexpr float kFormationBankRad = 1.047f; // 60 deg — following a manoeuvring lead
inline constexpr float kCombatBankRad = 1.396f;    // 80 deg — pursuit and gun tracking
inline constexpr float kApproachBankRad = 0.436f;  // 25 deg — approach and climbout, close to the ground

// Heading error -> aileron, closed on the entity's CURRENT bank relative to the local horizon.
// This is the attitude feedback the rate-only form above is missing: the roll stops at the
// commanded bank instead of continuing to wind up for as long as the heading error survives.
// Correct anywhere on the sphere (bankOf is measured against the radial up).
inline float bankToTurnAileron(const float quat[4], const double ownPos[3], float headingErrorRad,
                               double R = fl::kEarthRadiusM, float maxBankRad = 0.785f) {
    const glm::dvec3 pos(ownPos[0], ownPos[1], ownPos[2]);
    const float cmdBank = bankCommandFromHeading(headingErrorRad, maxBankRad);
    const float curBank = fl::bankOf(quat, pos, R);
    return aileronFromBankError(cmdBank - curBank);
}

// Sideslip angle [rad] from the world velocity and the orientation: positive = the airflow is
// coming from the aircraft's right, i.e. the nose is left of the flight path. Observable from
// EntityState alone (world velocity rotated back into the body frame), which is what makes it usable
// by an AI controller.
inline float sideslipOf(const float quat[4], const float velWorld[3]) {
    const glm::quat q(quat[3], quat[0], quat[1], quat[2]);
    const glm::vec3 vb = glm::inverse(q) * glm::vec3(velWorld[0], velWorld[1], velWorld[2]);
    if (std::abs(vb.x) < 1.f)
        return 0.f;
    return std::atan2(vb.z, std::abs(vb.x)); // body +Z is right
}

// Rudder to null a sideslip. THIS is turn coordination; the aileron-proportional form below is not.
//
// In a steady turn the aileron sits near zero — the bank is already established — so a rudder tied
// to it commands nothing at all, and nothing else in the loop points the nose along the flight path.
// Measured on a loitering entity: the body velocity reached [160, 6, 92] m/s, a **30 deg sideslip**,
// flying half sideways with the wing at -2 deg AoA and sinking (#1141). An airframe with weathercock
// stability would have sorted itself out; the builtin arcade model has cn_beta = 0 and never does.
inline float rudderToCoordinate(float sideslipRad, float gain = 2.f) {
    return std::clamp(sideslipRad * gain, -1.f, 1.f);
}

// Where to aim when pursuing a moving target: the target's position shifted along its OWN velocity
// by the estimated time to intercept (#1259).
//
// The sign of `gain` is the whole difference between lead and lag pursuit, which is why
// LeadPursuitController and LagPursuitController were the same file twice -- identical down to the
// time-to-intercept clamp, differing in one operator and the member's name. Positive leads (aim
// ahead, the guns-tracking geometry); negative lags (aim behind, to stay inside a turn without
// overshooting). Zero aims straight at the target, which is what a nonsensical gain also does.
//
// Closing speed is floored at 10 m/s and the time to intercept capped at 30 s: an opening or
// co-speed target otherwise divides toward infinity and throws the aim point off the planet.
inline void pursuitOffsetPoint(double out[3], const double ownPos[3], const float ownVel[3], const double tgtPos[3],
                               const float tgtVel[3], float gain) {
    out[0] = tgtPos[0];
    out[1] = tgtPos[1];
    out[2] = tgtPos[2];

    // Float arithmetic is sufficient for kinematics at ACM scales.
    const float dx = static_cast<float>(tgtPos[0] - ownPos[0]);
    const float dy = static_cast<float>(tgtPos[1] - ownPos[1]);
    const float dz = static_cast<float>(tgtPos[2] - ownPos[2]);
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (gain == 0.f || dist <= 0.1f)
        return;

    // Closing speed = -d/dt(|r|) = -dot(r_hat, relVel).
    const float rvx = tgtVel[0] - ownVel[0];
    const float rvy = tgtVel[1] - ownVel[1];
    const float rvz = tgtVel[2] - ownVel[2];
    const float closingSpeed = -(dx * rvx + dy * rvy + dz * rvz) / dist;
    const float ttoIntercept = std::min(dist / std::max(closingSpeed, 10.f), 30.f);

    out[0] += static_cast<double>(tgtVel[0]) * ttoIntercept * gain;
    out[1] += static_cast<double>(tgtVel[1]) * ttoIntercept * gain;
    out[2] += static_cast<double>(tgtVel[2]) * ttoIntercept * gain;
}

// The whole steering tail for "fly toward this world point", in one place (#1259).
//
// Seven controllers -- pursuit, lead and lag pursuit, guns employment, waypoint, swarm and
// formation -- each spelled this block out, differing only in the bank limit and one role-specific
// comment. Each also dragged its own copy of the four-line #1143 rationale, which is why that
// rationale now lives here, once:
//
//   The aileron is a bank-ANGLE command closed on the current bank, not the old rate-only law.
//   The rate-only law wound these controllers to 179.8 deg of bank and 89 deg of sideslip within
//   90 s of a heading error they could not null, and flew them into the ground. The rudder nulls
//   the measured SIDESLIP rather than mirroring the aileron, because in a steady turn the aileron
//   is back at zero and nothing else points the nose along the flight path.
//
// `maxBankRad` stays a caller argument -- it is the one thing these seven genuinely disagree about,
// and the role constants below say why each picked what it did.
//
// Throttle is deliberately NOT set here: some callers hold a fixed throttle and others close a
// speed loop, and that is a real difference rather than a copy.
inline void steerTowardPoint(ControlInput& ctrl, const float quat[4], const double ownPos[3], const float ownVel[3],
                             const double tgtPos[3], double R, float maxBankRad) {
    const glm::dvec3 own(ownPos[0], ownPos[1], ownPos[2]);
    const glm::dvec3 tgt(tgtPos[0], tgtPos[1], tgtPos[2]);

    const float headErr = horizontalHeadingError(quat, ownPos, tgtPos, R);
    const float altErr = static_cast<float>(fl::localAltitude(tgt, R) - fl::localAltitude(own, R));
    const float pitchErr = pitchErrorFromAlt(quat, ownPos, altErr, R);

    ctrl.aileron = bankToTurnAileron(quat, ownPos, headErr, R, maxBankRad);
    ctrl.rudder = rudderToCoordinate(sideslipOf(quat, ownVel));
    ctrl.elevator = elevatorFromPitchError(pitchErr);
}

// ---------------------------------------------------------------------------
// The terrain floor (#1352)
// ---------------------------------------------------------------------------

// Hard-deck heights above TERRAIN, by role. These are floors, not altitude policy: a mission that
// commands a 300 m orbit still gets a 300 m orbit. What it no longer gets is a controller that keeps
// flying its geometry while the ground comes up to meet it.
//
// Two numbers because the two families need different room. A MANOEUVRING controller arrives at the
// deck nose-down and fast, and the pull costs height before it buys any: measured in #1339, a
// trainer recovering from a 25 deg dive at 175 m/s with the altitude cascade doing the work hit the
// terrain at ~100 m AGL, and the firm pull below stopped it ~250 m higher. 300 m is that recovery
// plus margin. A NAVIGATION controller is already holding a commanded altitude and only needs a
// genuine "you are about to hit the ground" backstop — set low so a deliberately low-level route or
// a 100 m patrol orbit is still flown, not fought.
inline constexpr float kCombatDeckAglM = 300.f; // pursuit, guns, evade, break, the yo-yos, Immelmann, Split-S
inline constexpr float kNavDeckAglM = 60.f;     // waypoint, swarm, formation, the loiters

// Aileron per rad of bank error while recovering. Firm — the roll is the part that has to finish
// before the pull can start.
inline constexpr float kDeckRollGain = 2.f;

// Speed-scaled recovery pull, as v^2 * n: 5300 holds the pull near 6 g at sea level on the builtin
// trainer, whose ~0.8 rad of trimmed alpha per rad of elevator makes a FIXED large pull both over
// the +-7 g structural limit above ~140 m/s and past the 15 deg stall below it. The clamps keep a
// slow recovery honest at one end and under the stall at the other. Shared verbatim with the
// builtin Lua scripts' pull_out (engine/script/BuiltinAiScripts.h) — one law, two seams.
inline constexpr float kDeckPullVsq = 5300.f;
inline constexpr float kDeckPullMin = 0.10f;
inline constexpr float kDeckPullMax = 0.30f;

// Height above terrain [m], or nullopt when this tick evaluated no ground reference.
//
// The null is normative and must NOT be read as sea level: over the shipped sandbox's ~545 m terrain
// that mistake is 545 m of imaginary air, which is the whole of #1352.
[[nodiscard]] inline std::optional<float> aglOf(const double ownPos[3], const fl::AiTickContext& ctx,
                                                double R = fl::kEarthRadiusM) {
    if (!ctx.groundElevM)
        return std::nullopt;
    const glm::dvec3 pos(ownPos[0], ownPos[1], ownPos[2]);
    return static_cast<float>(fl::localAltitude(pos, R) - static_cast<double>(*ctx.groundElevM));
}

// TERRAIN DOES NOT NEGOTIATE. Fills `ctrl` with a recovery and returns true when the entity is below
// its hard deck; returns false and leaves `ctrl` alone otherwise.
//
// Call it FIRST, before the controller's own geometry, and return immediately when it fires — the
// point is that it outranks the manoeuvre. That includes the manoeuvres whose whole shape is
// nose-down (Split-S, the low yo-yo): abandoning one at the deck is the correct outcome, and the
// state machine driving it re-enters on the next tick.
//
// Recover in order: WINGS, then pull (#1141). A firm pull while rolled past vertical is a split-S
// into the terrain, so the lift vector is levelled first and the pull is gated on it pointing up.
// Full throttle throughout — but note that thrust is not what gets a mushing aircraft off the back
// of the drag curve, which is the separate defect in #1353.
//
// A null ground reference disables the floor rather than inventing one. That keeps a
// default-constructed AiTickContext behaving exactly as it did before this existed, which is the
// contract every other field in that struct already has.
[[nodiscard]] inline bool terrainFloorRecovery(ControlInput& ctrl, const float quat[4], const double ownPos[3],
                                               const float velWorld[3], const fl::AiTickContext& ctx,
                                               float floorAglM = kCombatDeckAglM, double R = fl::kEarthRadiusM) {
    const std::optional<float> agl = aglOf(ownPos, ctx, R);
    if (!agl || *agl >= floorAglM)
        return false;

    const glm::dvec3 pos(ownPos[0], ownPos[1], ownPos[2]);
    const float bank = fl::bankOf(quat, pos, R);
    const float spdSq = velWorld[0] * velWorld[0] + velWorld[1] * velWorld[1] + velWorld[2] * velWorld[2];

    ctrl = ControlInput{};
    ctrl.aileron = std::clamp(-kDeckRollGain * bank, -1.f, 1.f);
    ctrl.rudder = rudderToCoordinate(sideslipOf(quat, velWorld));
    ctrl.elevator =
        std::cos(bank) > 0.5f ? std::clamp(kDeckPullVsq / std::max(spdSq, 1.f), kDeckPullMin, kDeckPullMax) : 0.f;
    ctrl.throttle = 1.f;
    return true;
}

// Yaw coordination: rudder proportional to aileron.
inline float coordinatedRudder(float aileronCmd, float k = 0.3f) {
    return std::clamp(aileronCmd * k, -1.f, 1.f);
}

// Which way round the circle. Lives here rather than in LoiterController.h because the orbit body
// below is shared by the fixed-centre and target-following loiters (#1265).
enum class LoiterDir : uint8_t { Clockwise, CounterClockwise };

// Bank limit for an ORBIT (#1141). Numerically equal to kNavBankRad and deliberately NOT merged
// into it: this one also sets the airspeed the orbit is flyable at (turnSpeedForRadius), so it is
// pinned by the geometry rather than by the "what does this role fly like" judgement the role
// constants above encode. 45 deg turns briskly and stays clear of the attitudes where the pitch and
// roll axes fight each other.
inline constexpr float kOrbitBankRad = 0.785f;

// How far around the circle the orbit aim point sits, in radians of orbit angle (#1340). The aim
// point is on the orbit itself, so this sets how much of the radial error reaches the heading error:
// too small and the capture is sluggish, too large and the aircraft cuts the corner and scallops.
// 0.6 rad (~34 deg) settles the shipped 3 km default within a few percent of the commanded radius.
inline constexpr float kOrbitLeadRad = 0.6f;

// How far below its commanded altitude an orbit may sink before the speed hold loses the throttle
// to the altitude hold (#1340). Full trim throttle is restored at this deficit and blended in below
// it, so a momentary dip costs a little thrust and a real sink costs the deceleration.
inline constexpr float kOrbitAltPriorityM = 100.f;

// Pitch rate by backward difference across sample intervals (#1141/#1265).
//
// EntityState carries no body angular rates, so every controller with a damped pitch loop has to
// differentiate pitch itself -- and three of them (loiter, dynamic loiter, evade) wrote the same
// five lines. The state that makes it correct is the `have` flag: the FIRST sample has no previous
// pitch, and must damp on nothing rather than on a difference against a default-constructed zero,
// which at a 30 deg initial pitch is a fictitious 30 rad/s kick into the elevator.
struct PitchRateEstimator {
    float prevRad{0.f};
    bool have{false};

    [[nodiscard]] float step(float pitchRad, double dt) noexcept {
        float rate = 0.f;
        if (have && dt > 1e-6)
            rate = static_cast<float>((pitchRad - prevRad) / dt);
        prevRad = pitchRad;
        have = true;
        return rate;
    }
};

// Everything a circular orbit needs that does not depend on WHERE the centre came from.
struct OrbitParams {
    glm::dvec3 centre{};       // world position the orbit is flown around
    float radiusM{3000.f};     // orbit radius
    float targetAltM{600.f};   // local altitude to hold
    float targetSpeedMps{0.f}; // airspeed the orbit is flyable at (see orbitSpeedForRadius)
    float trimThrottle{0.65f}; // the speed hold trims around this
    LoiterDir dir{LoiterDir::Clockwise};
};

// How far under the bank limit an orbit is flown, as a fraction of the speed that limit could turn
// the radius at (#1141). A turn held exactly at the limit has nothing left for the altitude loop --
// it is paid for with altitude, which was half of the original loiter descent. Factored out of
// orbitSpeedForRadius (#1340) because the radius half of the same geometry needs the SAME margin:
// picking the speed a margin under the limit, then sizing the circle to the limit, would have put
// the aircraft right back on a saturated bank whenever the airframe declined to fly that slowly.
inline constexpr float kOrbitSpeedMargin = 0.85f;

// The airspeed an orbit of this radius is flyable at. Held a margin under the speed the bank limit
// can turn the radius at, so the turn is not permanently saturated (#1141).
[[nodiscard]] inline float orbitSpeedForRadius(float radiusM) {
    return std::clamp(kOrbitSpeedMargin * turnSpeedForRadius(radiusM, kOrbitBankRad), 60.f, 300.f);
}

// The tightest circle THIS orbit will actually ask for at `speedMps` -- the geometric minimum
// (flyableOrbitRadius) opened up by the same margin orbitSpeedForRadius holds on the speed, so a
// clamped orbit is flown at ~36 deg of bank rather than pinned against the 45 deg limit (#1340).
// Measured on the builtin trainer at a 700 m command: at the bare geometric minimum it settled at
// 1,300 m but bled 1,060 m of altitude over 300 s holding 44 deg of bank; with the margin it settles
// wider and holds its altitude.
[[nodiscard]] inline float orbitRadiusFloor(float speedMps) {
    return flyableOrbitRadius(speedMps / kOrbitSpeedMargin, kOrbitBankRad);
}

// Fly a circle around `p.centre`, in one place (#1265).
//
// LoiterController and DynamicLoiterController were this function twice. Their real difference is
// where the centre and the hold altitude come from -- a fixed point and a fixed altitude, versus a
// live target's position and the altitude of that target -- and that difference is now the caller's,
// which is where it belongs. Everything below it is the same geometry and the same three loops, and
// the #1141 rationale for each of those loops is recorded on the primitives they call.
//
// Returns false when the entity is within 1 m of the centre horizontally, where the tangent is
// undefined: the throttle is still set, the surfaces are left neutral, and the caller returns.
inline bool orbitSteer(ControlInput& ctrl, const float quat[4], const double ownPos[3], const float velWorld[3],
                       const OrbitParams& p, double R, float pitchRate) {
    const glm::dvec3 pos(ownPos[0], ownPos[1], ownPos[2]);
    const float speed = std::sqrt(velWorld[0] * velWorld[0] + velWorld[1] * velWorld[1] + velWorld[2] * velWorld[2]);

    // Speed hold around the trim throttle (#1141). A FIXED throttle let the aircraft accelerate
    // 150 -> 226 m/s while descending, past the speed its own bank limit can turn a 3 km circle at
    // (v = sqrt(r g tan(bank))) -- at which point the orbit can only be flown by trading altitude,
    // and it does, all the way to the ground. Holding the flyable speed removes the energy half of
    // the problem; the flight-path altitude loop below removes the other half.
    //
    // ALTITUDE OUTRANKS THAT SPEED TARGET (#1340). The speed hold is a P-loop with no equilibrium
    // when its target is out of reach: a 700 m orbit asks the trainer for 70 m/s, which it cannot
    // fly, so the throttle pins at idle and the aircraft pays the difference in altitude -- a steady
    // 1.9 m/s sink, 570 m gone in 300 s, while the orbit geometry itself looked healthy. An aircraft
    // gliding at idle is not loitering. So while the orbit is below its commanded altitude the
    // throttle may not be pulled under its trim setting: the speed hold gets what is left after the
    // altitude is paid for, which is the right priority for a station-keeping orbit and the
    // assumption the altitude cascade below already makes about its own authority.
    const float altErrM = p.targetAltM - static_cast<float>(fl::localAltitude(pos, R));
    const float trimFloor = p.trimThrottle * std::clamp(altErrM / kOrbitAltPriorityM, 0.f, 1.f);
    ctrl.throttle = std::max(throttleForSpeed(speed, p.targetSpeedMps, p.trimThrottle), trimFloor);

    // Vector from the entity to the centre in the XZ plane.
    const float tx = static_cast<float>(p.centre.x - ownPos[0]);
    const float tz = static_cast<float>(p.centre.z - ownPos[2]);
    const float tLen = std::sqrt(tx * tx + tz * tz);
    if (tLen < 1.f)
        return false; // at the centre: hold throttle, neutral surfaces

    const float nx = tx / tLen;
    const float nz = tz / tLen;

    // The circle we can actually fly (#1340). The commanded radius is a FLOOR, not a promise: below
    // the radius the current speed can be turned at within the bank limit, the guidance would spend
    // the whole orbit at a saturated bank and still never close the circle. Deriving it from the
    // MEASURED speed each tick is the only honest source -- a controller sees an EntityState, never
    // a flight model, so it cannot know how slowly this airframe will agree to fly. The speed hold
    // above is already asking for the slower speed; this is what the geometry does meanwhile.
    const float effRadiusM = std::max(p.radiusM, orbitRadiusFloor(speed));

    // Aim at a point ON that circle, an angular lead ahead of our own bearing from the centre --
    // NOT at a lookahead along the tangent from wherever we happen to be (#1340). The tangent form
    // had no radial term at all: flying perpendicular to the radius is an equilibrium at ANY radius,
    // so every metre of outward drift the bank-limited turn lagged by was permanent, and the orbit
    // grew for as long as the run lasted. Measured on the shipped 3 km default: 3.0 -> 8.7 km in
    // 120 s, monotonically, with nothing in the mission or the log to say the circle had moved.
    // An aim point on the circle carries the radial error into the heading error, which is what
    // turns "I am outside my orbit" into a command to turn back onto it.
    const float rx = -nx; // unit radial, centre -> own
    const float rz = -nz;
    const float lead = p.dir == LoiterDir::Clockwise ? kOrbitLeadRad : -kOrbitLeadRad;
    const float cl = std::cos(lead);
    const float sl = std::sin(lead);
    const double lookahead[3] = {
        p.centre.x + static_cast<double>(effRadiusM) * static_cast<double>(rx * cl - rz * sl),
        p.centre.y,
        p.centre.z + static_cast<double>(effRadiusM) * static_cast<double>(rx * sl + rz * cl),
    };

    const float headErr = horizontalHeadingError(quat, ownPos, lookahead, R);
    // Bank-ANGLE command closed on the current bank (#1141). An orbit never runs out of heading
    // error -- the target bearing keeps moving around the circle -- so the rate-only form held the
    // aileron deflected and rolled the aircraft steadily past vertical to inverted, after which
    // "pull up" flies it into the ground.
    ctrl.aileron = bankToTurnAileron(quat, ownPos, headErr, R, kOrbitBankRad);
    // Rudder nulls the SIDESLIP, not the aileron (#1141): in a steady turn the aileron is near zero,
    // so the aileron-proportional form commands nothing and the nose never follows the flight path.
    ctrl.rudder = rudderToCoordinate(sideslipOf(quat, velWorld));
    // Altitude hold closed on CLIMB RATE, not pitch attitude (#1141). The attitude form left the
    // aircraft sitting nose-up 30 deg with a -2.8 deg flight path -- descending 11 m/s while the
    // loop, satisfied the nose was where it asked for, commanded a neutral elevator into the ground.
    ctrl.elevator = elevatorForAltitudeHold(quat, ownPos, velWorld, p.targetAltM, R, pitchRate);
    return true;
}

} // namespace fl::ai
