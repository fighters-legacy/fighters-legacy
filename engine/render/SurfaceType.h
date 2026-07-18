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

#include "flight/GroundSurface.h"

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
    // Runway surfaces (#487), appended so the WorldCover ordinals above stay stable (no land-cover
    // class maps to these). TerrainStreamer::surfaceTypeAt reports one of these inside a runway
    // footprint via the injected override, so ground physics can differentiate a paved runway from
    // the grass beside it. The bridge from the RunwaySurface authoring enum lives in RunwaySurfaceMap.h.
    Concrete,
    Asphalt,
    Gravel,
    Deck,
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

// Ground-handling parameters (#487) for a surface, mapping the terrain SurfaceType to the physics
// GroundFriction the FlightIntegrator applies during ground contact. Shared by the server
// (WorldBroadcaster) and the client (ClientPrediction) so the rollout is bit-identical on both — pure
// table math, no libm. A hard runway / unknown / non-ground surface adds no extra rolling resistance
// (bit-identical to before the surface feature); grass and gravel add drag; water is a ditching
// (heavy drag). Header-only so both layers include it without an engine-flight link beyond the header.
[[nodiscard]] constexpr GroundFriction groundFrictionFor(SurfaceType s) noexcept {
    switch (s) {
    case SurfaceType::Grass:
    case SurfaceType::Forest:
    case SurfaceType::Wetland:
        return GroundFriction{/*extraRollingPerSec=*/0.35f};
    case SurfaceType::Gravel:
    case SurfaceType::Rock:
        return GroundFriction{0.15f};
    case SurfaceType::Snow:
        return GroundFriction{0.20f};
    case SurfaceType::Water:
        return GroundFriction{1.5f}; // a ditching bleeds speed hard
    case SurfaceType::Concrete:
    case SurfaceType::Asphalt:
    case SurfaceType::Deck:
    case SurfaceType::Urban:
    case SurfaceType::Unknown:
        return GroundFriction{}; // hard paved / default — no extra rolling resistance
    }
    return GroundFriction{};
}

} // namespace fl
