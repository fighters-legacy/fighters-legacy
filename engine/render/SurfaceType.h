// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Terrain surface classification (#475, part of the spherical-Earth epic #468). The tile pipeline
// carries an ESA WorldCover land-cover class per vertex (a raw class byte in COLOR_0 / TerrainStreamer
// land-cover layer); this enum is the engine-facing vocabulary that byte maps to, so gameplay and
// rendering reason about a named surface (water/forest/snow/…) rather than a magic number.
//
// ESA WorldCover 2021 v200 class codes (the source data): 10 tree cover, 20 shrubland, 30 grassland,
// 40 cropland, 50 built-up, 60 bare/sparse vegetation, 70 snow & ice, 80 permanent water bodies,
// 90 herbaceous wetland, 95 mangroves, 100 moss & lichen.

#include <cstdint>

namespace fl {

enum class SurfaceType : uint8_t {
    Unknown = 0,
    Water,
    Grass,
    Forest,
    Urban,
    Snow,
    Rock,
    Wetland,
};

// Map a raw ESA WorldCover class code to a SurfaceType. Unrecognised codes (including 0 = no data)
// return Unknown. Header-only, pure — unit-tested and shared by the streamer query and any tooling.
[[nodiscard]] constexpr SurfaceType surfaceTypeFromWorldCover(uint8_t worldCoverClass) noexcept {
    switch (worldCoverClass) {
    case 10:
        return SurfaceType::Forest; // tree cover
    case 20:                        // shrubland
    case 30:                        // grassland
    case 40:                        // cropland
    case 100:                       // moss & lichen
        return SurfaceType::Grass;
    case 50:
        return SurfaceType::Urban; // built-up
    case 60:
        return SurfaceType::Rock; // bare / sparse vegetation
    case 70:
        return SurfaceType::Snow; // snow & ice
    case 80:
        return SurfaceType::Water; // permanent water bodies
    case 90:                       // herbaceous wetland
    case 95:                       // mangroves
        return SurfaceType::Wetland;
    default:
        return SurfaceType::Unknown;
    }
}

} // namespace fl
