// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for buildTileMeshGlb (#471): curved cube-sphere tile meshes with true world positions,
// curvature-correct normals, and optional TEXCOORD_0 + packed-terrain TANGENT attributes (#475).

#include "render/CubeSphere.h"
#include "render/TerrainMeshBuilder.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace fl;

namespace {

// GLB layout produced by buildTileMeshGlb is deterministic non-interleaved:
// POSITION, NORMAL, [TEXCOORD_0], [TANGENT], INDICES.
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

glm::vec4 readVec4(const uint8_t* base, std::size_t byteOff, int idx) {
    glm::vec4 out;
    std::memcpy(&out, base + byteOff + static_cast<std::size_t>(idx) * 16u, 16u);
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
    // POSITION(12) | NORMAL(12) | TEXCOORD_0(8) | TANGENT(16, packed terrain data #475) | INDICES.
    // The packed TANGENT rides with TEXCOORD_0 (emitTexcoord default true), even with null landCover.
    const std::size_t idxOff = static_cast<std::size_t>(vertCount) * (12u + 12u + 8u + 16u);

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

TEST_CASE("buildTileMeshGlb skirt appends border ring and side quads", "[terrain][tile-mesh]") {
    const int size = 129;
    const int meshGrid = 32;
    const int gridPts = meshGrid + 1;
    std::vector<uint16_t> heights(static_cast<std::size_t>(size) * size, 32768);
    const TileKey key{4, 5, 16, 16};
    const double R = 6371000.0;
    const glm::dvec3 origin = tileToWorld(key, 0.5, 0.5, 0.0, R);
    const glm::dvec3 centre{0.0, -R, 0.0};
    const double skirt = 50.0;

    auto plain = buildTileMeshGlb(heights, size, meshGrid, key, R, origin, nullptr, true, 0.0);
    auto skirted = buildTileMeshGlb(heights, size, meshGrid, key, R, origin, nullptr, true, skirt);
    REQUIRE(!plain.empty());
    REQUIRE(!skirted.empty());

    // skirtDepthM == 0 must be byte-identical to the default-arg output (#471 compat).
    auto defaulted = buildTileMeshGlb(heights, size, meshGrid, key, R, origin);
    CHECK(plain == defaulted);
    CHECK(skirted.size() > plain.size());

    // Accessor counts: +4*gridPts vertices, +4*meshGrid*6 indices.
    auto view = parseGlb(skirted);
    const int vertCount = gridPts * gridPts + 4 * gridPts;
    const int indexCount = meshGrid * meshGrid * 6 + 4 * meshGrid * 6;
    CHECK(view.json.find("\"count\":" + std::to_string(vertCount)) != std::string::npos);
    CHECK(view.json.find("\"count\":" + std::to_string(indexCount)) != std::string::npos);

    // A skirt vertex sits skirtDepthM closer to the planet centre than its source
    // border vertex. The first skirt vertex duplicates main vertex (row 0, col 0).
    const glm::dvec3 srcWorld = glm::dvec3(readVec3(view.bin, 0, 0)) + origin;
    const glm::dvec3 skirtWorld = glm::dvec3(readVec3(view.bin, 0, gridPts * gridPts)) + origin;
    const double srcRad = glm::length(srcWorld - centre);
    const double skirtRad = glm::length(skirtWorld - centre);
    CHECK(srcRad - skirtRad == Catch::Approx(skirt).margin(0.01));
}

// The packed-terrain TANGENT (#475) rides with TEXCOORD_0: bin layout is
// POSITION | NORMAL | TEXCOORD_0 | TANGENT | INDICES.
TEST_CASE("buildTileMeshGlb emits TEXCOORD_0 and the packed-terrain TANGENT per flags", "[terrain][tile-mesh]") {
    const int size = 129;
    const int meshGrid = 32;
    const int stride = (size - 1) / meshGrid;
    const int gridPts = meshGrid + 1;
    const int vertCount = gridPts * gridPts;
    std::vector<uint16_t> heights(static_cast<std::size_t>(size) * size, 32768);
    std::vector<uint8_t> landCover(static_cast<std::size_t>(size) * size, 0);
    // Tag one heightmap pixel that maps to a mesh vertex (col=16, row=16 -> pixel 16*stride).
    const int pc = 16 * stride, pr = 16 * stride;
    landCover[static_cast<std::size_t>(pr) * size + pc] = 30; // ESA WorldCover grassland

    const TileKey key{4, 5, 16, 16};
    const double R = 6371000.0;
    const glm::dvec3 origin = tileToWorld(key, 0.5, 0.5, 0.0, R);
    // TANGENT block begins after POSITION(12) + NORMAL(12) + TEXCOORD_0(8) per vertex.
    const std::size_t texOff = static_cast<std::size_t>(vertCount) * 24u;
    const std::size_t tanOff = texOff + static_cast<std::size_t>(vertCount) * 8u;

    SECTION("both attributes present; TANGENT carries class/elev/detail") {
        auto glb = buildTileMeshGlb(heights, size, meshGrid, key, R, origin, landCover.data(), /*emitTexcoord=*/true);
        auto view = parseGlb(glb);
        CHECK(view.json.find("TEXCOORD_0") != std::string::npos);
        CHECK(view.json.find("TANGENT") != std::string::npos);
        CHECK(view.json.find("COLOR_0") == std::string::npos); // replaced by the packed TANGENT

        const int vi = 16 * gridPts + 16;
        const glm::vec2 st = readVec2(view.bin, texOff, vi);
        CHECK(st.x == Catch::Approx(0.5f));
        CHECK(st.y == Catch::Approx(0.5f));

        const glm::vec4 tan = readVec4(view.bin, tanOff, vi);
        CHECK(tan.x == Catch::Approx(30.0f));                       // WorldCover class
        CHECK(tan.y == Catch::Approx((0.0f + 1000.0f) / 10000.0f)); // normalized elev at h = 0
        // Detail coordinate is a metre value reduced modulo 3000 (seamless-wrap period).
        CHECK(tan.z >= 0.0f);
        CHECK(tan.z < 3000.0f);
        CHECK(tan.w >= 0.0f);
        CHECK(tan.w < 3000.0f);
    }

    SECTION("class 255 sentinel when landCover is null (fallback selection)") {
        auto glb = buildTileMeshGlb(heights, size, meshGrid, key, R, origin, nullptr, /*emitTexcoord=*/true);
        auto view = parseGlb(glb);
        CHECK(view.json.find("TANGENT") != std::string::npos);
        const glm::vec4 tan = readVec4(view.bin, tanOff, 0);
        CHECK(tan.x == Catch::Approx(255.0f));
    }

    SECTION("no TEXCOORD_0 and no TANGENT when disabled") {
        auto glb = buildTileMeshGlb(heights, size, meshGrid, key, R, origin, nullptr, /*emitTexcoord=*/false);
        auto view = parseGlb(glb);
        CHECK(view.json.find("TEXCOORD_0") == std::string::npos);
        CHECK(view.json.find("TANGENT") == std::string::npos);
    }
}

// The detail coordinate must be continuous across a shared tile edge (#475) — the same physical
// point on two adjacent tiles maps to the same global face-UV, hence the same detail metres.
TEST_CASE("buildTileMeshGlb detail coordinate is continuous across a shared tile edge", "[terrain][tile-mesh]") {
    const int size = 129;
    const int meshGrid = 32;
    const int gridPts = meshGrid + 1;
    const int vertCount = gridPts * gridPts;
    std::vector<uint16_t> heights(static_cast<std::size_t>(size) * size, 32768);
    const double R = 6371000.0;
    const std::size_t tanOff = static_cast<std::size_t>(vertCount) * 32u; // pos+nrm+tex per vertex

    // Two horizontally-adjacent tiles on the same face (i, i+1). The +U edge of the left tile
    // coincides with the -U edge of the right tile.
    const TileKey left{2, 6, 20, 24};
    const TileKey right{2, 6, 21, 24};
    const glm::dvec3 oL = tileToWorld(left, 0.5, 0.5, 0.0, R);
    const glm::dvec3 oR = tileToWorld(right, 0.5, 0.5, 0.0, R);
    // Keep the GLB vectors alive — parseGlb stores a pointer into them.
    const std::vector<uint8_t> glbL = buildTileMeshGlb(heights, size, meshGrid, left, R, oL, nullptr, true);
    const std::vector<uint8_t> glbR = buildTileMeshGlb(heights, size, meshGrid, right, R, oR, nullptr, true);
    auto gL = parseGlb(glbL);
    auto gR = parseGlb(glbR);

    // For each row, the left tile's last column (col=meshGrid) and the right tile's first column
    // (col=0) are the same world point -> identical detail metres.
    for (int row = 0; row <= meshGrid; ++row) {
        const int viL = row * gridPts + meshGrid;
        const int viR = row * gridPts + 0;
        const glm::vec4 tL = readVec4(gL.bin, tanOff, viL);
        const glm::vec4 tR = readVec4(gR.bin, tanOff, viR);
        CHECK(tL.z == Catch::Approx(tR.z).margin(1e-3));
        CHECK(tL.w == Catch::Approx(tR.w).margin(1e-3));
    }
}
