// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "render/CubeSphere.h" // TileKey, tileToWorld

#include <glm/vec3.hpp>

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

// Generate a binary glTF 2.0 (GLB) mesh for one cube-sphere tile (#471, spherical-Earth epic #468).
//
// Unlike buildTerrainMeshGlb (planar grid, spherical curvature fudged into a scalar Y), this builds
// a TRUE curved tile: every vertex world position comes from CubeSphere::tileToWorld(key, s, t, h, R)
// and every normal from the cross product of the two surface tangents, so curvature is exact at any
// scale (including the antipode, where the planar sqrt(R^2 - x^2 - z^2) term goes imaginary).
//
// heights       — row-major uint16 heightmap, heightmapSize x heightmapSize; elevation_m = value - 32768
// heightmapSize — samples per axis (513 / 257 / 129 for LOD 0/1/2)
// meshGrid      — quads per axis (128 / 64 / 32 for LOD 0/1/2)
// key           — cube-sphere tile addressed; vertices span tile-local (s, t) in [0,1]^2
// R             — planet radius in metres
// tileOriginWorld — the camera-relative rebase origin (typically the tile-centre world position at
//                   sea level). Vertices are emitted RELATIVE to this (small float32 offsets); the
//                   single rebase lives in the model matrix, so mesh.vert is unchanged. See the
//                   camera-relative rebase invariant in CLAUDE.md / #468.
// landCover     — optional row-major uint8 WorldCover class per heightmap pixel (heightmapSize^2). When
//                 non-null, a COLOR_0 accessor (VEC4 u8 normalized) is emitted with the class in .r.
// emitTexcoord  — when true (default), a TEXCOORD_0 accessor (VEC2 f32 = tile-local (s, t)) is emitted.
//
// Output GLB has POSITION + NORMAL (VEC3 f32), optional TEXCOORD_0 (VEC2 f32) / COLOR_0 (VEC4 u8
// normalized), UNSIGNED_SHORT indices, non-interleaved bufferViews. Winding is CCW-from-outside
// (validate-mesh clean): the (s->U, t->V) tile basis is right-handed with the outward normal U x V,
// so the triangle order is reversed relative to the left-handed planar (col->X, row->Z, up->+Y) grid.
//
// Returns an empty vector on invalid input (empty heights, meshGrid <= 0, heightmapSize < 2,
// heightmapSize < meshGrid + 1, or heights too small).
std::vector<uint8_t> buildTileMeshGlb(const std::vector<uint16_t>& heights, int heightmapSize, int meshGrid,
                                      const TileKey& key, double R, glm::dvec3 tileOriginWorld,
                                      const uint8_t* landCover = nullptr, bool emitTexcoord = true) noexcept;

} // namespace fl
