// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "render/BuiltinGeometry.h"
#include "render/ProceduralTerrainChunk.h"
#include "render/TerrainChunkIO.h"
#include "render/TerrainManifest.h"
#include "temp_path.h"

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

    const auto tmpPath = fl::test::uniqueTempPath("fl_test_chunk_io", ".u16").string();

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
    // uniqueTempPath creates nothing, so the parent really is missing -- which is the point here.
    const auto dir = fl::test::uniqueTempPath("fl_test_chunk_io_subdir") / "lod0";
    const auto path = (dir / "chunk_000000_000000.u16").string();

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
    const auto path = fl::test::uniqueTempPath("fl_bad_magic", ".u16").string();
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
// ProceduralTerrainChunk (cube-sphere tiles, #472)
// ---------------------------------------------------------------------------

TEST_CASE("generateProceduralTile returns kTileHeightmapSize^2 elements") {
    const auto tile = generateProceduralTile(TileKey{2, 0, 0, 0}, 6'371'000.0, kBuiltinProceduralParams);
    CHECK(tile.size() == static_cast<std::size_t>(kTileHeightmapSize) * kTileHeightmapSize);
}

TEST_CASE("generateProceduralTile elevation range matches params") {
    // base=550, amplitude=150 -> elevations in [400, 700] m
    // encoded: [32768+400, 32768+700] = [33168, 33468]
    const auto tile = generateProceduralTile(TileKey{4, 2, 1, 3}, 6'371'000.0, kBuiltinProceduralParams);
    REQUIRE(!tile.empty());
    for (auto v : tile) {
        CHECK(v >= 33168u);
        CHECK(v <= 33468u);
    }
}

TEST_CASE("generateProceduralTile is deterministic") {
    const TileKey key{1, 5, 7, 11};
    const auto a = generateProceduralTile(key, 6'371'000.0, kBuiltinProceduralParams);
    const auto b = generateProceduralTile(key, 6'371'000.0, kBuiltinProceduralParams);
    REQUIRE(a.size() == b.size());
    CHECK(a == b);
}

TEST_CASE("generateProceduralTile is seamless across sibling tile edges") {
    // Two level-1 siblings on face 4 share the u=0.5 edge: the last sample column of
    // the left tile equals the first column of the right tile, row for row.
    const auto left = generateProceduralTile(TileKey{4, 1, 0, 0}, 6'371'000.0, kBuiltinProceduralParams);
    const auto right = generateProceduralTile(TileKey{4, 1, 1, 0}, 6'371'000.0, kBuiltinProceduralParams);
    const int s = kTileHeightmapSize;
    for (int row = 0; row < s; ++row) {
        CHECK(left[static_cast<std::size_t>(row) * s + (s - 1)] == right[static_cast<std::size_t>(row) * s]);
    }
}

TEST_CASE("generateProceduralTile matches its parent at shared sample points") {
    // A child tile's even samples coincide with half of its parent's samples: the
    // shared directions must produce identical values (coarse-to-fine consistency).
    const TileKey parentKey{4, 2, 1, 1};
    const auto parentTile = generateProceduralTile(parentKey, 6'371'000.0, kBuiltinProceduralParams);
    const auto childTile = generateProceduralTile(child(parentKey, 0), 6'371'000.0, kBuiltinProceduralParams);
    const int s = kTileHeightmapSize;
    // Child quadrant 0 covers the parent's lower-left quarter: child sample (2c, 2r)
    // = parent sample (c, r) for c, r in [0, (s-1)/2].
    for (int r = 0; r <= (s - 1) / 2; r += 8) {
        for (int c = 0; c <= (s - 1) / 2; c += 8) {
            CHECK(childTile[static_cast<std::size_t>(2 * r) * s + 2 * c] ==
                  parentTile[static_cast<std::size_t>(r) * s + c]);
        }
    }
}

// ---------------------------------------------------------------------------
// builtinWorldTerrainManifest
// ---------------------------------------------------------------------------

TEST_CASE("builtinWorldTerrainManifest has expected values") {
    const auto m = builtinWorldTerrainManifest();
    CHECK(m.terrainId == "world");
    CHECK(m.maxTileLevel == 12);
}

// ---------------------------------------------------------------------------
// TerrainChunkIO — malformed-PNG hardening (regression, #94 fuzzing)
// ---------------------------------------------------------------------------

TEST_CASE("decodeTerrainChunkPng rejects a PNG whose chunk length overruns the buffer") {
    // Valid 8-byte signature + IHDR (4x4, 16-bit gray) + an IDAT chunk declaring ~738 MB of data in a
    // tiny file. Without the chunk-length sanity check, stb_image allocates the declared IDAT length
    // up front — a memory-exhaustion DoS from an untrusted content-pack chunk.
    static const uint8_t png[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, // signature
                                  0x00, 0x00, 0x00, 0x0D, 'I',  'H',  'D',  'R',  // IHDR len=13
                                  0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x04, // width=4 height=4
                                  0x10, 0x00, 0x00, 0x00, 0x00,                   // bitdepth=16, gray, no interlace
                                  0xDC, 0x0A, 0x1D, 0xE1,                         // IHDR crc
                                  0x2C, 0x00, 0x00, 0x00, 'I',  'D',  'A',  'T'}; // IDAT len=0x2C000000 (~738 MB)
    int w = 0, h = 0;
    CHECK(decodeTerrainChunkPng(png, sizeof(png), &w, &h).empty());
}

TEST_CASE("decodeTerrainChunkPng rejects non-PNG input (no stb format auto-probe)") {
    // A JPEG magic must not reach stb_image's other, more fragile format decoders (PSD/PNM/HDR/...).
    static const uint8_t jpeg[] = {0xFF, 0xD8, 0xFF, 0xE0, 'J', 'F', 'I', 'F', 0, 1, 2, 3, 4, 5, 6, 7};
    int w = 0, h = 0;
    CHECK(decodeTerrainChunkPng(jpeg, sizeof(jpeg), &w, &h).empty());
}
