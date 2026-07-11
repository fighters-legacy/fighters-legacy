// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Cube-sphere tile addressing and geometry core (#469, part of the spherical-Earth epic #468).
//
// A cube-sphere maps the six faces of a cube onto a sphere, giving a global, seam-free,
// singularity-free tiling — unlike the Cartesian chunk grid (whose curvature term goes imaginary
// at the antipode). Each face carries a quadtree of square tiles; a TileKey addresses one tile.
//
// Conventions (consistent with engine/flight/Geodetic.h):
//   * Planet centre is at world {0, -R, 0}; the direction from the centre to a world point p is
//     n = normalize(p - {0,-R,0}) = normalize(p.x, p.y+R, p.z).
//   * The world origin {0,0,0} is the NORTH POLE (lat = +pi/2): it is the centre of the +Y face.
//   * +180 deg longitude is an ordinary cube edge (no seam); the poles are the +/-Y face centres
//     (no polar singularity).
//
// Face indices (by outward axis):  0:+X  1:-X  2:+Y  3:-Y  4:+Z  5:-Z
//
// Per face we pick a right-handed tangent basis (U, V, N) with N the outward face normal and
// U x V = N, so the cube is consistently CCW-wound from outside. A face point at warped coords
// (a, b) in [-1,1]^2 is  p_cube = a*U + b*V + N.  Because the warp tan(c*pi/4) is odd and maps
// +/-1 -> +/-1, adjacent faces evaluate a shared cube edge to the same 3D point, so vertices
// coincide across all 12 edges and 8 corners (C0 continuity).
//
// The face->sphere warp is the tangent-adaptive scheme (tan(c*pi/4)); it noticeably reduces the
// area distortion of a raw normalized cube. Nowell equal-area is a later drop-in upgrade
// (replace warp/invWarp only).
//
// Header-only, pure math, zero runtime wiring — deliberately de-risks the streamer rewrite first.

#include <glm/geometric.hpp> // normalize
#include <glm/vec3.hpp>

#include <cmath>
#include <cstdint>

namespace fl {

// A tile in the cube-sphere quadtree. At `level` there are 2^level tiles per face axis;
// (i, j) index the tile within the face's [0,1]^2 uv-grid (i along u, j along v).
struct TileKey {
    uint8_t face{0};  // 0..5, see face-index table above
    uint8_t level{0}; // quadtree depth; 2^level tiles per axis
    uint32_t i{0};    // tile column in [0, 2^level)
    uint32_t j{0};    // tile row    in [0, 2^level)

