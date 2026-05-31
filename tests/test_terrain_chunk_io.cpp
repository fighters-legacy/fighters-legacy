// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "render/BuiltinGeometry.h"
#include "render/ProceduralTerrainChunk.h"
#include "render/TerrainChunkIO.h"
#include "render/TerrainManifest.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace fl;

// ---------------------------------------------------------------------------
// TerrainChunkIO — PNG decode
// ---------------------------------------------------------------------------

TEST_CASE("decodeTerrainChunkPng rejects null data") {
    int w = 0, h = 0;
    CHECK(decodeTerrainChunkPng(nullptr, 128, &w, &h).empty());
}

TEST_CASE("decodeTerrainChunkPng rejects empty buffer") {
    const uint8_t dummy = 0;
    int w = 0, h = 0;
    CHECK(decodeTerrainChunkPng(&dummy, 0, &w, &h).empty());
}

TEST_CASE("decodeTerrainChunkPng rejects corrupt bytes") {
    static const uint8_t junk[] = {0x00, 0x01, 0x02, 0x03, 0xFF, 0xFE};
    int w = 0, h = 0;
    CHECK(decodeTerrainChunkPng(junk, sizeof(junk), &w, &h).empty());
}

// ---------------------------------------------------------------------------
// TerrainChunkIO — binary cache round-trip
// ---------------------------------------------------------------------------

TEST_CASE("writeTerrainChunkCache / readTerrainChunkCache round-trip") {
    // Build a small test heightmap.
    constexpr int W = 4, H = 3;
    const uint16_t src[W * H] = {
        1000, 2000, 3000, 4000, 5000, 6000, 7000, 8000, 9000, 10000, 11000, 12000,
    };

    const auto tmpPath = (std::filesystem::temp_directory_path() / "fl_test_chunk_io.u16").string();

    REQUIRE(writeTerrainChunkCache(tmpPath, src, W, H));

    int outW = 0, outH = 0;
    const auto result = readTerrainChunkCache(tmpPath, &outW, &outH);

    REQUIRE(outW == W);
    REQUIRE(outH == H);
    REQUIRE(result.size() == static_cast<size_t>(W * H));
    for (int i = 0; i < W * H; ++i)
        CHECK(result[i] == src[i]);

    std::filesystem::remove(tmpPath);
}

TEST_CASE("writeTerrainChunkCache creates missing parent directories") {
    const auto dir = std::filesystem::temp_directory_path() / "fl_test_chunk_io_subdir" / "lod0";
    const auto path = (dir / "chunk_000000_000000.u16").string();

    std::filesystem::remove_all(dir.parent_path());

    const uint16_t val = 33318u; // elevation 550 m encoded
    REQUIRE(writeTerrainChunkCache(path, &val, 1, 1));
    REQUIRE(std::filesystem::exists(path));

    std::filesystem::remove_all(dir.parent_path());
}

TEST_CASE("readTerrainChunkCache returns empty for missing file") {
    int w = 0, h = 0;
    CHECK(readTerrainChunkCache("/nonexistent/path/chunk.u16", &w, &h).empty());
}

TEST_CASE("readTerrainChunkCache returns empty for wrong magic") {
    const auto path = (std::filesystem::temp_directory_path() / "fl_bad_magic.u16").string();
    // Write garbage that starts with the wrong magic.
    {
        std::ofstream f(path, std::ios::binary);
        const uint32_t badMagic = 0xDEADBEEFu;
        const uint16_t w = 1, h = 1, v = 0;
        f.write(reinterpret_cast<const char*>(&badMagic), 4);
        f.write(reinterpret_cast<const char*>(&w), 2);
        f.write(reinterpret_cast<const char*>(&h), 2);
        f.write(reinterpret_cast<const char*>(&v), 2);
    }
    int w = 0, h = 0;
    CHECK(readTerrainChunkCache(path, &w, &h).empty());
    std::filesystem::remove(path);
}

