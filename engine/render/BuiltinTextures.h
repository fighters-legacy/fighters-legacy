// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IRenderer.h" // TextureUploadDesc / TextureHandle, for the upload helpers

#include <cstddef>
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

// A zeroed RGBA8 texture of the builtin size, and one pixel store into it. Both procedural
// generators (BuiltinTextures.cpp's panels, BuiltinBiomes.cpp's terrain layers) carried their own
// byte-identical copies of these two (#1265) -- including the width*height*4 sizing and the
// row-major index, which are the two places an off-by-one silently produces a skewed texture.
[[nodiscard]] inline BuiltinRgbaTexture makeBuiltinRgba() {
    BuiltinRgbaTexture t;
    t.width = kBuiltinTexSize;
    t.height = kBuiltinTexSize;
    t.pixels.resize(static_cast<std::size_t>(kBuiltinTexSize) * kBuiltinTexSize * 4u);
    return t;
}

inline void putPixel(BuiltinRgbaTexture& t, int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    const std::size_t i = (static_cast<std::size_t>(y) * t.width + x) * 4u;
    t.pixels[i] = r;
    t.pixels[i + 1] = g;
    t.pixels[i + 2] = b;
    t.pixels[i + 3] = a;
}

// A subtle panel/grid base color — a mid grey with darker seam lines, so the albedo sample is
// visibly textured rather than a flat factor. sRGB.
[[nodiscard]] BuiltinRgbaTexture builtinBaseColorTexture();

// A tangent-space normal map with a gentle bevel toward the seam lines, so normal-mapped lighting is
// exercised. Linear (not sRGB); flat is (128,128,255).
[[nodiscard]] BuiltinRgbaTexture builtinNormalTexture();

// An ORM map: R=occlusion (1), G=roughness (~0.7 with grooves), B=metallic (0). Linear.
[[nodiscard]] BuiltinRgbaTexture builtinOrmTexture();

// The three builtin maps, uploaded and ready to hang on a material (#1265).
struct BuiltinPbrMaps {
    TextureHandle baseColor;
    TextureHandle normal;
    TextureHandle orm;
};

// Upload one BuiltinRgbaTexture through the raw-RGBA fallback (no KTX2/PNG container).
[[nodiscard]] inline TextureHandle uploadBuiltinTexture(IRenderer& renderer, const char* name,
                                                        const BuiltinRgbaTexture& tex, bool srgb) {
    TextureUploadDesc td{};
    td.name = name;
    td.bytes = tex.pixels;
    td.srgb = srgb;
    td.rawWidth = static_cast<uint32_t>(tex.width);
    td.rawHeight = static_cast<uint32_t>(tex.height);
    return renderer.createTexture(td);
}

// Generate and upload all three (#867). SceneRenderer's builtin material and PreviewScene's
// material-less fallback both need them, and PreviewScene's comment called itself a "mirror" of
// SceneRenderer's — which is the dual-maintenance shape this removes.
//
// ⚠ THE sRGB TRIPLE IS THE POINT: base colour is sRGB, normal and ORM are LINEAR. Getting one wrong
// does not fail anything, it just lights the model incorrectly, and it was previously three separate
// flags at each of two call sites. `namePrefix` stays a parameter because the two callers name their
// textures differently and those names show up in GPU captures.
[[nodiscard]] inline BuiltinPbrMaps uploadBuiltinPbrMaps(IRenderer& renderer, const char* baseName,
                                                         const char* normalName, const char* ormName) {
    return {uploadBuiltinTexture(renderer, baseName, builtinBaseColorTexture(), /*srgb=*/true),
            uploadBuiltinTexture(renderer, normalName, builtinNormalTexture(), /*srgb=*/false),
            uploadBuiltinTexture(renderer, ormName, builtinOrmTexture(), /*srgb=*/false)};
}

} // namespace fl
