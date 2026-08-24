// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <cmath>

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

// The trigger-discipline rule the three gun fire controllers share (#1265).
//
// "Predicted miss" is the angular offset between where the gun actually POINTS and where the lead
// solution says it should, projected to an arc at the target's range: a 1 deg error at 500 m is an
// 8.7 m miss, at 2 km a 35 m one. Fire when that arc is inside the round's lethal radius.
//
// GunsEmploymentController, the AAA controllers in SurfaceThreatControllers and TurretGunnerController
// each wrote this out, differing only in what supplies the bore -- the nose for the two that point
// the whole aircraft, seat.turret.boreWorld for the one that slews a turret. That is a parameter,
// not three rules, and three copies is three chances for one weapon class to become quietly more
// trigger-happy than the others.
//
// Both directions must already be unit vectors; the dot is clamped because a rounding excursion past
// +-1 makes acos return NaN, and a NaN miss compares false and silently holds fire forever.
[[nodiscard]] inline bool withinLethalMiss(const glm::vec3& boreDir, const glm::vec3& aimDir, float rangeM,
                                           float lethalRadiusM) noexcept {
    const float cosOff = std::clamp(glm::dot(boreDir, aimDir), -1.f, 1.f);
    const float missM = std::acos(cosOff) * rangeM; // small-angle arc at the target's range
    return missM <= lethalRadiusM;
}

// Where an unpowered store released RIGHT NOW would land (#629).
struct CcipResult {
    glm::dvec3 impact{};
    float timeOfFallS{0.f};
    bool valid{false}; // false = never reached the ground inside maxFallTimeS
};

// Continuously-computed impact point (#629): forward-integrates the SAME point-mass model
// ProjectileSystem flies — gravity, plus drag decaying the velocity toward the air mass
// (`dragDecayPerS`, the ProjectileSystem::kCoastDecayPerS coefficient), at the sim's 60 Hz step —
// so the predicted impact and the actual bomb cannot disagree by more than integration phase.
//
// `heightAboveGround` returns the store's height above the terrain at a world position (the caller
// composes IGravityField::geodeticAltitude with its ground query); the fall ends when it reaches
// zero. Pure and deterministic; consumed by AI air-to-ground employment and by tests — the HUD
// pipper rendering rides the HUD-redesign work.
template <typename HeightAboveGroundFn>
[[nodiscard]] inline CcipResult
computeCcip(const glm::dvec3& releasePos, const glm::vec3& releaseVel, const glm::vec3& windMps, float dragDecayPerS,
            const glm::vec3& gravityAccel, HeightAboveGroundFn&& heightAboveGround, float maxFallTimeS = 120.f) {
    CcipResult r;
    constexpr float dt = 1.f / 60.f;
    glm::dvec3 pos = releasePos;
    glm::vec3 vel = releaseVel;
    for (float t = 0.f; t < maxFallTimeS; t += dt) {
        vel += (gravityAccel - (vel - windMps) * dragDecayPerS) * dt;
        pos += glm::dvec3(vel) * static_cast<double>(dt);
        if (heightAboveGround(pos) <= 0.0) {
            r.impact = pos;
            r.timeOfFallS = t + dt;
            r.valid = true;
            return r;
        }
    }
    return r;
}

} // namespace fl
