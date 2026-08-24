// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/BuiltinBiomes.h"

#include <cstdint>

namespace fl {

namespace {

// A deterministic value-noise-ish hash in [0,1) from integer pixel coords + a per-layer seed. Wang
// hash mixing, IEEE-float only — byte-stable everywhere (same discipline as the sky/terrain FBM).
float hash01(int x, int y, uint32_t seed) {
    uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return static_cast<float>(h & 0xFFFFFFu) / static_cast<float>(0x1000000u);
}

[[nodiscard]] uint8_t clamp8(int v) {
    return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

// Per-layer base colour + grain amplitude, in id order (grass, dirt, rock, snow). Colours are sRGB
// (the base-color array is an sRGB texture, so the shader reads them back through sRGB->linear); they
// are chosen so their LINEAR values land near sensible terrain albedos (grass ~0.23/0.42/0.15 linear),
// NOT as raw dark values that would crush to near-black once linearised.
struct BiomeStyle {
    uint8_t r, g, b;
    int grain;     // +/- albedo variation from the noise
    uint8_t rough; // base roughness (ORM.b)
};
constexpr BiomeStyle kStyles[kBiomeLayerCount] = {
    {133, 173, 107, 22, 200}, // grass — green
    {179, 162, 130, 18, 205}, // dirt  — brown
    {173, 169, 164, 26, 170}, // rock  — grey
    {244, 247, 251, 10, 120}, // snow  — near-white, smoother
};

} // namespace

std::array<BuiltinRgbaTexture, kBiomeLayerCount> builtinBiomeBaseColorLayers() {
    std::array<BuiltinRgbaTexture, kBiomeLayerCount> out;
    for (int layer = 0; layer < kBiomeLayerCount; ++layer) {
        const BiomeStyle& s = kStyles[layer];
        BuiltinRgbaTexture t = makeBuiltinRgba();
        const auto seed = static_cast<uint32_t>(layer * 101 + 7);
        for (int y = 0; y < t.height; ++y)
            for (int x = 0; x < t.width; ++x) {
                const float n = hash01(x, y, seed) - 0.5f; // [-0.5, 0.5]
                const int d = static_cast<int>(n * 2.f * static_cast<float>(s.grain));
                putPixel(t, x, y, clamp8(s.r + d), clamp8(s.g + d), clamp8(s.b + d), 255);
            }
        out[static_cast<std::size_t>(layer)] = std::move(t);
    }
    return out;
}

std::array<BuiltinRgbaTexture, kBiomeLayerCount> builtinBiomeNormalOrmLayers() {
    std::array<BuiltinRgbaTexture, kBiomeLayerCount> out;
    for (int layer = 0; layer < kBiomeLayerCount; ++layer) {
        const BiomeStyle& s = kStyles[layer];
        BuiltinRgbaTexture t = makeBuiltinRgba();
        const auto seed = static_cast<uint32_t>(layer * 211 + 31);
        for (int y = 0; y < t.height; ++y)
            for (int x = 0; x < t.width; ++x) {
                // Finite-difference the noise for a gentle per-biome bump; rock bumps most, snow least.
                const float amp = (layer == 2) ? 60.f : (layer == 3 ? 12.f : 34.f);
                const float hx = hash01(x + 1, y, seed) - hash01(x - 1, y, seed);
                const float hy = hash01(x, y + 1, seed) - hash01(x, y - 1, seed);
                const uint8_t nx = clamp8(128 - static_cast<int>(hx * amp));
                const uint8_t ny = clamp8(128 - static_cast<int>(hy * amp));
                const int roughVar = static_cast<int>((hash01(x, y, seed + 9u) - 0.5f) * 40.f);
                putPixel(t, x, y, nx, ny, clamp8(s.rough + roughVar), 255); // B=roughness, A=occlusion(full)
            }
        out[static_cast<std::size_t>(layer)] = std::move(t);
    }
    return out;
}

} // namespace fl
