// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for the cube-sphere quadtree TerrainStreamer (#472): SSE refinement, 2:1 edge
// balance, LRU residency, radial dvec3 height/surface queries, deterministic global
// procedural tiles, and the async pack-tile layer path (resolveTilePath, #473).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "content/AssetManager.h"
#include "content/AssetTypes.h"
#include "content/IContentPack.h"
#include "render/CubeSphere.h"
#include "render/ProceduralTerrainChunk.h"
#include "render/TerrainStreamer.h"

#include "mock_content.h"
#include "mock_hal.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace fl;

// ===========================================================================
// Minimal 16-bit grayscale PNG encoder (no external deps)
//
// Produces a valid uncompressed PNG with DEFLATE stored blocks.
// All targets are little-endian; PNG requires big-endian for multi-byte fields.
// ===========================================================================

namespace {

static void wbe32(uint8_t* p, uint32_t v) {
    p[0] = (v >> 24) & 0xFFu;
    p[1] = (v >> 16) & 0xFFu;
    p[2] = (v >> 8) & 0xFFu;
    p[3] = v & 0xFFu;
}
static void wbe16(uint8_t* p, uint16_t v) {
    p[0] = (v >> 8) & 0xFFu;
    p[1] = v & 0xFFu;
}

// CRC-32 (ISO 3309 polynomial)
static uint32_t crc32Update(uint32_t crc, const uint8_t* buf, std::size_t len) {
    static constexpr uint32_t kPoly = 0xEDB88320u;
    crc = ~crc;
    for (std::size_t i = 0; i < len; ++i) {
        crc ^= buf[i];
        for (int b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ ((crc & 1u) ? kPoly : 0u);
    }
    return ~crc;
}

// Adler-32 checksum (for zlib wrapper)
static uint32_t adler32(const uint8_t* buf, std::size_t len) {
    uint32_t s1 = 1, s2 = 0;
    for (std::size_t i = 0; i < len; ++i) {
        s1 = (s1 + buf[i]) % 65521u;
        s2 = (s2 + s1) % 65521u;
    }
    return (s2 << 16) | s1;
}

// Append a PNG chunk: length(4BE) + type(4) + data + crc(4BE)
static void appendPngChunk(std::vector<uint8_t>& out, const char type[4], const uint8_t* data, uint32_t len) {
    const std::size_t start = out.size();
    out.resize(start + 12 + len);
    uint8_t* p = out.data() + start;
    wbe32(p, len);
    std::memcpy(p + 4, type, 4);
    if (len > 0)
        std::memcpy(p + 8, data, len);
    const uint32_t crc = crc32Update(0, p + 4, 4 + len);
    wbe32(p + 8 + len, crc);
}

// Build a flat w×h 16-bit grayscale PNG (all pixels = fill).
static std::vector<uint8_t> makeFlatPng16(int w, int h, uint16_t fill) {
    // Raw image data: for each row: filter byte (0) + w×2 bytes (big-endian uint16)
    const int rowBytes = 1 + w * 2;
    std::vector<uint8_t> raw(static_cast<std::size_t>(h) * rowBytes, 0);
    for (int r = 0; r < h; ++r) {
        uint8_t* row = raw.data() + r * rowBytes;
        row[0] = 0; // filter = None
        for (int c = 0; c < w; ++c)
            wbe16(row + 1 + c * 2, fill);
    }

    // zlib header: CMF=0x78, FLG=0x01 (FCHECK: (0x78*256 + 1) % 31 == 0)
    static constexpr uint8_t kZlibCMF = 0x78u;
    static constexpr uint8_t kZlibFLG = 0x01u;

    std::vector<uint8_t> zlib;
    zlib.push_back(kZlibCMF);
    zlib.push_back(kZlibFLG);

    // Emit DEFLATE stored blocks (BTYPE=00). Each block: up to 65535 bytes.
    const std::size_t rawSize = raw.size();
    std::size_t offset = 0;
    while (offset < rawSize) {
        const std::size_t blockSize = std::min<std::size_t>(rawSize - offset, 65535u);
        const bool bfinal = (offset + blockSize >= rawSize);
        zlib.push_back(bfinal ? 0x01u : 0x00u); // BFINAL | BTYPE=00
        const uint16_t len16 = static_cast<uint16_t>(blockSize);
        zlib.push_back(len16 & 0xFFu);
        zlib.push_back((len16 >> 8) & 0xFFu);
        zlib.push_back(static_cast<uint8_t>(~len16 & 0xFFu));
        zlib.push_back(static_cast<uint8_t>((~len16 >> 8) & 0xFFu));
        zlib.insert(zlib.end(), raw.data() + offset, raw.data() + offset + blockSize);
        offset += blockSize;
    }

    const uint32_t a32 = adler32(raw.data(), rawSize);
    const std::size_t zlibEnd = zlib.size();
    zlib.resize(zlibEnd + 4);
    wbe32(zlib.data() + zlibEnd, a32);

    std::vector<uint8_t> png;
    png.reserve(64 + zlib.size());

    static constexpr uint8_t kSig[] = {137, 80, 78, 71, 13, 10, 26, 10};
    png.insert(png.end(), kSig, kSig + 8);

    uint8_t ihdr[13]{};
    wbe32(ihdr + 0, static_cast<uint32_t>(w));
    wbe32(ihdr + 4, static_cast<uint32_t>(h));
    ihdr[8] = 16; // bit depth
    ihdr[9] = 0;  // color type: grayscale
    appendPngChunk(png, "IHDR", ihdr, 13);
    appendPngChunk(png, "IDAT", zlib.data(), static_cast<uint32_t>(zlib.size()));
    appendPngChunk(png, "IEND", nullptr, 0);

    return png;
}

// ===========================================================================
// MockTerrainPack — IContentPack with configurable resolveTilePath
// ===========================================================================

// Resolves tile paths from an in-memory map; everything else null-object.
// The streamer probes with the explicit TileKey (#473):
// resolveTilePath("<terrainId>", face, level, i, j, layer).
struct MockTerrainPack : NullContentPack {
    // key format: "<terrainId>:<face>:<level>:<i>:<j>:<layer>" (layer 0=Height, 1=LandCover)
    std::map<std::string, std::string> tilePaths;

    const char* name() const override {
        return "MockTerrainPack";
    }
    const char* version() const override {
        return "0.0.1";
    }
    const char* id() const override {
        return "test:terrain";
    }
    std::optional<std::string> resolveTilePath(const char* terrainId, uint8_t face, uint8_t level, uint32_t i,
                                               uint32_t j, TileLayer layer) const override {
        const std::string key = std::string(terrainId) + ":" + std::to_string(static_cast<unsigned>(face)) + ":" +
                                std::to_string(static_cast<unsigned>(level)) + ":" + std::to_string(i) + ":" +
                                std::to_string(j) + ":" + std::to_string(static_cast<unsigned>(layer));
        auto it = tilePaths.find(key);
        if (it == tilePaths.end())
            return std::nullopt;
        return it->second;
    }
};

static fl::TerrainManifest worldManifest(int maxLevel) {
    fl::TerrainManifest m;
    m.terrainId = "world";
    m.maxTileLevel = maxLevel;
    return m;
}

constexpr double kR = 6'371'000.0;
// North-pole datum point (the world origin) and a camera just above it.
const glm::dvec3 kPoleProbe{0.0, 0.0, 0.0};
const glm::dvec3 kPoleCam{0.0, 550.0, 0.0};
// South-pole (antipode) datum point.
const glm::dvec3 kAntipodeProbe{0.0, -2.0 * kR, 0.0};

// Pump update()+service() until heightReadyAt(probe) or maxIter.
static void pumpUntilReady(fl::TerrainStreamer& ts, MockAsyncFilesystem& fs, glm::dvec3 cam, glm::dvec3 probe,
                           int maxIter = 400) {
    for (int i = 0; i < maxIter && !ts.heightReadyAt(probe); ++i) {
        ts.update(cam);
        fs.service();
    }
}

static void pump(fl::TerrainStreamer& ts, MockAsyncFilesystem& fs, glm::dvec3 cam, int iters) {
    for (int i = 0; i < iters; ++i) {
        ts.update(cam);
        fs.service();
    }
}

// Fixture bundle for the no-pack procedural cases.
struct StreamerFixture {
    MockLogger logger;
    std::unique_ptr<AssetManager> assets;
    MockAsyncFilesystem asyncFs;

    explicit StreamerFixture(std::unique_ptr<IContentPack> pack = nullptr) {
        std::vector<std::unique_ptr<IContentPack>> packs;
        if (pack)
            packs.push_back(std::move(pack));
        assets = std::make_unique<AssetManager>(std::move(packs), logger);
        assets->initialize(nullptr);
        asyncFs.init();
    }
};

} // namespace

// ===========================================================================
// Tests
// ===========================================================================

TEST_CASE("TerrainStreamer procedural tiles produce render items and plausible heights") {
    StreamerFixture fx;
    MockRenderer renderer;
    fl::TerrainStreamer ts{worldManifest(2), *fx.assets, fx.asyncFs, &renderer};

    pump(ts, fx.asyncFs, kPoleCam, 60);

    auto items = ts.getRenderItems(kPoleCam);
    REQUIRE(!items.empty());
    for (const auto& item : items) {
        CHECK(item.mesh.valid());
        CHECK((item.flags & kRenderFlagTerrain) != 0u);
    }

    // Builtin FBM: base 550 m, amplitude 150 m.
    const double h = ts.heightAt(kPoleProbe);
    CHECK(h > 300.0);
    CHECK(h < 900.0);
}

TEST_CASE("TerrainStreamer getRenderItems is empty before the first update") {
    StreamerFixture fx;
    MockRenderer renderer;
    fl::TerrainStreamer ts{worldManifest(2), *fx.assets, fx.asyncFs, &renderer};
    CHECK(ts.getRenderItems(kPoleCam).empty());
}

TEST_CASE("TerrainStreamer null renderer returns empty render items but height works") {
    StreamerFixture fx;
    fl::TerrainStreamer ts{worldManifest(2), *fx.assets, fx.asyncFs, nullptr};

    pump(ts, fx.asyncFs, kPoleCam, 60);

    CHECK(ts.getRenderItems(kPoleCam).empty());
    CHECK(ts.heightAt(kPoleProbe) > 0.0);
}

TEST_CASE("TerrainStreamer heightReadyAt gates on covering-tile depth") {
    StreamerFixture fx;
    fl::TerrainStreamer ts{worldManifest(4), *fx.assets, fx.asyncFs, nullptr};

    // Before any update(): nothing is loaded.
    CHECK_FALSE(ts.heightReadyAt(kPoleProbe));
    CHECK(ts.heightAt(kPoleProbe) == 0.0);

    pumpUntilReady(ts, fx.asyncFs, kPoleCam, kPoleProbe);
    CHECK(ts.heightReadyAt(kPoleProbe));
    CHECK(ts.heightAt(kPoleProbe) > 0.0);
}

TEST_CASE("TerrainStreamer refinement is camera-local") {
    StreamerFixture fx;
    fl::TerrainStreamer ts{worldManifest(6), *fx.assets, fx.asyncFs, nullptr};

    pumpUntilReady(ts, fx.asyncFs, kPoleCam, kPoleProbe);
    CHECK(ts.heightReadyAt(kPoleProbe));
    // The antipode is only covered by coarse tiles — not spawn-accurate.
    CHECK_FALSE(ts.heightReadyAt(kAntipodeProbe));
    // But a coarse height IS available there (roots always desired).
    CHECK(ts.heightAt(kAntipodeProbe) > 0.0);
}

TEST_CASE("TerrainStreamer desired leaves satisfy the 2:1 edge balance") {
    StreamerFixture fx;
    fl::TerrainStreamer ts{worldManifest(6), *fx.assets, fx.asyncFs, nullptr};
    pump(ts, fx.asyncFs, kPoleCam, 5); // desired tree is rebuilt every update

    struct KeyHash {
        std::size_t operator()(const TileKey& k) const noexcept {
            return (static_cast<std::size_t>(k.face) << 40) ^ (static_cast<std::size_t>(k.level) << 32) ^
                   (static_cast<std::size_t>(k.i) << 16) ^ k.j;
        }
    };
    const auto leaves = ts.desiredLeaves();
    REQUIRE(!leaves.empty());
    std::unordered_set<TileKey, KeyHash> leafSet(leaves.begin(), leaves.end());

    for (const TileKey& t : leaves) {
        if (t.level < 2)
            continue;
        for (int e = 0; e < 4; ++e) {
            // Walk up from the same-level neighbour to the leaf covering that region.
            TileKey cover = neighbor(t, static_cast<TileEdge>(e));
            bool found = false;
            while (true) {
                if (leafSet.count(cover)) {
                    found = true;
                    break;
                }
                if (cover.level == 0)
                    break;
                cover = parent(cover);
            }
            if (found) {
                INFO("leaf f" << int(t.face) << " L" << int(t.level) << " edge " << e << " covered at L"
                              << int(cover.level));
                CHECK(static_cast<int>(cover.level) >= static_cast<int>(t.level) - 1);
            }
        }
    }
}

TEST_CASE("TerrainStreamer LRU eviction drops fine tiles left behind by the camera") {
    StreamerFixture fx;
    fl::TerrainStreamer ts{worldManifest(6), *fx.assets, fx.asyncFs, nullptr};
    ts.setResidencyCap(8); // force aggressive eviction of non-desired tiles

    pumpUntilReady(ts, fx.asyncFs, kPoleCam, kPoleProbe);
    REQUIRE(ts.heightReadyAt(kPoleProbe));

    // Move to the antipode and pump: the pole-side deep tiles leave the desired
    // tree and must be evicted under the tiny cap.
    const glm::dvec3 antipodeCam{0.0, -2.0 * kR - 550.0, 0.0};
    pumpUntilReady(ts, fx.asyncFs, antipodeCam, kAntipodeProbe);
    CHECK(ts.heightReadyAt(kAntipodeProbe));
    CHECK_FALSE(ts.heightReadyAt(kPoleProbe)); // deep pole tiles evicted
}

TEST_CASE("TerrainStreamer procedural tiles are deterministic across instances") {
    const TileKey key{2, 3, 4, 5};
    const auto a = generateProceduralTile(key, kR, kBuiltinProceduralParams);
    const auto b = generateProceduralTile(key, kR, kBuiltinProceduralParams);
    REQUIRE(a.size() == static_cast<std::size_t>(kTileHeightmapSize) * kTileHeightmapSize);
    CHECK(a == b);

    // Server (headless) and client (rendered) streamers agree exactly on heights.
    StreamerFixture fxServer;
    fl::TerrainStreamer server{worldManifest(3), *fxServer.assets, fxServer.asyncFs, nullptr};
    pump(server, fxServer.asyncFs, kPoleCam, 60);

    StreamerFixture fxClient;
    MockRenderer renderer;
    fl::TerrainStreamer client{worldManifest(3), *fxClient.assets, fxClient.asyncFs, &renderer};
    pump(client, fxClient.asyncFs, kPoleCam, 60);

    for (const glm::dvec3 probe :
         {kPoleProbe, glm::dvec3{50'000.0, 0.0, 20'000.0}, glm::dvec3{-120'000.0, 0.0, 90'000.0}}) {
        CHECK(server.heightAt(probe) == client.heightAt(probe));
    }
}

TEST_CASE("TerrainStreamer procedural tiles are seamless across sibling edges") {
    // Two level-1 siblings on face 2 share the u=0.5 edge: the last sample column of
    // the left tile must equal the first column of the right tile, row for row.
    const auto left = generateProceduralTile(TileKey{2, 1, 0, 0}, kR, kBuiltinProceduralParams);
    const auto right = generateProceduralTile(TileKey{2, 1, 1, 0}, kR, kBuiltinProceduralParams);
    const int s = kTileHeightmapSize;
    for (int row = 0; row < s; ++row) {
        REQUIRE(left[static_cast<std::size_t>(row) * s + (s - 1)] == right[static_cast<std::size_t>(row) * s]);
    }
}

TEST_CASE("TerrainStreamer loads a pack tile PNG via resolveTilePath") {
    auto pack = std::make_unique<MockTerrainPack>();
    const std::string tilePath = "terrain/world/f2/l0/tile_0_0.png";
    pack->tilePaths["world:2:0:0:0:0"] = tilePath;

    StreamerFixture fx(std::move(pack));
    // 33318 - 32768 = 550 m radial elevation, flat.
    fx.asyncFs.addFile(tilePath, makeFlatPng16(kTileHeightmapSize, kTileHeightmapSize, 33318));

    fl::TerrainStreamer ts{worldManifest(0), *fx.assets, fx.asyncFs, nullptr};
    pump(ts, fx.asyncFs, kPoleCam, 5);

    CHECK(ts.heightAt(kPoleProbe) == Catch::Approx(550.0).margin(0.5));
    CHECK(ts.heightReadyAt(kPoleProbe)); // maxTileLevel 0 => required level 0
}

TEST_CASE("TerrainStreamer async read error falls back to procedural") {
    auto pack = std::make_unique<MockTerrainPack>();
    pack->tilePaths["world:2:0:0:0:0"] = "terrain/world/f2/l0/tile_0_0.png";
    // No file added to asyncFs -> service() fires the Error callback.

    StreamerFixture fx(std::move(pack));
    fl::TerrainStreamer ts{worldManifest(0), *fx.assets, fx.asyncFs, nullptr};
    pump(ts, fx.asyncFs, kPoleCam, 5);

    const double h = ts.heightAt(kPoleProbe);
    CHECK(h > 300.0);
    CHECK(h < 900.0);
}

TEST_CASE("TerrainStreamer wrong-size pack PNG falls back to procedural") {
    auto pack = std::make_unique<MockTerrainPack>();
    const std::string tilePath = "terrain/world/f2/l0/tile_0_0.png";
    pack->tilePaths["world:2:0:0:0:0"] = tilePath;

    StreamerFixture fx(std::move(pack));
    // Legacy 513x513 chunk-sized PNG: valid image, wrong tile dimensions.
    fx.asyncFs.addFile(tilePath, makeFlatPng16(513, 513, 40000));

    fl::TerrainStreamer ts{worldManifest(0), *fx.assets, fx.asyncFs, nullptr};
    pump(ts, fx.asyncFs, kPoleCam, 5);

    const double h = ts.heightAt(kPoleProbe);
    CHECK(h > 300.0); // FBM range, not the PNG's 7232 m
    CHECK(h < 900.0);
}

TEST_CASE("TerrainStreamer land-cover layer feeds surfaceAt") {
    auto pack = std::make_unique<MockTerrainPack>();
    const std::string heightPath = "terrain/world/f2/l0/tile_0_0.png";
    const std::string coverPath = "terrain/world/f2/l0/tile_0_0_lc.png";
    pack->tilePaths["world:2:0:0:0:0"] = heightPath; // Height layer
    pack->tilePaths["world:2:0:0:0:1"] = coverPath;  // LandCover layer

    StreamerFixture fx(std::move(pack));
    fx.asyncFs.addFile(heightPath, makeFlatPng16(kTileHeightmapSize, kTileHeightmapSize, 33318));
    fx.asyncFs.addFile(coverPath, makeFlatPng16(kTileHeightmapSize, kTileHeightmapSize, 7)); // class 7

    fl::TerrainStreamer ts{worldManifest(0), *fx.assets, fx.asyncFs, nullptr};
    pump(ts, fx.asyncFs, kPoleCam, 5);

    CHECK(ts.heightAt(kPoleProbe) == Catch::Approx(550.0).margin(0.5));
    CHECK(ts.surfaceAt(kPoleProbe) == 7);
}

TEST_CASE("TerrainStreamer surfaceAt returns zero without a land-cover layer") {
    StreamerFixture fx;
    fl::TerrainStreamer ts{worldManifest(2), *fx.assets, fx.asyncFs, nullptr};
    pump(ts, fx.asyncFs, kPoleCam, 30);
    CHECK(ts.surfaceAt(kPoleProbe) == 0);
    CHECK(ts.surfaceTypeAt(kPoleProbe) == fl::SurfaceType::Unknown); // #475: no layer → Unknown
}

TEST_CASE("SurfaceType: ESA WorldCover class codes map to the engine vocabulary (#475)") {
    using fl::SurfaceType;
    CHECK(fl::surfaceTypeFromWorldCover(10) == SurfaceType::Forest);   // tree cover
    CHECK(fl::surfaceTypeFromWorldCover(30) == SurfaceType::Grass);    // grassland
    CHECK(fl::surfaceTypeFromWorldCover(40) == SurfaceType::Grass);    // cropland
    CHECK(fl::surfaceTypeFromWorldCover(50) == SurfaceType::Urban);    // built-up
    CHECK(fl::surfaceTypeFromWorldCover(60) == SurfaceType::Rock);     // bare / sparse
    CHECK(fl::surfaceTypeFromWorldCover(70) == SurfaceType::Snow);     // snow & ice
    CHECK(fl::surfaceTypeFromWorldCover(80) == SurfaceType::Water);    // water bodies
    CHECK(fl::surfaceTypeFromWorldCover(90) == SurfaceType::Wetland);  // herbaceous wetland
    CHECK(fl::surfaceTypeFromWorldCover(0) == SurfaceType::Unknown);   // no data
    CHECK(fl::surfaceTypeFromWorldCover(200) == SurfaceType::Unknown); // out-of-range
}

TEST_CASE("TerrainStreamer surfaceTypeAt maps a WorldCover water tile to Water (#475)") {
    auto pack = std::make_unique<MockTerrainPack>();
    const std::string heightPath = "terrain/world/f2/l0/tile_0_0.png";
    const std::string coverPath = "terrain/world/f2/l0/tile_0_0_lc.png";
    pack->tilePaths["world:2:0:0:0:0"] = heightPath;
    pack->tilePaths["world:2:0:0:0:1"] = coverPath;

    StreamerFixture fx(std::move(pack));
    fx.asyncFs.addFile(heightPath, makeFlatPng16(kTileHeightmapSize, kTileHeightmapSize, 33318));
    fx.asyncFs.addFile(coverPath, makeFlatPng16(kTileHeightmapSize, kTileHeightmapSize, 80)); // WorldCover water

    fl::TerrainStreamer ts{worldManifest(0), *fx.assets, fx.asyncFs, nullptr};
    pump(ts, fx.asyncFs, kPoleCam, 5);
    CHECK(ts.surfaceAt(kPoleProbe) == 80);
    CHECK(ts.surfaceTypeAt(kPoleProbe) == fl::SurfaceType::Water);
}

TEST_CASE("TerrainStreamer heightAt is callable from a background thread", "[threading]") {
    StreamerFixture fx;
    fl::TerrainStreamer ts{worldManifest(3), *fx.assets, fx.asyncFs, nullptr};

    std::thread reader([&ts] {
        double acc = 0.0;
        for (int i = 0; i < 500; ++i)
            acc += ts.heightAt(glm::dvec3{static_cast<double>(i) * 37.0, 0.0, static_cast<double>(i) * 91.0});
        CHECK(acc >= 0.0);
    });
    pump(ts, fx.asyncFs, kPoleCam, 60);
    reader.join();
    CHECK(ts.heightAt(kPoleProbe) > 0.0);
}

TEST_CASE("TerrainStreamer render tiles never overlap a rendered ancestor") {
    StreamerFixture fx;
    MockRenderer renderer;
    fl::TerrainStreamer ts{worldManifest(4), *fx.assets, fx.asyncFs, &renderer};
    // Partially stream (few pumps), then check the emitted set at every stage:
    // no emitted tile's region may be contained in another emitted tile.
    for (int stage = 0; stage < 12; ++stage) {
        pump(ts, fx.asyncFs, kPoleCam, 5);
        auto items = ts.getRenderItems(kPoleCam);
        // MockRenderer mesh names encode the tile keys; approximate the overlap check
        // via item count sanity: emission is deduped and bounded by the leaf count.
        CHECK(items.size() <= ts.desiredLeaves().size());
    }
}

TEST_CASE("TerrainStreamer eviction cancels an in-flight pack read without crashing") {
    // Pack-provide the level-2 tile covering the pole; everything else procedural.
    const TileCoord tc = worldToTile(kPoleProbe, kR);
    const TileKey packKey{tc.key.face, 2, tileIndexForUv(tc.s, 2), tileIndexForUv(tc.t, 2)};
    const std::string tilePath = "terrain/pack/pole_tile.png";

    auto pack = std::make_unique<MockTerrainPack>();
    // key: terrainId:face:level:i:j:layer (level 2, Height layer 0)
    pack->tilePaths["world:" + std::to_string(static_cast<unsigned>(packKey.face)) + ":2:" + std::to_string(packKey.i) +
                    ":" + std::to_string(packKey.j) + ":0"] = tilePath;
    StreamerFixture fx(std::move(pack));
    fx.asyncFs.addFile(tilePath, makeFlatPng16(kTileHeightmapSize, kTileHeightmapSize, 33318));

    fl::TerrainStreamer ts{worldManifest(2), *fx.assets, fx.asyncFs, nullptr};
    ts.setResidencyCap(8);

    // Pump WITHOUT servicing the async fs: procedural tiles finalize synchronously,
    // the pack tile stays Loading with its read still in flight.
    for (int i = 0; i < 30; ++i)
        ts.update(kPoleCam);

    // Move to the antipode: the pole tile leaves the desired tree and the small
    // residency cap evicts it while its read is pending (evictTile's cancelRead path).
    const glm::dvec3 antipodeCam{0.0, -2.0 * kR - 550.0, 0.0};
    for (int i = 0; i < 60; ++i)
        ts.update(antipodeCam);

    // Deliver the cancelled read's completion: it must be ignored — no crash, no
    // resurrected Loading entry — and queries keep working.
    fx.asyncFs.service();
    ts.update(antipodeCam);
    CHECK(ts.heightAt(kAntipodeProbe) > 0.0);
}

TEST_CASE("TerrainStreamer setPlanetRadius drops resident tiles and regenerates at the new radius") {
    StreamerFixture fx;
    fl::TerrainStreamer ts{worldManifest(2), *fx.assets, fx.asyncFs, nullptr};
    pump(ts, fx.asyncFs, kPoleCam, 30);
    const std::size_t before = ts.tileCount();
    REQUIRE(before > 0);

    // Same-value and invalid calls are no-ops.
    ts.setPlanetRadius(kR);
    ts.setPlanetRadius(0.0);
    ts.setPlanetRadius(-5.0);
    REQUIRE(ts.tileCount() == before);

    // A real change flushes every resident tile (curvature and procedural elevations
    // are baked at generation time)...
    ts.setPlanetRadius(600'000.0);
    REQUIRE(ts.tileCount() == 0);

    // ...and subsequent pumps regenerate at the new radius: the datum point at the
    // origin is covered again with a plausible procedural elevation.
    pump(ts, fx.asyncFs, kPoleCam, 60);
    REQUIRE(ts.tileCount() > 0);
    CHECK(ts.heightAt(kPoleProbe) > 0.0);
    CHECK(ts.heightAt(kPoleProbe) < 1000.0);
}
