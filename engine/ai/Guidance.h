// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "flight/LocalFrame.h" // enuBasis / radialUp / pitchOf / localAltitude (+ kEarthRadiusM via Geodetic.h)

#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <numbers>

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

    // Signed wrap of (targetBearing - fwdBearing) into [-pi, pi]. Positive = right.
    constexpr float kPi = std::numbers::pi_v<float>;
    float err = targetBearing - fwdBearing;
    while (err > kPi)
        err -= 2.f * kPi;
    while (err < -kPi)
        err += 2.f * kPi;
    return err;
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

// Angle of attack [rad] to command for a climb-rate error, bounded well short of the stall.
// The bound is the load-bearing part: an unbounded demand is what let the loop ask for 40 deg of AoA
// and get a mushing descent instead of a climb (#1141).
inline float aoaCommandFromClimbRate(float vsErrorMps, float maxAoaRad = 0.20f, float gain = 0.02f) {
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
inline float elevatorForAltitudeHold(const float quat[4], const double ownPos[3], const float velWorld[3],
                                     float targetAltM, double R = fl::kEarthRadiusM, float pitchRateRadS = 0.f) {
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
    const float pitchCmd =
        std::clamp(gamma + aoaCommandFromClimbRate(climbRateCommand(altErr) - vs), -kMaxPitchRad, kMaxPitchRad);
    const float pitchErr = pitchCmd - fl::pitchOf(quat, pos, R);
    return std::clamp(elevatorFromPitchError(pitchErr) - kPitchRateDamp * pitchRateRadS, -1.f, 1.f);
}

// Airspeed [m/s] at which a level turn of `radiusM` needs exactly `bankRad` of bank:
// v = sqrt(r * g * tan(bank)). Above it, the orbit cannot be flown at that bank without pulling
// more lift than the bank provides, so the aircraft trades altitude for the turn — which is the
// other half of the loiter's descent (#1141): a fixed throttle let it accelerate 150 -> 226 m/s,
// past the speed its own 45 deg bank limit could turn.
inline float turnSpeedForRadius(float radiusM, float bankRad) {
    constexpr float kG = 9.80665f;
    return std::sqrt(std::max(1.f, radiusM) * kG * std::tan(std::clamp(bankRad, 0.05f, 1.4f)));
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

// Yaw coordination: rudder proportional to aileron.
inline float coordinatedRudder(float aileronCmd, float k = 0.3f) {
    return std::clamp(aileronCmd * k, -1.f, 1.f);
}

} // namespace fl::ai
