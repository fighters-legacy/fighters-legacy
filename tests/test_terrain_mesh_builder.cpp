// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for buildTileMeshGlb (#471): curved cube-sphere tile meshes with true world positions,
// curvature-correct normals, and optional COLOR_0 / TEXCOORD_0 attributes.

#include "render/CubeSphere.h"
#include "render/TerrainMeshBuilder.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace fl;

namespace {

// GLB layout produced by buildTileMeshGlb is deterministic non-interleaved:
// POSITION, NORMAL, [TEXCOORD_0], [COLOR_0], INDICES.
struct GlbView {
    std::string json;
    const uint8_t* bin{nullptr};
    std::size_t binLen{0};
};

GlbView parseGlb(const std::vector<uint8_t>& glb) {
    REQUIRE(glb.size() >= 20u);
    uint32_t jsonLen = 0;
    std::memcpy(&jsonLen, glb.data() + 12, 4);
    REQUIRE(glb.size() >= 20u + jsonLen + 8u);
    GlbView v;
    v.json.assign(reinterpret_cast<const char*>(glb.data() + 20), jsonLen);
    const std::size_t binStart = 20u + jsonLen + 8u;
    v.bin = glb.data() + binStart;
    v.binLen = glb.size() - binStart;
    return v;
}

glm::vec3 readVec3(const uint8_t* base, std::size_t byteOff, int idx) {
    glm::vec3 out;
    std::memcpy(&out, base + byteOff + static_cast<std::size_t>(idx) * 12u, 12u);
    return out;
}

glm::vec2 readVec2(const uint8_t* base, std::size_t byteOff, int idx) {
    glm::vec2 out;
    std::memcpy(&out, base + byteOff + static_cast<std::size_t>(idx) * 8u, 8u);
    return out;
}

uint16_t readIdx(const uint8_t* base, std::size_t byteOff, int idx) {
    uint16_t out;
    std::memcpy(&out, base + byteOff + static_cast<std::size_t>(idx) * 2u, 2u);
    return out;
}

} // namespace

TEST_CASE("buildTileMeshGlb rejects invalid input", "[terrain][tile-mesh]") {
    const int size = 129;
    std::vector<uint16_t> heights(static_cast<std::size_t>(size) * size, 32768);
    const TileKey key{4, 3, 2, 2};
    const double R = 6371000.0;
    const glm::dvec3 origin = tileToWorld(key, 0.5, 0.5, 0.0, R);

    CHECK(buildTileMeshGlb({}, size, 32, key, R, origin).empty());
    CHECK(buildTileMeshGlb(heights, size, 0, key, R, origin).empty());
    CHECK(buildTileMeshGlb(heights, 1, 32, key, R, origin).empty());
    CHECK(buildTileMeshGlb(heights, 5, 32, key, R, origin).empty()); // hmSize < meshGrid + 1
}

TEST_CASE("buildTileMeshGlb parses as a valid GLB", "[terrain][tile-mesh]") {
    const int size = 129;
    std::vector<uint16_t> heights(static_cast<std::size_t>(size) * size, 32768);
    const TileKey key{4, 5, 16, 16};
    const double R = 6371000.0;
    const glm::dvec3 origin = tileToWorld(key, 0.5, 0.5, 0.0, R);

    auto glb = buildTileMeshGlb(heights, size, 32, key, R, origin);
    REQUIRE(!glb.empty());
    // GLB magic 'glTF' little-endian.
    CHECK(glb[0] == 0x67u);
    CHECK(glb[1] == 0x6Cu);
    CHECK(glb[2] == 0x54u);
    CHECK(glb[3] == 0x46u);
    uint32_t version = 0, total = 0;
    std::memcpy(&version, glb.data() + 4, 4);
    std::memcpy(&total, glb.data() + 8, 4);
    CHECK(version == 2u);
    CHECK(total == glb.size());
}

