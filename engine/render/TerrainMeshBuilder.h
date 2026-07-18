// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "render/CubeSphere.h" // TileKey, tileToWorld

#include <glm/vec3.hpp>

#include <cstdint>
#include <vector>

namespace fl {

// Generate a binary glTF 2.0 (GLB) mesh for one cube-sphere tile (#471, spherical-Earth epic #468).
//
// Every vertex world position comes from CubeSphere::tileToWorld(key, s, t, h, R) and every normal
// from the cross-product of the two central-difference surface tangents, so curvature (and terrain
// slope) are exact at any scale — including the antipode. The former planar builder
// (buildTerrainMeshGlb) was removed with the quadtree streamer rewrite (#472).
//
// heights       — row-major uint16 heightmap, heightmapSize x heightmapSize; elevation_m = value - 32768
// heightmapSize — samples per axis
// meshGrid      — quads per axis (heightmapSize must be >= meshGrid + 1)
// key           — cube-sphere tile addressed; vertices span tile-local (s, t) in [0,1]^2
// R             — planet radius in metres
// tileOriginWorld — the camera-relative rebase origin (typically the tile-centre world position at
//                   sea level). Vertices are emitted RELATIVE to this (small float32 offsets); the
//                   single rebase lives in the model matrix, so mesh.vert is unchanged. See the
//                   camera-relative rebase invariant in CLAUDE.md / #468.
// landCover     — optional row-major uint8 WorldCover class per heightmap pixel (heightmapSize^2). Feeds
//                 the packed-terrain TANGENT's .x (class; 255 = "no land-cover" when null → the shader
//                 falls back to elevation/slope selection).
// emitTexcoord  — when true (default), a TEXCOORD_0 accessor (VEC2 f32 = tile-local (s, t)) AND the
//                 packed-terrain TANGENT accessor are emitted (they ride together — see below).
// skirtDepthM   — when > 0, appends a skirt (#472): the outer edge ring is duplicated and pushed
//                 toward the planet centre by this many metres, with side quads sealing the tile
//                 border. Hides the hairline cracks at quadtree LOD boundaries; skirt vertices copy
//                 their source vertex's normal/texcoord/color so the flap shades like the surface.
//                 0 (default) emits no skirt — output is byte-identical to the pre-skirt builder.
//
// Output GLB has POSITION + NORMAL (VEC3 f32), optional TEXCOORD_0 (VEC2 f32) and a packed-terrain
// TANGENT (VEC4 f32, #475) — NOT a real tangent: .x = WorldCover class, .y = normalized elevation,
// .zw = spherical-valid detail coordinate in metres (global face-UV arc length mod 3000 m, seamless
// across tiles and LOD). UNSIGNED_SHORT indices, non-interleaved bufferViews. Winding is
// CCW-from-outside (validate-mesh clean): the (s->U, t->V) tile basis is right-handed, outward U x V.
//
// Returns an empty vector on invalid input (empty heights, meshGrid <= 0, heightmapSize < 2,
// heightmapSize < meshGrid + 1, heights too small, or a vertex count exceeding uint16 indices).
std::vector<uint8_t> buildTileMeshGlb(const std::vector<uint16_t>& heights, int heightmapSize, int meshGrid,
                                      const TileKey& key, double R, glm::dvec3 tileOriginWorld,
                                      const uint8_t* landCover = nullptr, bool emitTexcoord = true,
                                      double skirtDepthM = 0.0) noexcept;

} // namespace fl