// ---------------------------------------------------------------------------
// ProceduralTerrainChunk
// ---------------------------------------------------------------------------

TEST_CASE("generateProceduralChunk returns 513x513 elements") {
    const auto manifest = builtinWorldTerrainManifest();
    const auto chunk = generateProceduralChunk(0, 0, manifest, kBuiltinProceduralParams);
    CHECK(chunk.size() == 513u * 513u);
}

TEST_CASE("generateProceduralChunk values are in valid uint16 range") {
    const auto manifest = builtinWorldTerrainManifest();
    const auto chunk = generateProceduralChunk(3, 3, manifest, kBuiltinProceduralParams);
    REQUIRE(!chunk.empty());
    for (auto v : chunk) {
        CHECK(v <= 65535u);
    }
}

TEST_CASE("generateProceduralChunk elevation range matches params") {
    // base=550, amplitude=150 → elevations in [400, 700] m
    // encoded: [32768+400, 32768+700] = [33168, 33468]
    const auto manifest = builtinWorldTerrainManifest();
    const auto chunk = generateProceduralChunk(0, 0, manifest, kBuiltinProceduralParams);
    REQUIRE(!chunk.empty());
    for (auto v : chunk) {
        CHECK(v >= 33168u);
        CHECK(v <= 33468u);
    }
}

TEST_CASE("generateProceduralChunk is deterministic") {
    const auto manifest = builtinWorldTerrainManifest();
    const auto a = generateProceduralChunk(5, 7, manifest, kBuiltinProceduralParams);
    const auto b = generateProceduralChunk(5, 7, manifest, kBuiltinProceduralParams);
    REQUIRE(a.size() == b.size());
    CHECK(a == b);
}

TEST_CASE("generateProceduralChunk is seamless on X axis") {
    // Rightmost column of (cx=0, cy=0) must equal leftmost column of (cx=1, cy=0).
    const auto manifest = builtinWorldTerrainManifest();
    const auto left = generateProceduralChunk(0, 0, manifest, kBuiltinProceduralParams);
    const auto right = generateProceduralChunk(1, 0, manifest, kBuiltinProceduralParams);
    REQUIRE(left.size() == 513u * 513u);
    REQUIRE(right.size() == 513u * 513u);
    for (int row = 0; row < 513; ++row) {
        const uint16_t lv = left[row * 513 + 512]; // last column of left chunk
        const uint16_t rv = right[row * 513 + 0];  // first column of right chunk
        CHECK(lv == rv);
    }
}

TEST_CASE("generateProceduralChunk is seamless on Z axis") {
    // Bottom row of (cx=0, cy=0) must equal top row of (cx=0, cy=1).
    const auto manifest = builtinWorldTerrainManifest();
    const auto bottom = generateProceduralChunk(0, 0, manifest, kBuiltinProceduralParams);
    const auto top = generateProceduralChunk(0, 1, manifest, kBuiltinProceduralParams);
    REQUIRE(bottom.size() == 513u * 513u);
    REQUIRE(top.size() == 513u * 513u);
    for (int col = 0; col < 513; ++col) {
        const uint16_t bv = bottom[512 * 513 + col]; // last row of bottom chunk
        const uint16_t tv = top[0 * 513 + col];      // first row of top chunk
        CHECK(bv == tv);
    }
}

// ---------------------------------------------------------------------------
// builtinWorldTerrainManifest
// ---------------------------------------------------------------------------

TEST_CASE("builtinWorldTerrainManifest has expected values") {
    const auto m = builtinWorldTerrainManifest();
    CHECK(m.terrainId == "world");
    CHECK(m.chunkSizeM == Catch::Approx(15360.f));
    CHECK(m.gridWidth == -1);
    CHECK(m.gridHeight == -1);
    CHECK(m.originX == Catch::Approx(-7680.0));
    CHECK(m.originZ == Catch::Approx(-7680.0));
}