TEST_CASE("buildTileMeshGlb vertex world positions match tileToWorld", "[terrain][tile-mesh]") {
    const int size = 129;
    const int meshGrid = 32;
    const int stride = (size - 1) / meshGrid;
    const int gridPts = meshGrid + 1;
    // Non-flat heightmap so positions exercise the height term too.
    std::vector<uint16_t> heights(static_cast<std::size_t>(size) * size);
    for (int r = 0; r < size; ++r)
        for (int c = 0; c < size; ++c)
            heights[static_cast<std::size_t>(r) * size + c] = static_cast<uint16_t>(32768 + (c - r));

    const TileKey key{4, 5, 16, 16};
    const double R = 6371000.0;
    const glm::dvec3 origin = tileToWorld(key, 0.5, 0.5, 0.0, R);

    auto glb = buildTileMeshGlb(heights, size, meshGrid, key, R, origin);
    auto view = parseGlb(glb);
    // Sample a handful of vertices and reconstruct the world position.
    for (int row : {0, 7, 16, 31, gridPts - 1}) {
        for (int col : {0, 5, 16, gridPts - 1}) {
            const int vi = row * gridPts + col;
            const glm::vec3 rel = readVec3(view.bin, 0, vi);
            const glm::dvec3 got = glm::dvec3(rel) + origin;
            const double s = static_cast<double>(col) / meshGrid;
            const double t = static_cast<double>(row) / meshGrid;
            const double h =
                static_cast<double>(heights[static_cast<std::size_t>(row * stride) * size + col * stride]) - 32768.0;
            const glm::dvec3 want = tileToWorld(key, s, t, h, R);
            CHECK(glm::length(got - want) < 0.5); // float32 rebase precision near a small tile
        }
    }
}

TEST_CASE("buildTileMeshGlb normals are unit-length and radial on a flat tile", "[terrain][tile-mesh]") {
    const int size = 129;
    const int meshGrid = 32;
    const int gridPts = meshGrid + 1;
    std::vector<uint16_t> heights(static_cast<std::size_t>(size) * size, 32768); // flat, sea level

    const TileKey key{4, 5, 16, 16};
    const double R = 6371000.0;
    const glm::dvec3 origin = tileToWorld(key, 0.5, 0.5, 0.0, R);
    const glm::dvec3 centre{0.0, -R, 0.0};

    auto glb = buildTileMeshGlb(heights, size, meshGrid, key, R, origin);
    auto view = parseGlb(glb);
    const int vertCount = gridPts * gridPts;
    const std::size_t nrmOff = static_cast<std::size_t>(vertCount) * 12u;

    for (int row : {0, 16, gridPts - 1}) {
        for (int col : {0, 16, gridPts - 1}) {
            const int vi = row * gridPts + col;
            const glm::vec3 nrm = readVec3(view.bin, nrmOff, vi);
            CHECK(glm::length(nrm) == Catch::Approx(1.0f).margin(1e-4));
            // Flat tile: the surface normal is the radial (outward) direction at that vertex.
            const glm::vec3 rel = readVec3(view.bin, 0, vi);
            const glm::dvec3 world = glm::dvec3(rel) + origin;
            const glm::vec3 outward = glm::vec3(glm::normalize(world - centre));
            CHECK(glm::dot(nrm, outward) > 0.999f);
        }
    }
}

