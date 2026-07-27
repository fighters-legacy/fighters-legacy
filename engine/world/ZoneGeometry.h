// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "world/AirspaceZone.h"

#include <cstddef>
#include <utility>
#include <vector>

namespace fl {

// Pure containment tests for an AirspaceZone (#162). Zones are volumes of revolution about the
// world Y axis: a shape in the XZ plane crossed with an altitude band, matching the AirspaceZone
// POD contract and the planar (x, z) vocabulary missions already author spawn points in.
//
// Free functions over plain scalars so the geometry is unit-testable on its own and so both the
// per-tick AlertSystem pass and validate-mission share one implementation.

// Circle: (x-cx)^2 + (z-cz)^2 < r^2. A non-positive radius contains nothing.
[[nodiscard]] inline bool pointInCircleXZ(double cx, double cz, double radiusM, double x, double z) noexcept {
    if (radiusM <= 0.0)
        return false;
    const double dx = x - cx;
    const double dz = z - cz;
    return dx * dx + dz * dz < radiusM * radiusM;
}

// Crossing-number (ray-casting) containment for a simple polygon in XZ. Deliberately not
// convexity-dependent: a mission that slips a concave ring past authoring validation still gets a
// well-defined answer instead of undefined behaviour. Fewer than three vertices contains nothing.
[[nodiscard]] inline bool pointInPolygonXZ(const std::vector<std::pair<double, double>>& verts, double x,
                                           double z) noexcept {
    const std::size_t n = verts.size();
    if (n < 3)
        return false;

    bool inside = false;
    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
        const double xi = verts[i].first, zi = verts[i].second;
        const double xj = verts[j].first, zj = verts[j].second;
        // Half-open edge rule (zi > z) != (zj > z): a vertex lying exactly on the ray is counted
        // once, so a point level with a vertex does not flip parity twice.
        if ((zi > z) != (zj > z) && x < (xj - xi) * (z - zi) / (zj - zi) + xi)
            inside = !inside;
    }
    return inside;
}

// Altitude band, metres above the datum. Inclusive at both edges: an authored ceiling of 12000 is
// the last altitude the zone covers, which is what a mission author writing "up to 12 km" means.
[[nodiscard]] inline bool altitudeInZone(const AirspaceZone& zone, double altM) noexcept {
    return altM >= zone.altFloorM && altM <= zone.altCeilingM;
}

// Full test: XZ shape AND altitude band.
[[nodiscard]] inline bool zoneContains(const AirspaceZone& zone, double x, double z, double altM) noexcept {
    if (!altitudeInZone(zone, altM))
        return false;
    return zone.shape == ZoneShape::Circle ? pointInCircleXZ(zone.centerX, zone.centerZ, zone.radiusM, x, z)
                                           : pointInPolygonXZ(zone.vertices, x, z);
}

// Authoring-time convexity check for a polygon zone: every cross product of consecutive edges must
// share a sign (collinear runs allowed). AirspaceZone documents polygons as convex, and a concave
// or self-intersecting ring is far more often an authoring slip than an intent -- validate-mission
// reports it rather than letting the mission ship with a zone nobody can predict. Fewer than three
// vertices is not a polygon and is reported as non-convex.
[[nodiscard]] inline bool isConvexPolygonXZ(const std::vector<std::pair<double, double>>& verts) noexcept {
    const std::size_t n = verts.size();
    if (n < 3)
        return false;

    int sign = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const auto& a = verts[i];
        const auto& b = verts[(i + 1) % n];
        const auto& c = verts[(i + 2) % n];
        const double cross = (b.first - a.first) * (c.second - b.second) - (b.second - a.second) * (c.first - b.first);
        if (cross > 0.0) {
            if (sign < 0)
                return false;
            sign = 1;
        } else if (cross < 0.0) {
            if (sign > 0)
                return false;
            sign = -1;
        }
    }
    // sign == 0 means every vertex is collinear -- a degenerate line, not a polygon.
    return sign != 0;
}

} // namespace fl
