// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace fl::ai {

// Extract world-frame forward vector (+X body axis) from EntityTransform quat [x,y,z,w].
inline glm::vec3 bodyForward(const float quat[4]) {
    // EntityTransform stores [x,y,z,w]; GLM quat constructor takes (w,x,y,z).
    glm::quat q(quat[3], quat[0], quat[1], quat[2]);
    return q * glm::vec3(1.f, 0.f, 0.f);
}

// Signed horizontal angle [rad] from current heading to target bearing in XZ plane.
// Positive = turn right (+Z side in Y-up right-hand system with forward=+X).
// Returns 0 when horizontal distance to target is < 0.1 m.
inline float horizontalHeadingError(const float quat[4], const double ownPos[3], const double targetPos[3]) {
    float dx = static_cast<float>(targetPos[0] - ownPos[0]);
    float dz = static_cast<float>(targetPos[2] - ownPos[2]);
    float dist = std::sqrt(dx * dx + dz * dz);
    if (dist < 0.1f)
        return 0.f;

    glm::vec3 fwd = bodyForward(quat);
    glm::vec2 fwdXZ = glm::vec2(fwd.x, fwd.z);
    glm::vec2 tgtXZ = glm::vec2(dx / dist, dz / dist);

    // Cross product Y component in XZ plane: positive = target to the right.
    float crossY = fwdXZ.x * tgtXZ.y - fwdXZ.y * tgtXZ.x;
    float dot = glm::dot(fwdXZ, tgtXZ);
    return std::atan2(crossY, dot);
}

// Signed pitch error [rad] needed to reach targetAltM.
// Gain: 0.002 rad/m, clamped to +/-30 deg (0.524 rad).
// Positive = nose-up command needed.
inline float pitchErrorFromAlt(const float quat[4], float altErrorM) {
    constexpr float kGain = 0.002f;
    constexpr float kMaxPitch = 0.524f; // 30 deg in radians
    glm::vec3 fwd = bodyForward(quat);
    float curPitch = std::asin(std::clamp(fwd.y, -1.f, 1.f));
    float desPitch = std::clamp(altErrorM * kGain, -kMaxPitch, kMaxPitch);
    return desPitch - curPitch;
}

// Map signed heading error to aileron command.
// Gain: 2/pi so 90 deg error -> full deflection.
inline float bankToTurnAileron(float headingErrorRad, float maxAileron = 1.f) {
    constexpr float kGain = 2.f / 3.14159265f;
    return std::clamp(headingErrorRad * kGain, -maxAileron, maxAileron);
}

// Yaw coordination: rudder proportional to aileron.
inline float coordinatedRudder(float aileronCmd, float k = 0.3f) {
    return std::clamp(aileronCmd * k, -1.f, 1.f);
}

// Elevator from pitch error.
// Gain: 2/pi so 90 deg pitch error -> full deflection.
inline float elevatorFromPitchError(float pitchErrorRad) {
    constexpr float kGain = 2.f / 3.14159265f;
    return std::clamp(pitchErrorRad * kGain, -1.f, 1.f);
}

} // namespace fl::ai
