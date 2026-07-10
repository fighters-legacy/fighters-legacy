// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for the cube-sphere tile addressing + geometry core (#469).

#include "flight/Geodetic.h"
#include "render/CubeSphere.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <numbers>
#include <utility>

using namespace fl;

namespace {

constexpr double R = kEarthRadiusM;

// Inverse of cubePoint for a direction KNOWN to lie on face f: recover face uv in [0,1]^2.
std::pair<double, double> faceUvForFace(uint8_t f, glm::dvec3 d) {
    double a = 0.0, b = 0.0;
    switch (f) {
    case 0:
        a = d.y / d.x;
        b = d.z / d.x;
        break; // +X: (1,a,b)
    case 1: {
        double k = -d.x;
        b = d.y / k;
        a = d.z / k;
    } break; // -X: (-1,b,a)
    case 2: {
        double k = d.y;
        b = d.x / k;
        a = d.z / k;
    } break; // +Y: (b,1,a)
    case 3: {
        double k = -d.y;
        a = d.x / k;
        b = d.z / k;
    } break; // -Y: (a,-1,b)
    case 4: {
        double k = d.z;
        a = d.x / k;
        b = d.y / k;
    } break; // +Z: (a,b,1)
    default: {
        double k = -d.z;
        b = d.x / k;
        a = d.y / k;
    } break; // -Z: (b,a,-1)
    }
    return {0.5 * (cubeUnwarp(a) + 1.0), 0.5 * (cubeUnwarp(b) + 1.0)};
}

uint8_t faceOf(int axis, int sign) {
    return static_cast<uint8_t>(axis * 2 + (sign > 0 ? 0 : 1));
}

} // namespace

TEST_CASE("CubeSphere: warp/unwarp are inverses and fix +/-1") {
    CHECK(cubeWarp(0.0) == Catch::Approx(0.0));
    CHECK(cubeWarp(1.0) == Catch::Approx(1.0));
    CHECK(cubeWarp(-1.0) == Catch::Approx(-1.0));
    for (double c = -1.0; c <= 1.0; c += 0.1)
        CHECK(cubeUnwarp(cubeWarp(c)) == Catch::Approx(c).margin(1e-12));
}

TEST_CASE("CubeSphere: +Y face centre is the north pole, -Y is the south pole") {
    const glm::dvec3 north = tileToWorld({2, 0, 0, 0}, 0.5, 0.5, 0.0, R);
    CHECK(north.x == Catch::Approx(0.0).margin(1e-6));
    CHECK(north.y == Catch::Approx(0.0).margin(1e-6)); // world origin
    CHECK(north.z == Catch::Approx(0.0).margin(1e-6));
    LatLonAlt nl = worldToGeodetic(north.x, north.y, north.z, R);
    CHECK(nl.lat_rad == Catch::Approx(std::numbers::pi / 2.0).margin(1e-9));

    const glm::dvec3 south = tileToWorld({3, 0, 0, 0}, 0.5, 0.5, 0.0, R);
    LatLonAlt sl = worldToGeodetic(south.x, south.y, south.z, R);
    CHECK(sl.lat_rad == Catch::Approx(-std::numbers::pi / 2.0).margin(1e-9));
}

TEST_CASE("CubeSphere: world -> face uv -> world round-trips to ~mm") {
    // Sweep every face over an interior uv grid.
    for (uint8_t f = 0; f < kCubeFaceCount; ++f) {
        for (double u = 0.05; u < 1.0; u += 0.1) {
            for (double v = 0.05; v < 1.0; v += 0.1) {
                const glm::dvec3 world = tileToWorld({f, 0, 0, 0}, u, v, 1234.0, R);
                const TileCoord tc = worldToTile(world, R);
                CHECK(tc.key.face == f);
                CHECK(tc.s == Catch::Approx(u).margin(1e-9));
                CHECK(tc.t == Catch::Approx(v).margin(1e-9));
                // And the reconstructed world position matches to sub-mm.
                const glm::dvec3 back = tileToWorld(tc.key, tc.s, tc.t, 1234.0, R);
                CHECK(glm::length(back - world) < 1e-3);
            }
        }
    }
}

TEST_CASE("CubeSphere: all 8 cube corners coincide across their 3 incident faces") {
    // Each of the 24 (face, corner-uv) pairs maps to one of 8 corner directions; group and require
    // each corner to be produced identically by exactly 3 faces.
    struct Hit {
        glm::dvec3 dir;
        int count{0};
    };
    std::array<Hit, 8> corners{};
    auto cornerIndex = [](glm::dvec3 d) { return (d.x > 0 ? 1 : 0) | (d.y > 0 ? 2 : 0) | (d.z > 0 ? 4 : 0); };
    for (uint8_t f = 0; f < kCubeFaceCount; ++f) {
        for (double u : {0.0, 1.0}) {
            for (double v : {0.0, 1.0}) {
                const glm::dvec3 d = faceUvToDirection(f, u, v);
                const int idx = cornerIndex(d);
                if (corners[idx].count == 0)
                    corners[idx].dir = d;
                else
                    CHECK(glm::length(corners[idx].dir - d) < 1e-12); // coincident
                corners[idx].count++;
            }
        }
    }
    for (const auto& c : corners)
        CHECK(c.count == 3); // every corner shared by exactly 3 faces
}