    [[nodiscard]] constexpr bool operator==(const TileKey& o) const noexcept {
        return face == o.face && level == o.level && i == o.i && j == o.j;
    }
    [[nodiscard]] constexpr bool operator!=(const TileKey& o) const noexcept {
        return !(*this == o);
    }
};

// World face uv position: face + fractional (u, v) in [0,1]^2. Returned by worldToTile as a
// level-0 TileKey (i = j = 0) plus (s, t) = (u, v) — the exact inverse of tileToWorld at level 0.
struct TileCoord {
    TileKey key;
    double s{0.0}; // local u within key (== face u at level 0)
    double t{0.0}; // local v within key (== face v at level 0)
};

constexpr uint8_t kCubeFaceCount = 6;

// Face adjacency: for each face, the neighbour face across its [u<0, u>1, v<0, v>1] edges.
// Documentation / fast-topology table; the coordinate transform is done geometrically by
// neighbor(). Index 0 = -u edge, 1 = +u edge, 2 = -v edge, 3 = +v edge.
inline constexpr uint8_t kFaceEdgeNeighbor[6][4] = {
    /* 0 +X: U=+Y V=+Z */ {3, 2, 5, 4},
    /* 1 -X: U=+Z V=+Y */ {5, 4, 3, 2},
    /* 2 +Y: U=+Z V=+X */ {5, 4, 1, 0},
    /* 3 -Y: U=+X V=+Z */ {1, 0, 5, 4},
    /* 4 +Z: U=+X V=+Y */ {1, 0, 3, 2},
    /* 5 -Z: U=+Y V=+X */ {3, 2, 1, 0},
};

// ── warp (tangent-adaptive) ────────────────────────────────────────────────────
constexpr double kQuarterPi = 0.785398163397448309616;

[[nodiscard]] inline double cubeWarp(double c) noexcept {
    return std::tan(c * kQuarterPi);
}
[[nodiscard]] inline double cubeUnwarp(double w) noexcept {
    return std::atan(w) / kQuarterPi;
}

// ── cube <-> direction ──────────────────────────────────────────────────────────

// Build the (un-normalized) cube-face point for face `f` at warped coords (a, b).
[[nodiscard]] inline glm::dvec3 cubePoint(uint8_t f, double a, double b) noexcept {
    switch (f) {
    case 0:
        return {1.0, a, b}; // +X: U=+Y, V=+Z
    case 1:
        return {-1.0, b, a}; // -X: U=+Z, V=+Y
    case 2:
        return {b, 1.0, a}; // +Y: U=+Z, V=+X  (centre -> +Y = north pole)
    case 3:
        return {a, -1.0, b}; // -Y: U=+X, V=+Z
    case 4:
        return {a, b, 1.0}; // +Z: U=+X, V=+Y
    default:
        return {b, a, -1.0}; // -Z: U=+Y, V=+X
    }
}

// Face uv in [0,1]^2 -> unit direction from the planet centre.
[[nodiscard]] inline glm::dvec3 faceUvToDirection(uint8_t face, double u, double v) noexcept {
    const double a = cubeWarp(2.0 * u - 1.0);
    const double b = cubeWarp(2.0 * v - 1.0);
    return glm::normalize(cubePoint(face, a, b));
}

// Unit (or any non-zero) direction from the planet centre -> face + face uv in [0,1]^2.
[[nodiscard]] inline TileCoord directionToFaceUv(glm::dvec3 dir) noexcept {
    const double ax = std::abs(dir.x), ay = std::abs(dir.y), az = std::abs(dir.z);
    uint8_t face;
    double a, b; // warped face coords
    if (ax >= ay && ax >= az) {
        const double k = ax;
        if (dir.x > 0.0) {
            face = 0;
            a = dir.y / k;
            b = dir.z / k;
        } // +X: (1,a,b)
        else {
            face = 1;
            b = dir.y / k;
            a = dir.z / k;
        } // -X: (-1,b,a)
    } else if (ay >= ax && ay >= az) {
        const double k = ay;
        if (dir.y > 0.0) {
            face = 2;
            b = dir.x / k;
            a = dir.z / k;
        } // +Y: (b,1,a)
        else {
            face = 3;
            a = dir.x / k;
            b = dir.z / k;
        } // -Y: (a,-1,b)
    } else {
        const double k = az;
        if (dir.z > 0.0) {
            face = 4;
            a = dir.x / k;
            b = dir.y / k;
        } // +Z: (a,b,1)
        else {
            face = 5;
            b = dir.x / k;
            a = dir.y / k;
        } // -Z: (b,a,-1)
    }
    TileCoord out;
    out.key.face = face;
    out.s = 0.5 * (cubeUnwarp(a) + 1.0);
    out.t = 0.5 * (cubeUnwarp(b) + 1.0);
    return out;
}

// ── tile <-> world ──────────────────────────────────────────────────────────────

// Tile-local (s, t) in [0,1]^2 + height h (m above the sphere) -> world position (m).
[[nodiscard]] inline glm::dvec3 tileToWorld(const TileKey& key, double s, double t, double h, double R) noexcept {
    const double n = static_cast<double>(uint64_t{1} << key.level);
    const double u = (static_cast<double>(key.i) + s) / n;
    const double v = (static_cast<double>(key.j) + t) / n;
    const glm::dvec3 dir = faceUvToDirection(key.face, u, v);
    return dir * (R + h) + glm::dvec3{0.0, -R, 0.0};
}

// World position -> level-0 tile (face) + fractional face uv in (s, t). Exact inverse of
// tileToWorld at level 0; the caller refines to a level via i = floor(s * 2^level), etc.
[[nodiscard]] inline TileCoord worldToTile(glm::dvec3 world, double R) noexcept {
    const glm::dvec3 dir = glm::normalize(world - glm::dvec3{0.0, -R, 0.0});
    return directionToFaceUv(dir);
}

// ── quadtree topology ────────────────────────────────────────────────────────────

// Parent tile (one level coarser). Root tiles (level 0) are returned unchanged.
[[nodiscard]] inline TileKey parent(const TileKey& k) noexcept {
    if (k.level == 0)
        return k;
    return {k.face, static_cast<uint8_t>(k.level - 1), k.i >> 1, k.j >> 1};
}

// Child tile for quadrant q in [0,3]: bit0 = +i half, bit1 = +j half.
[[nodiscard]] inline TileKey child(const TileKey& k, uint8_t q) noexcept {
    return {k.face, static_cast<uint8_t>(k.level + 1), (k.i << 1) | (q & 1u), (k.j << 1) | ((q >> 1) & 1u)};
}

// Tile index along one axis at `level` for a face-uv coordinate in [0, 1]:
// floor(uv * 2^level) clamped into [0, 2^level - 1] (guards a floating-point uv
// landing exactly on 1.0 or just below 0.0).
[[nodiscard]] inline uint32_t tileIndexForUv(double uv, uint8_t level) noexcept {
    const uint32_t n = uint32_t{1} << level;
    double c = uv * static_cast<double>(n);
    if (c < 0.0)
        c = 0.0;
    const uint32_t idx = static_cast<uint32_t>(c);
    return idx >= n ? n - 1 : idx;
}

// Edge-neighbour direction for neighbor().
enum class TileEdge : uint8_t { NegU = 0, PosU = 1, NegV = 2, PosV = 3 };

// Neighbouring tile across one edge, at the same level. Within a face this is (i,j)+/-1; across a
// face boundary it resolves the adjacent face and the rotated/flipped (i,j) geometrically — sample
// the neighbour tile's centre direction and re-project — so no hand-coded per-edge transform table
// is needed and the result is guaranteed consistent with tileToWorld.
[[nodiscard]] inline TileKey neighbor(const TileKey& k, TileEdge edge) noexcept {
    const uint32_t n = uint32_t{1} << k.level;
    // In-face fast path.
    switch (edge) {
    case TileEdge::NegU:
        if (k.i > 0)
            return {k.face, k.level, k.i - 1, k.j};
        break;
    case TileEdge::PosU:
        if (k.i + 1 < n)
            return {k.face, k.level, k.i + 1, k.j};
        break;
    case TileEdge::NegV:
        if (k.j > 0)
            return {k.face, k.level, k.i, k.j - 1};
        break;
    case TileEdge::PosV:
        if (k.j + 1 < n)
            return {k.face, k.level, k.i, k.j + 1};
        break;
    }
    // Cross-face: aim at the centre of where the neighbour tile would be (half a tile beyond the
    // shared edge, at the edge's mid-span), convert to a direction, and re-project onto the sphere.
    const double inv = 1.0 / static_cast<double>(n);
    double u = (static_cast<double>(k.i) + 0.5) * inv;
    double v = (static_cast<double>(k.j) + 0.5) * inv;
    switch (edge) {
    case TileEdge::NegU:
        u = static_cast<double>(k.i) * inv - 0.5 * inv;
        break;
    case TileEdge::PosU:
        u = (static_cast<double>(k.i) + 1.0) * inv + 0.5 * inv;
        break;
    case TileEdge::NegV:
        v = static_cast<double>(k.j) * inv - 0.5 * inv;
        break;
    case TileEdge::PosV:
        v = (static_cast<double>(k.j) + 1.0) * inv + 0.5 * inv;
        break;
    }
    const TileCoord nc = directionToFaceUv(faceUvToDirection(k.face, u, v));
    return {nc.key.face, k.level, tileIndexForUv(nc.s, k.level), tileIndexForUv(nc.t, k.level)};
}

} // namespace fl
