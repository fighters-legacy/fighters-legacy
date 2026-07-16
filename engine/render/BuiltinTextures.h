// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <vector>

// Compiled-in procedural PBR textures for the zero-content-pack sandbox (#867): a base-color, a
// normal, and an ORM (occlusion/roughness/metallic) map applied to the builtin placeholder mesh so
// the renderer's albedo / normal / ORM SAMPLING path runs with no content pack (the builtin material
// otherwise used only PBR scalar factors). Uploaded as raw RGBA8 via TextureUploadDesc::rawWidth
// (the raw-RGBA fallback), so no KTX2/PNG container is needed.
//
// DETERMINISTIC: generated from fixed integer math, never rand()/time — byte-stable across runs.
namespace fl {

inline constexpr int kBuiltinTexSize = 64; // px, square

// A raw RGBA8 texture: `pixels` is width*height*4 bytes, row-major, R,G,B,A.
struct BuiltinRgbaTexture {
    int width{0};
    int height{0};
    std::vector<uint8_t> pixels;
};

// A subtle panel/grid base color — a mid grey with darker seam lines, so the albedo sample is
// visibly textured rather than a flat factor. sRGB.
[[nodiscard]] BuiltinRgbaTexture builtinBaseColorTexture();

// A tangent-space normal map with a gentle bevel toward the seam lines, so normal-mapped lighting is
// exercised. Linear (not sRGB); flat is (128,128,255).
[[nodiscard]] BuiltinRgbaTexture builtinNormalTexture();

// An ORM map: R=occlusion (1), G=roughness (~0.7 with grooves), B=metallic (0). Linear.
[[nodiscard]] BuiltinRgbaTexture builtinOrmTexture();

} // namespace fl
