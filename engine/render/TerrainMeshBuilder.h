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
// chunkWorldX   — world X of the chunk's col=0 corner (default 0)
// chunkWorldZ   — world Z of the chunk's row=0 corner (default 0)
// planetRadius  — sphere radius in metres; 0 = flat (default)
//
// When planetRadius > 0 each vertex Y incorporates the per-vertex spherical correction
// sqrt(R²−vx²−vz²)−R at that vertex's world position. Surface normals account for the
// curvature gradient too. Call setPlanetRadius() and provide chunk world coords before
// the first update() call; meshes are baked at finalize time.
//
// Output GLB has POSITION (VEC3 float32) and NORMAL (VEC3 float32) accessors with
// UNSIGNED_SHORT indices and non-interleaved bufferViews. Vertices are in chunk-local
// space: X in [0, chunkSizeM], Y = elevation_m + spherical correction, Z in [0, chunkSizeM].
//
// Returns an empty vector on invalid input (empty heights, zero meshGrid, hmSize < 2,
// or heightmapSize < meshGrid + 1).
std::vector<uint8_t> buildTerrainMeshGlb(const std::vector<uint16_t>& heights, int heightmapSize, int meshGrid,
                                         float chunkSizeM, double chunkWorldX = 0.0, double chunkWorldZ = 0.0,
                                         double planetRadius = 0.0) noexcept;

} // namespace fl