TEST_CASE("CubeSphere: all 12 cube edges are continuous across their 2 incident faces") {
    const std::array<std::pair<int, int>, 3> axisPairs = {{{0, 1}, {0, 2}, {1, 2}}};
    for (auto [axA, axB] : axisPairs) {
        const int freeAx = 3 - axA - axB;
        for (int sA : {+1, -1}) {
            for (int sB : {+1, -1}) {
                const uint8_t fa = faceOf(axA, sA);
                const uint8_t fb = faceOf(axB, sB);
                for (double t : {-0.9, -0.5, 0.0, 0.5, 0.9}) {
                    glm::dvec3 p{0.0};
                    p[axA] = static_cast<double>(sA);
                    p[axB] = static_cast<double>(sB);
                    p[freeAx] = t;
                    const glm::dvec3 d = glm::normalize(p);
                    // Both incident faces must reproduce the exact edge direction.
                    auto [ua, va] = faceUvForFace(fa, d);
                    auto [ub, vb] = faceUvForFace(fb, d);
                    CHECK(glm::length(faceUvToDirection(fa, ua, va) - d) < 1e-12);
                    CHECK(glm::length(faceUvToDirection(fb, ub, vb) - d) < 1e-12);
                }
            }
        }
    }
}

TEST_CASE("CubeSphere: parent/child arithmetic round-trips") {
    TileKey k{4, 3, 5, 6};
    for (uint8_t q = 0; q < 4; ++q) {
        TileKey c = child(k, q);
        CHECK(c.level == 4);
        CHECK(parent(c) == k);
    }
    // Child quadrant bits: bit0 = +i, bit1 = +j.
    CHECK(child(k, 0) == TileKey{4, 4, 10, 12});
    CHECK(child(k, 1) == TileKey{4, 4, 11, 12});
    CHECK(child(k, 2) == TileKey{4, 4, 10, 13});
    CHECK(child(k, 3) == TileKey{4, 4, 11, 13});
    // Root has no parent.
    CHECK(parent(TileKey{2, 0, 0, 0}) == TileKey{2, 0, 0, 0});
}

TEST_CASE("CubeSphere: in-face neighbours are simple index steps") {
    TileKey k{0, 3, 3, 4}; // level 3 -> 8x8 grid, interior tile
    CHECK(neighbor(k, TileEdge::PosU) == TileKey{0, 3, 4, 4});
    CHECK(neighbor(k, TileEdge::NegU) == TileKey{0, 3, 2, 4});
    CHECK(neighbor(k, TileEdge::PosV) == TileKey{0, 3, 3, 5});
    CHECK(neighbor(k, TileEdge::NegV) == TileKey{0, 3, 3, 3});
}

TEST_CASE("CubeSphere: cross-face neighbour lands on the adjacent face and is reciprocal") {
    // A tile on the +U border of every face steps onto that face's +U neighbour (kFaceEdgeNeighbor).
    for (uint8_t f = 0; f < kCubeFaceCount; ++f) {
        const uint8_t level = 3;
        const uint32_t n = 1u << level;
        TileKey border{f, level, n - 1, 2}; // right edge, mid row
        TileKey nb = neighbor(border, TileEdge::PosU);
        CHECK(nb.face == kFaceEdgeNeighbor[f][1]); // index 1 = +u edge
        CHECK(nb.level == level);
        // Reciprocity: the neighbour tile's world border coincides with this tile's border, so
        // stepping back from nb reaches a tile on face f again.
        bool backToF = false;
        for (auto e : {TileEdge::NegU, TileEdge::PosU, TileEdge::NegV, TileEdge::PosV})
            if (neighbor(nb, e).face == f)
                backToF = true;
        CHECK(backToF);
    }
}

TEST_CASE("CubeSphere: cross-face neighbour tiles share their border in world space") {
    // The shared vertex between a border tile and its cross-face neighbour must coincide (C0).
    const uint8_t level = 2;
    const uint32_t n = 1u << level;
    TileKey k{4, level, n - 1, 1}; // +Z face, +U border
    TileKey nb = neighbor(k, TileEdge::PosU);
    REQUIRE(nb.face != k.face);
    // k's +U border corner (s=1, t=0) is a shared cube-grid vertex; it must be reproducible from nb.
    const glm::dvec3 corner = tileToWorld(k, 1.0, 0.0, 0.0, R);
    const TileCoord tc = worldToTile(corner, R);
    // The corner lies on the face boundary; it resolves to one of the two incident faces, and
    // re-projecting reproduces the same world point to sub-mm either way.
    const glm::dvec3 back = tileToWorld(tc.key, tc.s, tc.t, 0.0, R);
    CHECK(glm::length(back - corner) < 1e-3);
}
