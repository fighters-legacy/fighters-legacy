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
inline float pitchErrorFromAlt(const float quat[4], const double ownPos[3], float altErrorM,
                               double R = fl::kEarthRadiusM) {
    constexpr float kGain = 0.002f;
    constexpr float kMaxPitch = 0.524f; // 30 deg in radians
    const glm::dvec3 pos(ownPos[0], ownPos[1], ownPos[2]);
    const float curPitch = fl::pitchOf(quat, pos, R);
    const float desPitch = std::clamp(altErrorM * kGain, -kMaxPitch, kMaxPitch);
    return desPitch - curPitch;
}

// Map signed heading error to aileron command.
// Gain: 2/pi so 90 deg error -> full deflection.
inline float bankToTurnAileron(float headingErrorRad, float maxAileron = 1.f) {
    constexpr float kGain = 2.f / std::numbers::pi_v<float>;
    return std::clamp(headingErrorRad * kGain, -maxAileron, maxAileron);
}

// Yaw coordination: rudder proportional to aileron.
inline float coordinatedRudder(float aileronCmd, float k = 0.3f) {
    return std::clamp(aileronCmd * k, -1.f, 1.f);
}

// Elevator from pitch error.
// Gain: 2/pi so 90 deg pitch error -> full deflection.
inline float elevatorFromPitchError(float pitchErrorRad) {
    constexpr float kGain = 2.f / std::numbers::pi_v<float>;
    return std::clamp(pitchErrorRad * kGain, -1.f, 1.f);
}

} // namespace fl::ai
