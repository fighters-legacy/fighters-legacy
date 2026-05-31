// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "render/TerrainManifest.h"

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

// Generate a 513x513 row-major uint16_t heightmap for terrain chunk (cx, cy).
// Height encoding matches gen_terrain_chunks.py defaults:
//   uint16 = clamp(elevation_m + 32768, 0, 65535)
// Adjacent chunks are seamless because the noise is sampled in world-space
// coordinates: world_x = manifest.originX + (cx + u) * manifest.chunkSizeM.
// Thread-safe; pure function with no shared mutable state.
std::vector<uint16_t> generateProceduralChunk(int cx, int cy, const TerrainManifest& manifest,
                                              const ProceduralTerrainParams& params) noexcept;

} // namespace fl
