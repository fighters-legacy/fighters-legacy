// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ai/TargetView.h"
#include "entity/EntityState.h"
#include "flight/BallisticLead.h"
#include "flight/LocalFrame.h" // localGravity -- the one "which way is down" (#1246)

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

namespace fl::ai {

// The gun lead preamble, in one place (#1265).
//
// Three fire controllers -- guns employment, the AAA batteries in SurfaceThreatControllers, and the
// turret gunner -- each opened with the same eight lines: unpack the shooter's and the contact's
// position and velocity out of their flat arrays, take the LOCAL gravity at the shooter, and hand
// all of it to computeBallisticLead. What they do with the answer differs completely (steer at it,
// return early, slew a turret onto it) and stays theirs; what they must NOT differ on is how the
// solution was set up, because a lead solved against a different "down" is a different lead.
struct GunLeadSolution {
    glm::dvec3 ownPos{};        // shooter position, unpacked
    glm::dvec3 tgtPos{};        // contact's last-known position
    BallisticLeadResult lead{}; // lead.valid == false = the round can never catch this target

    // Straight-line distance to the target. The trigger gates and the miss arc are both measured
    // against this, never against the distance to the lead point.
    [[nodiscard]] float rangeM() const noexcept {
        return static_cast<float>(glm::length(tgtPos - ownPos));
    }
};

[[nodiscard]] inline GunLeadSolution leadSolution(const fl::EntityState& shooter, const TargetView& tv,
                                                  float muzzleVelMps, double planetRadiusM) noexcept {
    GunLeadSolution s;
    s.ownPos = {shooter.transform.pos[0], shooter.transform.pos[1], shooter.transform.pos[2]};
    s.tgtPos = {tv.pos[0], tv.pos[1], tv.pos[2]};
    const glm::vec3 ownVel{shooter.transform.vel[0], shooter.transform.vel[1], shooter.transform.vel[2]};
    const glm::vec3 tgtVel{tv.vel[0], tv.vel[1], tv.vel[2]};
    // Local gravity from the planet radius the broadcaster wired -- the drop is along LOCAL down,
    // which is what keeps the pipper honest far from the world origin.
    const glm::vec3 gravity = fl::localGravity(s.ownPos, planetRadiusM);
    s.lead = computeBallisticLead(s.ownPos, ownVel, s.tgtPos, tgtVel, muzzleVelMps, gravity);
    return s;
}

} // namespace fl::ai
