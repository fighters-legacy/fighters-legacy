// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Local-level (tangent-plane) frame utilities for a spherical planet (#470, part of the
// spherical-Earth epic #468). The nav/display/AI code historically hardcoded world-Y as "up" and
// world-XZ as "horizontal", which only holds near the world origin. These helpers give the true
// local up and an East/North/Up basis at any point, so the radial physics floor (#477) and the
// local-level navigation/HUD (#478/#479) work far from the origin.
//
// Conventions (consistent with engine/flight/Geodetic.h):
//   * Planet centre at world {0, -R, 0}; the world origin {0,0,0} is the north pole.
//   * The planet's polar axis (centre -> north pole) is world +Y.
//   * Up  = radial direction from the centre.
//     East = normalize(polarAxis x Up)   (horizontal, direction of increasing longitude)
//     North = Up x East                  (right-handed: East x North = Up)
//   * At a pole Up is parallel to the polar axis, so East/North are singular; we fall back to a
//     deterministic axis-aligned basis (East = +X, North = Up x East). At the north pole this
//     yields East=+X, North=-Z, Up=+Y — a valid right-handed frame anchored to the world axes.
//
// Header-only, pure math, no new link deps.

#include "flight/Geodetic.h"
#include "math/Units.h"

#include <glm/geometric.hpp> // normalize, cross, dot, length
#include <glm/gtc/quaternion.hpp>
#include <glm/mat3x3.hpp>
#include <glm/vec3.hpp>

#include <cmath>

namespace fl {

// Planet centre in world space (Geodetic.h convention).
[[nodiscard]] inline glm::dvec3 planetCentre(double R) noexcept {
    return {0.0, -R, 0.0};
}

// Local "up": unit radial direction from the planet centre to `pos`.
[[nodiscard]] inline glm::vec3 radialUp(glm::dvec3 pos, double R) noexcept {
    return glm::vec3(glm::normalize(pos - planetCentre(R)));
}

// Gravity at `pos`: standard gravity down the local radial. Three ballistic solvers — the turret
// gunner, the guns-employment controller and the surface-threat controllers — each spelled this
// expression out, and the combat HUD spelled it a fourth way round, which is a lot of chances to
// disagree about which way is down.
[[nodiscard]] inline glm::vec3 localGravity(glm::dvec3 pos, double R) noexcept {
    return -radialUp(pos, R) * kG0<float>;
}

// East/North/Up orthonormal basis at `pos` as a mat3 with columns (East, North, Up).
// Right-handed: East x North = Up.
[[nodiscard]] inline glm::mat3 enuBasis(glm::dvec3 pos, double R) noexcept {
    const glm::dvec3 up = glm::normalize(pos - planetCentre(R));
    const glm::dvec3 polar{0.0, 1.0, 0.0}; // centre -> north pole
    glm::dvec3 east = glm::cross(polar, up);
    const double e2 = glm::dot(east, east);
    if (e2 < 1e-18)
        east = glm::dvec3{1.0, 0.0, 0.0}; // pole singularity: deterministic fallback
    else
        east /= std::sqrt(e2);
    const glm::dvec3 north = glm::cross(up, east); // already unit (up ⟂ east)
    return glm::mat3(glm::vec3(east), glm::vec3(north), glm::vec3(up));
}

// Radial MSL altitude (m) of `pos` — the local "up" distance above the sphere.
[[nodiscard]] inline double localAltitude(glm::dvec3 pos, double R) noexcept {
    return geodeticAltitude(pos.x, pos.y, pos.z, R);
}

// Compass bearing (rad) from `pos` toward `target` in the local tangent plane at `pos`.
// 0 = North, +pi/2 = East (clockwise, standard aviation convention). Returns 0 when the
// horizontal separation is negligible.
[[nodiscard]] inline float headingTo(glm::dvec3 pos, glm::dvec3 target, double R) noexcept {
    const glm::mat3 enu = enuBasis(pos, R);
    const glm::dvec3 d = target - pos;
    const double e = glm::dot(d, glm::dvec3(enu[0])); // east component
    const double n = glm::dot(d, glm::dvec3(enu[1])); // north component
    if (e * e + n * n < 1e-6)
        return 0.f;
    return static_cast<float>(std::atan2(e, n));
}

// Pitch angle (rad) of the entity's forward (+X body) axis relative to the local horizon at `pos`.
// Positive = nose above the horizon. quat is EntityTransform order [x, y, z, w].
[[nodiscard]] inline float pitchOf(const float quat[4], glm::dvec3 pos, double R) noexcept {
    const glm::quat q(quat[3], quat[0], quat[1], quat[2]);
    const glm::vec3 forward = q * glm::vec3(1.f, 0.f, 0.f);
    const glm::vec3 up = radialUp(pos, R);
    float s = glm::dot(glm::normalize(forward), up);
    s = s > 1.f ? 1.f : (s < -1.f ? -1.f : s); // clamp for asin domain
    return std::asin(s);
}

// Compass heading (rad) of the entity's forward (+X body) axis in the local tangent plane at `pos`.
// 0 = North, +pi/2 = East (clockwise, standard aviation convention). Returns 0 when the forward
// axis is (near) vertical, leaving heading undefined. quat is EntityTransform order [x, y, z, w].
[[nodiscard]] inline float headingOf(const float quat[4], glm::dvec3 pos, double R) noexcept {
    const glm::quat q(quat[3], quat[0], quat[1], quat[2]);
    const glm::vec3 forward = q * glm::vec3(1.f, 0.f, 0.f);
    const glm::mat3 enu = enuBasis(pos, R);
    const float e = glm::dot(forward, enu[0]); // east component
    const float n = glm::dot(forward, enu[1]); // north component
    if (e * e + n * n < 1e-12f)
        return 0.f;
    return std::atan2(e, n);
}

// Bank/roll angle (rad) of the entity relative to the local up at `pos`. 0 = wings level.
// Same convention as the historical world-frame roll (atan2(-right.y, up.y)) generalised to the
// radial up: reduces to it exactly when the local up is world +Y (near the origin). quat is
// EntityTransform order [x, y, z, w].
[[nodiscard]] inline float bankOf(const float quat[4], glm::dvec3 pos, double R) noexcept {
    const glm::quat q(quat[3], quat[0], quat[1], quat[2]);
    const glm::vec3 up = q * glm::vec3(0.f, 1.f, 0.f);    // body up
    const glm::vec3 right = q * glm::vec3(0.f, 0.f, 1.f); // body right
    const glm::vec3 lu = radialUp(pos, R);
    return std::atan2(-glm::dot(lu, right), glm::dot(lu, up));
}

} // namespace fl
