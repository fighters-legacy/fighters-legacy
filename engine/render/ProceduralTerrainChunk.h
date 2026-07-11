// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "render/CubeSphere.h" // TileKey

#include <cstdint>
#include <vector>

namespace fl {

struct ProceduralTerrainParams {
    float baseElevationM = 550.f; // sea-level offset for the terrain base
    float amplitudeM = 150.f;     // half-range of elevation variation
    float frequencyM = 30000.f;   // spatial period of primary FBM features
    int octaves = 4;              // FBM octave count
    float lacunarity = 2.f;       // frequency multiplier per octave
    float gain = 0.5f;            // amplitude multiplier per octave
};

// Nevada-like open desert: flat basin with gentle rolling terrain.
extern const ProceduralTerrainParams kBuiltinProceduralParams;

// Number of height samples per axis in a procedural cube-sphere tile (128 quads + 1).
inline constexpr int kTileHeightmapSize = 129;

// Generate a kTileHeightmapSize^2 row-major uint16_t heightmap for one cube-sphere
// tile (#472). Height encoding matches gen_terrain_chunks.py defaults:
//   uint16 = clamp(elevation_m + 32768, 0, 65535)
// where elevation_m is the RADIAL height above the sphere datum (the `h` passed to
// CubeSphere::tileToWorld).
//
// The FBM is sampled in the GLOBAL SPHERE DOMAIN: each sample's unit direction from
// the planet centre (faceUvToDirection) is scaled by R / frequencyM and fed to a 3D
// value-noise FBM. Because the domain is a 3D position (not per-face uv), the field is
// seamless across all face edges and corners, and any two tiles that share a sample
// direction produce bit-identical values — the server and client generate identical
// terrain with no wire transfer. Deterministic across platforms: integer Wang-hash
// lattice + IEEE-754 single-precision arithmetic only.
//
// Thread-safe; pure function with no shared mutable state.
std::vector<uint16_t> generateProceduralTile(const TileKey& key, double planetRadiusM,
                                             const ProceduralTerrainParams& params) noexcept;

} // namespace fl
