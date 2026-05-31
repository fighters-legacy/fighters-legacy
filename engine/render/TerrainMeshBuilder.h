// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <vector>

namespace fl {

// Generate a binary glTF 2.0 (GLB) terrain mesh from a 16-bit heightmap.
//
// heights       — row-major uint16; elevation_m = value - 32768
// heightmapSize — number of samples per axis (513 / 257 / 129 for LOD 0/1/2)
// meshGrid      — number of quads per axis (128 / 64 / 32 for LOD 0/1/2)
// chunkSizeM    — physical side length of the chunk in metres
//
// Output GLB has POSITION (VEC3 float32) and NORMAL (VEC3 float32) accessors with
// UNSIGNED_SHORT indices and non-interleaved bufferViews. Vertices are in chunk-local
// space: X in [0, chunkSizeM], Y = elevation_m, Z in [0, chunkSizeM].
//
// Returns an empty vector on invalid input (empty heights, zero meshGrid, hmSize < 2,
// or heightmapSize < meshGrid + 1).
std::vector<uint8_t> buildTerrainMeshGlb(const std::vector<uint16_t>& heights, int heightmapSize, int meshGrid,
                                         float chunkSizeM) noexcept;

} // namespace fl
