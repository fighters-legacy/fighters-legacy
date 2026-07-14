// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

namespace fl {

// Where to POINT an unguided gun so the round and the target arrive at the same place (#462).
struct BallisticLeadResult {
    glm::dvec3 aimPoint{}; // world-space point to put the gunsight pipper on
    float timeOfFlightS{0.f};
    bool valid{false}; // false = the round can never catch this target
};

// Ballistic lead by Picard iteration (#462): start with the time of flight to the target's CURRENT
// position, advance the target by its velocity over that time, correct for gravity drop, and
// re-derive the time of flight toward the new aim point — two rounds is within centimetres at
// gun ranges (the mapping is a strong contraction because target speed << round speed).
//
//   - The round inherits the SHOOTER's velocity: its speed toward the aim point is
//     |shooterVel + dir · muzzleVel|, which is why a tail chase shoots flat and a head-on shot
//     arrives early.
//   - Gravity drop is 0.5 · g · t² along the LOCAL gravity vector, which the caller supplies
//     (IGravityField::accelWorld at the shooter) — that is what makes the math planet-aware
//     without this header knowing what a planet is.
//   - Drag is deliberately ignored: at the 1–2 km ranges where guns kill, drop and lead dominate
//     and a drag model would imply a fidelity the hitscan resolver does not have either.
//
// Pure, deterministic, allocation-free. `muzzleVelMps` must be positive.
[[nodiscard]] inline BallisticLeadResult computeBallisticLead(const glm::dvec3& shooterPos, const glm::vec3& shooterVel,
                                                              const glm::dvec3& targetPos, const glm::vec3& targetVel,
                                                              float muzzleVelMps, const glm::vec3& gravityAccel,
                                                              int iterations = 2) noexcept {
    BallisticLeadResult r;
    if (muzzleVelMps <= 0.f)
        return r;

    glm::dvec3 aim = targetPos;
    float tof = 0.f;
    for (int i = 0; i <= iterations; ++i) {
        const glm::vec3 rel = glm::vec3(aim - shooterPos);
        const float dist = glm::length(rel);
        if (dist < 1.f) {
            r.aimPoint = aim;
            r.timeOfFlightS = 0.f;
            r.valid = true;
            return r;
        }
        const glm::vec3 dir = rel / dist;
        const float roundSpeed = glm::length(shooterVel + dir * muzzleVelMps);
        // The round must actually CLOSE: if the target opens faster than the round flies, there is
        // no lead angle, only a wasted burst.
        const float closure = roundSpeed - glm::dot(targetVel, dir);
        if (closure < 10.f)
            return r; // invalid
        tof = dist / roundSpeed;
        // Predicted target position at impact, raised by the round's gravity drop (aiming ABOVE
        // the target by the drop puts the falling round on it).
        aim = targetPos + glm::dvec3(targetVel) * static_cast<double>(tof) -
              glm::dvec3(gravityAccel) * (0.5 * static_cast<double>(tof) * static_cast<double>(tof));
    }
    r.aimPoint = aim;
    r.timeOfFlightS = tof;
    r.valid = true;
    return r;
}

} // namespace fl
