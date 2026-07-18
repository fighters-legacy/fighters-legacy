// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "render/BuiltinTextures.h" // BuiltinRgbaTexture

#include <array>

// Compiled-in procedural biome terrain textures for the zero-content-pack sandbox (#446): the
// fallback the terrain renderer uploads as 2D texture ARRAYS when a pack ships no
// terrain/biome_basecolor.ktx2 / terrain/biome_normalorm.ktx2. Four layers, ordered by biome id
// (the SAME ABI order the tex-compress --layers workflow and the shader use):
//   0 = grass, 1 = dirt, 2 = rock, 3 = snow.
//
// DETERMINISTIC: fixed integer/hash math, never rand()/time — byte-stable across runs (unit-tested).
// Same generator family as BuiltinTextures.h; uploaded via the raw-RGBA array path
// (TextureUploadDesc::rawLayers), so no KTX2 container is needed.
namespace fl {

inline constexpr int kBiomeLayerCount = 4;

enum class BiomeLayer : uint8_t { Grass = 0, Dirt = 1, Rock = 2, Snow = 3 };

// Base-color layers (sRGB), layer index == biome id. Each is kBuiltinTexSize square RGBA8.
[[nodiscard]] std::array<BuiltinRgbaTexture, kBiomeLayerCount> builtinBiomeBaseColorLayers();

// Normal + roughness layers (LINEAR), layer index == biome id. Channel packing matches the pack
// authoring convention: R,G = tangent-space normal x,y (flat = 128); B = roughness; A = occlusion.
[[nodiscard]] std::array<BuiltinRgbaTexture, kBiomeLayerCount> builtinBiomeNormalOrmLayers();

} // namespace fl
