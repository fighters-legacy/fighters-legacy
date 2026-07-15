// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <cmath>

namespace fl {

// The classic PN constant. 3–5 is the textbook band; 3.5 turns early without the tail-chase
// wobble higher gains buy.
inline constexpr float kProportionalNavGain = 3.5f;

// True proportional navigation (#627): a_cmd = N · Vc · (Ω × v̂), where Ω = (r × v_rel) / |r|² is
// the line-of-sight rotation rate and Vc the closing speed. Steering that nulls the LOS rate flies
// a collision course — the reason every real missile since the 1950s does this instead of pointing
// at the target (pure pursuit ends in a tail chase that spends the missile's whole energy
// advantage turning).
//
// Pure math on the caller's LAST-KNOWN target state (a seeker's contact, never ground truth),
// deterministic, no dice. `maxAccelMps2` clamps the command to the airframe's structural limit
// (performance.max_g); the closing speed is floored so an opening (fleeing) target still gets a
// finite correction toward the LOS instead of a sign-flipped command.
[[nodiscard]] inline glm::vec3 proportionalNavAccel(const glm::dvec3& missilePos, const glm::vec3& missileVel,
                                                    const glm::dvec3& targetPos, const glm::vec3& targetVel,
                                                    float navGain, float maxAccelMps2) noexcept {
    const glm::vec3 r = glm::vec3(targetPos - missilePos); // LOS; float is fine at weapon ranges
    const float r2 = glm::dot(r, r);
    const float speed = glm::length(missileVel);
    if (r2 < 1.f || speed < 1.f)
        return {};

    const glm::vec3 vRel = targetVel - missileVel;
    const glm::vec3 losRate = glm::cross(r, vRel) / r2;                       // Ω, rad/s
    const float closing = std::max(-glm::dot(r, vRel) / std::sqrt(r2), 10.f); // Vc floor 10 m/s
    const glm::vec3 vHat = missileVel / speed;

    glm::vec3 a = navGain * closing * glm::cross(losRate, vHat);
    const float aLen = glm::length(a);
    if (aLen > maxAccelMps2 && aLen > 0.f)
        a *= maxAccelMps2 / aLen;
    return a;
}

} // namespace fl
