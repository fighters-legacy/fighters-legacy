// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/ProceduralTerrainChunk.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace fl {

const ProceduralTerrainParams kBuiltinProceduralParams{};

// ---------------------------------------------------------------------------
// Value noise FBM — deterministic, cross-platform, no external deps
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

// Produce a [0, 1) float from integer grid coordinates (ix, iy).
// Uses combined hash of both coordinates for independence.
static float latticeValue(int32_t ix, int32_t iy) noexcept {
    uint32_t h = wangHash(static_cast<uint32_t>(ix) ^ wangHash(static_cast<uint32_t>(iy)));
    // Map to [0, 1): top 23 bits form a float mantissa.
    return static_cast<float>(h >> 9u) * (1.f / static_cast<float>(1u << 23u));
}

// Quintic smoothstep: f(t) = 6t^5 - 15t^4 + 10t^3
static float smoothstep(float t) noexcept {
    return t * t * t * (t * (t * 6.f - 15.f) + 10.f);
}

// Bilinear value noise sampled at continuous coordinates (x, y).
// Returns a value in [0, 1].
static float valueNoise(float x, float y) noexcept {
    const int32_t ix = static_cast<int32_t>(std::floor(x));
    const int32_t iy = static_cast<int32_t>(std::floor(y));
    const float fx = x - static_cast<float>(ix);
    const float fy = y - static_cast<float>(iy);

    const float v00 = latticeValue(ix, iy);
    const float v10 = latticeValue(ix + 1, iy);
    const float v01 = latticeValue(ix, iy + 1);
    const float v11 = latticeValue(ix + 1, iy + 1);

    const float sx = smoothstep(fx);
    const float sy = smoothstep(fy);

    return v00 + (v10 - v00) * sx + (v01 - v00) * sy + (v00 - v10 - v01 + v11) * sx * sy;
}

// Fractional Brownian Motion: sum of value-noise octaves sampled at (wx, wz)
// in world-space metres. Returns a value roughly in [-1, 1].
static float fbm(float wx, float wz, const ProceduralTerrainParams& p) noexcept {
    float value = 0.f;
    float amplitude = 1.f;
    float frequency = 1.f / p.frequencyM;
    float maxValue = 0.f;

    for (int i = 0; i < p.octaves; ++i) {
        // Shift each octave to break axis-aligned artefacts.
        const float sx = wx * frequency + static_cast<float>(i) * 1.7321f;
        const float sz = wz * frequency + static_cast<float>(i) * 3.1415f;
        value += valueNoise(sx, sz) * amplitude;
        maxValue += amplitude;
        amplitude *= p.gain;
        frequency *= p.lacunarity;
    }

    // Normalise to [-1, 1].
    return (value / maxValue) * 2.f - 1.f;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

std::vector<uint16_t> generateProceduralChunk(int cx, int cy, const TerrainManifest& manifest,
                                              const ProceduralTerrainParams& params) noexcept {
    constexpr int kPixels = 513;
    std::vector<uint16_t> out(kPixels * kPixels);

    // One pixel covers chunkSizeM / 512 metres. Keep as double so the
    // per-pixel addition stays in double precision before casting to float.
    const double pixelSizeM = static_cast<double>(manifest.chunkSizeM) / 512.0;

    // World-space X and Z of the SW corner of this chunk.
    const double chunkOriginX = manifest.originX + static_cast<double>(cx) * manifest.chunkSizeM;
    const double chunkOriginZ = manifest.originZ + static_cast<double>(cy) * manifest.chunkSizeM;

    for (int row = 0; row < kPixels; ++row) {
        const float wz = static_cast<float>(chunkOriginZ + row * pixelSizeM);
        for (int col = 0; col < kPixels; ++col) {
            const float wx = static_cast<float>(chunkOriginX + col * pixelSizeM);

            const float n = fbm(wx, wz, params);
            const float elevM = params.baseElevationM + n * params.amplitudeM;
            const float encoded = elevM + 32768.f;
            out[row * kPixels + col] = static_cast<uint16_t>(std::clamp(encoded, 0.f, 65535.f));
        }
    }

    return out;
}

} // namespace fl
