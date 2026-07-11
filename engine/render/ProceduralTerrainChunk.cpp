// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/ProceduralTerrainChunk.h"

#include <glm/vec3.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace fl {

const ProceduralTerrainParams kBuiltinProceduralParams{};

// ---------------------------------------------------------------------------
// 3D value noise FBM on the sphere — deterministic, cross-platform, no deps
// ---------------------------------------------------------------------------

// Wang hash: maps any uint32_t to a pseudo-random uint32_t.
static uint32_t wangHash(uint32_t v) noexcept {
    v = (v ^ 61u) ^ (v >> 16u);
    v *= 9u;
    v ^= v >> 4u;
    v *= 0x27d4eb2du;
    v ^= v >> 15u;
    return v;
}

// Produce a [0, 1) float from integer lattice coordinates (ix, iy, iz).
static float latticeValue(int32_t ix, int32_t iy, int32_t iz) noexcept {
    uint32_t h =
        wangHash(static_cast<uint32_t>(ix) ^ wangHash(static_cast<uint32_t>(iy) ^ wangHash(static_cast<uint32_t>(iz))));
    // Map to [0, 1): top 23 bits form a float mantissa.
    return static_cast<float>(h >> 9u) * (1.f / static_cast<float>(1u << 23u));
}

// Quintic smoothstep: f(t) = 6t^5 - 15t^4 + 10t^3
static float smoothstep(float t) noexcept {
    return t * t * t * (t * (t * 6.f - 15.f) + 10.f);
}

// Trilinear value noise sampled at a continuous 3D point. Returns a value in [0, 1].
static float valueNoise3(float x, float y, float z) noexcept {
    const int32_t ix = static_cast<int32_t>(std::floor(x));
    const int32_t iy = static_cast<int32_t>(std::floor(y));
    const int32_t iz = static_cast<int32_t>(std::floor(z));
    const float sx = smoothstep(x - static_cast<float>(ix));
    const float sy = smoothstep(y - static_cast<float>(iy));
    const float sz = smoothstep(z - static_cast<float>(iz));

    auto lerp = [](float a, float b, float t) noexcept { return a + (b - a) * t; };

    const float v000 = latticeValue(ix, iy, iz);
    const float v100 = latticeValue(ix + 1, iy, iz);
    const float v010 = latticeValue(ix, iy + 1, iz);
    const float v110 = latticeValue(ix + 1, iy + 1, iz);
    const float v001 = latticeValue(ix, iy, iz + 1);
    const float v101 = latticeValue(ix + 1, iy, iz + 1);
    const float v011 = latticeValue(ix, iy + 1, iz + 1);
    const float v111 = latticeValue(ix + 1, iy + 1, iz + 1);

    const float bottom = lerp(lerp(v000, v100, sx), lerp(v010, v110, sx), sy);
    const float top = lerp(lerp(v001, v101, sx), lerp(v011, v111, sx), sy);
    return lerp(bottom, top, sz);
}

// FBM: sum of 3D value-noise octaves at point p (already scaled to lattice units).
// Returns a value roughly in [-1, 1].
static float fbm3(glm::vec3 p, const ProceduralTerrainParams& params) noexcept {
    float value = 0.f;
    float amplitude = 1.f;
    float frequency = 1.f;
    float maxValue = 0.f;

    for (int i = 0; i < params.octaves; ++i) {
        // Shift each octave to break lattice-aligned artefacts.
        const float ox = static_cast<float>(i) * 1.7321f;
        const float oy = static_cast<float>(i) * 2.2361f;
        const float oz = static_cast<float>(i) * 3.1415f;
        value += valueNoise3(p.x * frequency + ox, p.y * frequency + oy, p.z * frequency + oz) * amplitude;
        maxValue += amplitude;
        amplitude *= params.gain;
        frequency *= params.lacunarity;
    }

    // Normalise to [-1, 1].
    return (value / maxValue) * 2.f - 1.f;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

std::vector<uint16_t> generateProceduralTile(const TileKey& key, double planetRadiusM,
                                             const ProceduralTerrainParams& params) noexcept {
    constexpr int kPixels = kTileHeightmapSize;
    std::vector<uint16_t> out(static_cast<std::size_t>(kPixels) * kPixels);

    // Noise domain: unit sphere direction scaled so one lattice cell spans ~frequencyM
    // metres of arc on the surface. double until the final scale, then float — the
    // lattice coordinates stay small (R / frequencyM ~ a few hundred), so float
    // precision is uniform across the globe.
    const double domainScale = planetRadiusM / static_cast<double>(params.frequencyM);

    const double inv = 1.0 / static_cast<double>(kPixels - 1);
    for (int row = 0; row < kPixels; ++row) {
        const double t = static_cast<double>(row) * inv;
        for (int col = 0; col < kPixels; ++col) {
            const double s = static_cast<double>(col) * inv;
            // Sample direction for this tile-local (s, t) — identical for any tile that
            // shares the sample point (edges/corners), so the field is globally seamless.
            const double n = static_cast<double>(uint64_t{1} << key.level);
            const double u = (static_cast<double>(key.i) + s) / n;
            const double v = (static_cast<double>(key.j) + t) / n;
            const glm::dvec3 dir = faceUvToDirection(key.face, u, v);
            const glm::vec3 p = glm::vec3(dir * domainScale);

            const float noise = fbm3(p, params);
            const float elevM = params.baseElevationM + noise * params.amplitudeM;
            const float encoded = elevM + 32768.f;
            out[static_cast<std::size_t>(row) * kPixels + col] =
                static_cast<uint16_t>(std::clamp(encoded, 0.f, 65535.f));
        }
    }

    return out;
}

} // namespace fl
