// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Altitude wind interpolation (#489). The ONE piece of code both the server (WorldBroadcaster's
// per-entity wind) and the client (ClientPrediction's replay) run, so a projectile or aircraft feels
// the SAME wind at a given altitude on both sides — parity is by construction (pure float arithmetic,
// clamped piecewise-linear, no libm). Header-only, in engine/weather so both link it.
//
// The profile lives on EnvironmentState (windProfile[] + windProfileCount, ascending by altitude,
// absolute world-frame wind m/s at each level). count == 0 means "no profile": fall back to the
// datum-level windX/windZ scalar.

#include "flight/Geodetic.h" // geodeticAltitude, kEarthRadiusM

#include <RenderTypes.h>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

namespace fl {

// Radial MSL altitude (m) of a world position — the altitude the wind profile is indexed by. Shared
// so server and client bucket a position to the same level.
[[nodiscard]] inline double windAltitudeM(const glm::dvec3& pos, double R = kEarthRadiusM) noexcept {
    return geodeticAltitude(pos.x, pos.y, pos.z, R);
}

// World-frame wind (m/s) at altitude `altM`, clamped piecewise-linear over the profile knots. Falls
// back to the datum scalar (env.windX/windZ) when the profile is empty.
[[nodiscard]] inline glm::vec2 windAtAltitude(const EnvironmentState& env, float altM) noexcept {
    const int n = env.windProfileCount;
    if (n <= 0)
        return glm::vec2(env.windX, env.windZ);
    if (altM <= env.windProfile[0].altM)
        return glm::vec2(env.windProfile[0].windX, env.windProfile[0].windZ);
    const int last = n - 1;
    if (altM >= env.windProfile[last].altM)
        return glm::vec2(env.windProfile[last].windX, env.windProfile[last].windZ);
    for (int i = 0; i < last; ++i) {
        const auto& a = env.windProfile[i];
        const auto& b = env.windProfile[i + 1];
        if (altM >= a.altM && altM <= b.altM) {
            const float span = b.altM - a.altM;
            const float t = (span > 1e-6f) ? (altM - a.altM) / span : 0.0f;
            return glm::vec2(a.windX + (b.windX - a.windX) * t, a.windZ + (b.windZ - a.windZ) * t);
        }
    }
    return glm::vec2(env.windProfile[last].windX, env.windProfile[last].windZ); // unreachable if ascending
}

} // namespace fl