TEST_CASE("buildTileMeshGlb winding is CCW-from-outside", "[terrain][tile-mesh]") {
    const int size = 129;
    const int meshGrid = 32;
    std::vector<uint16_t> heights(static_cast<std::size_t>(size) * size, 32768);

    const TileKey key{4, 5, 16, 16};
    const double R = 6371000.0;
    const glm::dvec3 origin = tileToWorld(key, 0.5, 0.5, 0.0, R);
    const glm::dvec3 centre{0.0, -R, 0.0};

    auto glb = buildTileMeshGlb(heights, size, meshGrid, key, R, origin);
    auto view = parseGlb(glb);
    const int gridPts = meshGrid + 1;
    const int vertCount = gridPts * gridPts;
    const int indexCount = meshGrid * meshGrid * 6;
    // POSITION | NORMAL | TEXCOORD_0 (default emit) | INDICES  (no COLOR_0 -> landCover null)
    const std::size_t idxOff =
        static_cast<std::size_t>(vertCount) * 12u * 2u + static_cast<std::size_t>(vertCount) * 8u;

    // Every triangle must front-face outward: cross(p1-p0, p2-p0) . outward > 0.
    for (int tri = 0; tri < indexCount / 3; tri += 37) {
        const uint16_t a = readIdx(view.bin, idxOff, tri * 3 + 0);
        const uint16_t b = readIdx(view.bin, idxOff, tri * 3 + 1);
        const uint16_t c = readIdx(view.bin, idxOff, tri * 3 + 2);
        const glm::dvec3 p0 = glm::dvec3(readVec3(view.bin, 0, a)) + origin;
        const glm::dvec3 p1 = glm::dvec3(readVec3(view.bin, 0, b)) + origin;
        const glm::dvec3 p2 = glm::dvec3(readVec3(view.bin, 0, c)) + origin;
        const glm::dvec3 faceN = glm::cross(p1 - p0, p2 - p0);
        const glm::dvec3 outward = glm::normalize((p0 + p1 + p2) / 3.0 - centre);
        CHECK(glm::dot(glm::normalize(faceN), outward) > 0.0);
    }
}

TEST_CASE("buildTileMeshGlb emits TEXCOORD_0 and COLOR_0 per flags", "[terrain][tile-mesh]") {
    const int size = 129;
    const int meshGrid = 32;
    const int stride = (size - 1) / meshGrid;
    const int gridPts = meshGrid + 1;
    std::vector<uint16_t> heights(static_cast<std::size_t>(size) * size, 32768);
    std::vector<uint8_t> landCover(static_cast<std::size_t>(size) * size, 0);
    // Tag one heightmap pixel that maps to a mesh vertex (col=16, row=16 -> pixel 16*stride).
    const int pc = 16 * stride, pr = 16 * stride;
    landCover[static_cast<std::size_t>(pr) * size + pc] = 7;

    const TileKey key{4, 5, 16, 16};
    const double R = 6371000.0;
    const glm::dvec3 origin = tileToWorld(key, 0.5, 0.5, 0.0, R);

    SECTION("both attributes present") {
        auto glb = buildTileMeshGlb(heights, size, meshGrid, key, R, origin, landCover.data(), /*emitTexcoord=*/true);
        auto view = parseGlb(glb);
        CHECK(view.json.find("TEXCOORD_0") != std::string::npos);
        CHECK(view.json.find("COLOR_0") != std::string::npos);

        const int vertCount = gridPts * gridPts;
        // TEXCOORD_0 = (s, t) at vertex (16,16) -> (0.5, 0.5).
        const std::size_t texOff = static_cast<std::size_t>(vertCount) * 12u * 2u;
        const int vi = 16 * gridPts + 16;
        const glm::vec2 st = readVec2(view.bin, texOff, vi);
        CHECK(st.x == Catch::Approx(0.5f));
        CHECK(st.y == Catch::Approx(0.5f));

        // COLOR_0.r carries the WorldCover class (7) as a normalized u8.
        const std::size_t colOff = texOff + static_cast<std::size_t>(vertCount) * 8u;
        const uint8_t r = view.bin[colOff + static_cast<std::size_t>(vi) * 4u + 0u];
        const uint8_t a = view.bin[colOff + static_cast<std::size_t>(vi) * 4u + 3u];
        CHECK(r == 7u);
        CHECK(a == 255u);
    }

    SECTION("no COLOR_0 when landCover is null") {
        auto glb = buildTileMeshGlb(heights, size, meshGrid, key, R, origin, nullptr, /*emitTexcoord=*/true);
        auto view = parseGlb(glb);
        CHECK(view.json.find("TEXCOORD_0") != std::string::npos);
        CHECK(view.json.find("COLOR_0") == std::string::npos);
    }

    SECTION("no TEXCOORD_0 when disabled") {
        auto glb = buildTileMeshGlb(heights, size, meshGrid, key, R, origin, nullptr, /*emitTexcoord=*/false);
        auto view = parseGlb(glb);
        CHECK(view.json.find("TEXCOORD_0") == std::string::npos);
        CHECK(view.json.find("COLOR_0") == std::string::npos);
    }
}
