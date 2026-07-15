// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/EntityId.h"

#include <cmath>

namespace fl {

// One detected entity-entity collision (#630). Canonical form: a.index < b.index, so a pair is
// recorded exactly once regardless of which entity's broadphase found it.
struct CollisionPair {
    EntityId a;
    EntityId b;
    float relativeSpeedMps{0.f}; // closing speed magnitude, drives the damage scale
};

// Do two spheres overlap? Pure geometry, double-precision positions (planet scale), so it is exact
// far from the world origin. The radii are the entities' collision radii
// (EntityDef::collisionRadiusM or the category default).
[[nodiscard]] inline bool spheresOverlap(const double a[3], float ra, const double b[3], float rb) noexcept {
    const double dx = a[0] - b[0];
    const double dy = a[1] - b[1];
    const double dz = a[2] - b[2];
    const double sumR = static_cast<double>(ra) + static_cast<double>(rb);
    return dx * dx + dy * dy + dz * dz <= sumR * sumR;
}

// Relative speed magnitude of two entities (m/s) — the |v_a − v_b| that scales collision damage,
// so a formation join-up brushing at 2 m/s is a scrape and a head-on merge at 600 m/s is fatal.
[[nodiscard]] inline float relativeSpeedMps(const float va[3], const float vb[3]) noexcept {
    const float dx = va[0] - vb[0];
    const float dy = va[1] - vb[1];
    const float dz = va[2] - vb[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Collision damage from a relative speed (#630): zero below a survivable brush threshold, then
// linear. A 60 m/s (~120 kt closure) contact is a firm bump; kCollisionDamagePerMps makes a
// several-hundred-knot merge lethal to a 100 hp airframe. Applied to BOTH entities.
inline constexpr float kCollisionFreeSpeedMps = 5.f;
inline constexpr float kCollisionDamagePerMps = 0.6f;

[[nodiscard]] inline float collisionDamage(float relativeSpeed) noexcept {
    return kCollisionDamagePerMps *
           (relativeSpeed > kCollisionFreeSpeedMps ? relativeSpeed - kCollisionFreeSpeedMps : 0.f);
}

} // namespace fl
