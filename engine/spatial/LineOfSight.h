// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// Terrain line-of-sight segment query (#687). A shared engine/spatial utility, decoupled from the
// #630 collision phase: nothing ray- or LOS-shaped existed anywhere in the engine, yet both the
// client padlock lock-break (#697) and server-side AI sensing terrain gates (#670) need the same
// "can point A see point B over the terrain?" answer. One utility serves both.
//
// The heightfield is INJECTED as a callable (template, not std::function — zero overhead, no heap):
//   heightFn(double x, double y, double z) -> double  : RADIAL terrain elevation above the sphere
//                                                        datum along the query point (the same
//                                                        convention as TerrainStreamer::heightAt).
//   readyFn (double x, double y, double z) -> bool     : is the terrain tile covering this point
//                                                        loaded? Defaults to "always ready".
//
// engine-spatial is deliberately pure ISO C++ stdlib (no glm), so this header takes raw double[3]
// endpoints like SpatialIndex's double[3] positions. Callers in the glm-based layers bridge with
// glm::value_ptr(dvec3) and a small lambda that forwards to their heightfield.
//
// Planet model: sphere centre at {0, -R, 0} (world +Y up, north pole = origin), so a point's radial
// altitude above the datum is length(p - centre) - R — identical to fl::geodeticAltitude.

namespace fl {

enum class LosResult : uint8_t {
    Clear,   // the whole segment stays above the terrain (+ clearance)
    Blocked, // terrain (or an endpoint below it) masks the segment
    Unknown, // terrain data was not loaded somewhere along the segment; caller policy decides
             // (padlock treats Unknown as Clear so unloaded terrain never false-breaks a lock)
};

namespace detail {
struct AlwaysReady {
    constexpr bool operator()(double, double, double) const noexcept {
        return true;
    }
};
} // namespace detail

// Marches the segment [a, b], sampling terrain clearance. The step is adaptive: proportional to the
// current clearance margin (floored near the clearance, capped at 20*stepM), so it densifies at
// grazing tangents and stretches over deep valleys — this margin-bounded stepping is what makes the
// result step-size-independent and prevents a thin ridge from being tunnelled between samples.
// No heap allocation; all state is on the stack.
template <typename HeightFn, typename ReadyFn = detail::AlwaysReady>
LosResult terrainLos(const double a[3], const double b[3], HeightFn&& heightFn, ReadyFn&& readyFn = {},
                     double planetRadiusM = 6'371'000.0, double stepM = 50.0, double clearanceM = 1.0) {
    const double R = planetRadiusM;

    // Signed clearance of the ray above the terrain at (x,y,z): >0 clear, <0 blocked.
    auto margin = [&](double x, double y, double z) -> double {
        const double ypR = y + R;
        const double altAboveDatum = std::sqrt(x * x + ypR * ypR + z * z) - R;
        return altAboveDatum - static_cast<double>(heightFn(x, y, z)) - clearanceM;
    };

    const double dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
    const double segLen = std::sqrt(dx * dx + dy * dy + dz * dz);

    bool sawUnknown = false;
    const double minStep = std::max(1.0, clearanceM);
    const double maxStep = std::max(minStep, 20.0 * stepM);

    // Zero-length (and near-vertical is fine — altitude varies, height ~constant): endpoint only.
    if (segLen < 1e-6) {
        if (!readyFn(a[0], a[1], a[2]))
            return LosResult::Unknown;
        return margin(a[0], a[1], a[2]) < 0.0 ? LosResult::Blocked : LosResult::Clear;
    }

    const double invLen = 1.0 / segLen;
    const double ux = dx * invLen, uy = dy * invLen, uz = dz * invLen;

    double t = 0.0;
    while (true) {
        const bool last = (t >= segLen);
        const double s = last ? segLen : t;
        const double x = a[0] + ux * s, y = a[1] + uy * s, z = a[2] + uz * s;

        if (!readyFn(x, y, z)) {
            // Unloaded height data is garbage (heightFn would report the datum) — never let it
            // false-block. Mark Unknown, step a fixed stride, keep marching.
            sawUnknown = true;
            if (last)
                break;
            t += stepM;
            continue;
        }

        const double m = margin(x, y, z);
        if (m < 0.0)
            return LosResult::Blocked; // endpoint-below-terrain and mid-segment crossings
        if (last)
            break;

        t += std::clamp(0.9 * m, minStep, maxStep);
    }

    return sawUnknown ? LosResult::Unknown : LosResult::Clear;
}

} // namespace fl
