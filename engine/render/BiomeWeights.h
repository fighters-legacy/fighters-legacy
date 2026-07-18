// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <glm/vec4.hpp>

#include <cstdint>

namespace fl {

// Biome blend weights for an ESA WorldCover class (#475). The vec4 lanes are the terrain biome
// texture-array layer order (#446): x = grass (layer 0), y = dirt (1), z = rock (2), w = snow (3).
//
// SINGLE SOURCE OF TRUTH. The terrain fragment shader mirrors this table exactly in
// platform/vulkan/shaders/mesh.frag (biomeWeightsForWorldCover). test_biome_weights pins the values
// here; if you change one, change both and update the test.
//
// A class of 255 (or any unmapped code) returns all-zero — the shader reads that as "no land-cover"
// and falls back to its elevation/slope selection (the pre-#475 behaviour, for tiles with no _lc
// layer). Water (80) and wetland (90/95) still return land weights here; the shader separately
// darkens/glosses them via worldCoverWaterness() below.
[[nodiscard]] constexpr glm::vec4 biomeWeightsForWorldCover(uint8_t cls) noexcept {
    switch (cls) {
    case 10:                               // tree cover
    case 20:                               // shrubland
    case 30:                               // grassland
    case 40:                               // cropland
    case 100:                              // moss & lichen
        return {1.0f, 0.0f, 0.0f, 0.0f};   // vegetation -> grass
    case 90:                               // herbaceous wetland
    case 95:                               // mangroves
        return {0.7f, 0.3f, 0.0f, 0.0f};   // wetland -> mostly grass over dirt
    case 50:                               // built-up
        return {0.0f, 0.6f, 0.4f, 0.0f};   // dirt/rock
    case 60:                               // bare / sparse vegetation
        return {0.0f, 0.45f, 0.55f, 0.0f}; // dirt/rock
    case 70:                               // snow & ice
        return {0.0f, 0.0f, 0.0f, 1.0f};
    case 80:                             // permanent water bodies
        return {0.0f, 0.5f, 0.5f, 0.0f}; // dirt/rock base (shader darkens via waterness)
    default:
        return {0.0f, 0.0f, 0.0f, 0.0f}; // 255 / unknown -> elevation/slope fallback
    }
}

// 0 = dry land, 1 = open water (dark + glossy in the shader). Wetland reads as half-water.
[[nodiscard]] constexpr float worldCoverWaterness(uint8_t cls) noexcept {
    switch (cls) {
    case 80:
        return 1.0f;
    case 90:
    case 95:
        return 0.5f;
    default:
        return 0.0f;
    }
}

// True when a class carries no usable land-cover (the shader uses its elevation/slope fallback).
[[nodiscard]] constexpr bool worldCoverIsUnclassified(uint8_t cls) noexcept {
    const glm::vec4 w = biomeWeightsForWorldCover(cls);
    return (w.x + w.y + w.z + w.w) <= 0.0f;
}

} // namespace fl
