// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

// Load-test affordance (#573): deterministic placement for the server-side AI entities that stress
// the entity pool + SpatialIndex at scale. Pure geometry, extracted from fl-server main() so it is
// unit-testable (mirrors resolveWorkerCount / clampCatchupTicks). A TESTING AFFORDANCE, NOT A
// CAPACITY GUARANTEE.

namespace fl {

// Returns `count` world positions {x, y, z} spread over a disk of radius `spreadM` (metres) around
// the origin via a phyllotaxis (sunflower) pattern — an even, deterministic fill that populates many
// SpatialIndex cells rather than clustering into one. All entities share the fixed altitude
// `baseElevM + aglM` (loiter AI at altitude; avoids per-entity terrain queries across an un-primed
// spread). Deterministic: identical input yields identical output on every platform.
inline std::vector<std::array<double, 3>> testSpawnPositions(uint32_t count, double spreadM, double aglM,
                                                             double baseElevM) {
    std::vector<std::array<double, 3>> out;
    out.reserve(count);
    const double y = baseElevM + aglM;
    // Golden angle in radians (pi * (3 - sqrt(5))).
    constexpr double kGoldenAngle = 2.399963229728653;
    const double denom = count > 0u ? static_cast<double>(count) : 1.0;
    for (uint32_t i = 0; i < count; ++i) {
        // sqrt distributes points uniformly by area; (i + 0.5)/count keeps the first point off the
        // exact centre (radius 0 would collapse onto the origin / the peer spawn).
        const double r = spreadM * std::sqrt((static_cast<double>(i) + 0.5) / denom);
        const double theta = static_cast<double>(i) * kGoldenAngle;
        out.push_back({r * std::cos(theta), y, r * std::sin(theta)});
    }
    return out;
}

} // namespace fl
