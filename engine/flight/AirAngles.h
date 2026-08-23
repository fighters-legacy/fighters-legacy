// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <cmath>

namespace fl {

// The air-data angles derived from an aircraft's attitude and velocity (#1251).
//
// Three copies of this existed: the HUD's, and the FFB and rumble paths inside HapticController --
// twice in the same file. They were not even spelled the same (`conjugate` in one, `inverse` in the
// others, which for a unit quaternion is the same rotation by way of a redundant divide), and each
// carried its own low-speed guard with its own threshold literal.
//
// NOT the same quantity as FlightIntegrator's alpha, which is deliberately computed against the
// RELATIVE wind -- it includes the wind vector, because that is what the airframe actually feels.
// These are display and cueing angles taken against velocity through the world. Keep them apart:
// folding them together would make the HUD read a number the pilot cannot see the cause of.

// Angle of attack [rad]: the pitch of the relative wind below the nose, so positive AoA means wind
// arriving from below. Body frame is nose = +X, up = +Y.
//
// Below `minSpeedMps` the angle is meaningless -- a nearly stationary aircraft has no meaningful
// relative wind, and atan2 of two near-zero components is numerical noise that would flap a stall
// cue on a parked aircraft -- so it reads zero rather than something arbitrary.
[[nodiscard]] inline float aoaRad(const glm::quat& orientation, const glm::vec3& velWorld,
                                  float minSpeedMps = 1.0f) noexcept {
    const glm::vec3 vBody = glm::conjugate(orientation) * velWorld;
    return (glm::length(vBody) > minSpeedMps) ? std::atan2(-vBody.y, vBody.x) : 0.0f;
}

} // namespace fl
