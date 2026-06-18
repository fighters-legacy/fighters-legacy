// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "content/AssetManager.h"
#include "content/AssetTypes.h"
#include "content/IContentPack.h"
#include "render/TerrainMeshBuilder.h"
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
#include <vector>

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

    // Compress with DEFLATE stored blocks (no actual compression)
    // zlib header: CMF=0x78 (deflate, window=32KB), FLG=0x01 (FCHECK so CMF*256+FLG % 31 == 0)
    // (0x78*256 + 0x01 = 30721; 30721 % 31 = 0 ... actually let's compute properly)
    // CMF = 0x78, need (CMF*256 + FLG) % 31 == 0. 0x78*256 = 30720. 30720 % 31 = 30720/31=991*31+?
    // 991*31=30721, so 30720%31=30. FLG must make total % 31 == 0: FLG = 31-30 = 1, but
    // FLG bits 5-6 (FDICT,FLEVEL) should be 0. FLG=0x01 is fine.
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
        // LEN and NLEN (little-endian)
        const uint16_t len16 = static_cast<uint16_t>(blockSize);
        zlib.push_back(len16 & 0xFFu);
        zlib.push_back((len16 >> 8) & 0xFFu);
        zlib.push_back(static_cast<uint8_t>(~len16 & 0xFFu));
        zlib.push_back(static_cast<uint8_t>((~len16 >> 8) & 0xFFu));
        zlib.insert(zlib.end(), raw.data() + offset, raw.data() + offset + blockSize);
        offset += blockSize;
    }

    // Adler-32 checksum (big-endian)
    const uint32_t a32 = adler32(raw.data(), rawSize);
    const std::size_t zlibEnd = zlib.size();
    zlib.resize(zlibEnd + 4);
    wbe32(zlib.data() + zlibEnd, a32);

    // Assemble PNG
    std::vector<uint8_t> png;
    png.reserve(64 + zlib.size());

    // PNG signature
    static constexpr uint8_t kSig[] = {137, 80, 78, 71, 13, 10, 26, 10};
    png.insert(png.end(), kSig, kSig + 8);

    // IHDR
    uint8_t ihdr[13]{};
    wbe32(ihdr + 0, static_cast<uint32_t>(w));
    wbe32(ihdr + 4, static_cast<uint32_t>(h));
    ihdr[8] = 16; // bit depth
    ihdr[9] = 0;  // color type: grayscale
    ihdr[10] = 0; // compression: deflate
    ihdr[11] = 0; // filter: adaptive
    ihdr[12] = 0; // interlace: none
    appendPngChunk(png, "IHDR", ihdr, 13);

    // IDAT
    appendPngChunk(png, "IDAT", zlib.data(), static_cast<uint32_t>(zlib.size()));

    // IEND
    appendPngChunk(png, "IEND", nullptr, 0);

    return png;
}

// ===========================================================================
// MockTerrainPack — IContentPack with configurable resolveTerrainChunk
// ===========================================================================

// Resolves terrain chunks from an in-memory map; everything else null-object (see mock_content.h).
struct MockTerrainPack : NullContentPack {
    // key format: "<cx>:<cy>:<lod>"
    std::map<std::string, std::string> chunkPaths;

    const char* name() const override {
        return "MockTerrainPack";
    }
    const char* version() const override {
        return "0.0.1";
    }
    const char* id() const override {
        return "test:terrain";
    }
    std::optional<std::string> resolveTerrainChunk(const char*, uint32_t cx, uint32_t cy, uint32_t lod) const override {
        std::string key = std::to_string(static_cast<int32_t>(cx)) + ":" + std::to_string(static_cast<int32_t>(cy)) +
                          ":" + std::to_string(lod);
        auto it = chunkPaths.find(key);
        if (it == chunkPaths.end())
            return std::nullopt;
        return it->second;
    }
};

// Build manifest matching builtinWorldTerrainManifest()
static fl::TerrainManifest worldManifest() {
    fl::TerrainManifest m;
    m.terrainId = "world";
    m.chunkSizeM = 15360.0f;
    m.gridWidth = -1;
    m.gridHeight = -1;
    m.originX = -7680.0;
    m.originZ = -7680.0;
    return m;
}

// Drive update() until chunkCount reaches the expected steady state (or max iterations).
static void driveToSteadyState(fl::TerrainStreamer& ts, glm::dvec3 pos, std::size_t expected = 83, int maxIter = 200) {
    for (int i = 0; i < maxIter && ts.chunkCount() < expected; ++i)
        ts.update(pos);
}

// ---------------------------------------------------------------------------
// GLB binary parsing helpers for per-vertex mesh curvature tests
// ---------------------------------------------------------------------------

// Extract vertex Y (metres) at index idx from a GLB produced by buildTerrainMeshGlb.
// POSITION accessor: non-interleaved, stride 12, layout [x, y, z] float32 per vertex.
static float getVertexY(const std::vector<uint8_t>& glb, int idx) {
    REQUIRE(glb.size() >= 20u);
    uint32_t jsonLen = 0;
    std::memcpy(&jsonLen, glb.data() + 12, 4);
    const std::size_t binStart = 20u + jsonLen + 8u; // 12 GLB hdr + 8 JSON chunk hdr + data + 8 BIN chunk hdr
    const std::size_t yOff = binStart + static_cast<std::size_t>(idx) * 12u + 4u;
    REQUIRE(glb.size() >= yOff + 4u);
    float y = 0.0f;
    std::memcpy(&y, glb.data() + yOff, 4);
    return y;
}

// Extract normal X component at vertex index idx.
// NORMAL accessor follows all POSITION data (vertCount * 12 bytes), same stride.
static float getVertexNX(const std::vector<uint8_t>& glb, int idx, int vertCount) {
    REQUIRE(glb.size() >= 20u);
    uint32_t jsonLen = 0;
    std::memcpy(&jsonLen, glb.data() + 12, 4);
    const std::size_t binStart = 20u + jsonLen + 8u;
    const std::size_t nxOff =
        binStart + static_cast<std::size_t>(vertCount) * 12u + static_cast<std::size_t>(idx) * 12u;
    REQUIRE(glb.size() >= nxOff + 4u);
    float nx = 0.0f;
    std::memcpy(&nx, glb.data() + nxOff, 4);
    return nx;
}

} // namespace

// ===========================================================================
// Tests
// ===========================================================================

TEST_CASE("TerrainStreamer procedural fallback generates render items") {
    MockLogger logger;
    std::vector<std::unique_ptr<IContentPack>> packs;
    AssetManager assets{std::move(packs), logger};
    assets.initialize(nullptr);

    MockAsyncFilesystem asyncFs;
    asyncFs.init();
    MockRenderer renderer;

    fl::TerrainStreamer ts{worldManifest(), assets, asyncFs, &renderer};
    driveToSteadyState(ts, {0.0, 0.0, 0.0});

    auto items = ts.getRenderItems({0.0, 0.0, 0.0});
    REQUIRE(!items.empty());
    const double h = ts.heightAt(0.0, 0.0);
    CHECK(h > 300.0);
    CHECK(h < 900.0);
}

TEST_CASE("TerrainStreamer null renderer returns empty render items but height works") {
    MockLogger logger;
    std::vector<std::unique_ptr<IContentPack>> packs;
    AssetManager assets{std::move(packs), logger};
    assets.initialize(nullptr);

    MockAsyncFilesystem asyncFs;
    asyncFs.init();

    fl::TerrainStreamer ts{worldManifest(), assets, asyncFs, nullptr};
    driveToSteadyState(ts, {0.0, 0.0, 0.0});

    auto items = ts.getRenderItems({0.0, 0.0, 0.0});
    CHECK(items.empty());
    CHECK(ts.heightAt(0.0, 0.0) > 0.0);
}

TEST_CASE("TerrainStreamer heightReadyAt tracks LOD0 chunk readiness") {
    MockLogger logger;
    std::vector<std::unique_ptr<IContentPack>> packs;
    AssetManager assets{std::move(packs), logger};
    assets.initialize(nullptr);

    MockAsyncFilesystem asyncFs;
    asyncFs.init();

    fl::TerrainStreamer ts{worldManifest(), assets, asyncFs, nullptr};

    // Before any update(): the origin LOD0 chunk is not loaded, so heightReadyAt is false
    // and heightAt returns the 0.0 not-loaded sentinel.
    CHECK_FALSE(ts.heightReadyAt(0.0, 0.0));
    CHECK(ts.heightAt(0.0, 0.0) == 0.0);

    // After streaming completes: heightReadyAt reports ready and heightAt returns a real value.
    driveToSteadyState(ts, {0.0, 0.0, 0.0});
    CHECK(ts.heightReadyAt(0.0, 0.0));
    CHECK(ts.heightAt(0.0, 0.0) > 0.0);
}

TEST_CASE("TerrainStreamer chunkCount is 83 at steady state") {
    MockLogger logger;
    std::vector<std::unique_ptr<IContentPack>> packs;
    AssetManager assets{std::move(packs), logger};
    assets.initialize(nullptr);

    MockAsyncFilesystem asyncFs;
    asyncFs.init();

    fl::TerrainStreamer ts{worldManifest(), assets, asyncFs, nullptr};
    driveToSteadyState(ts, {0.0, 0.0, 0.0}, 83, 200);

    CHECK(ts.chunkCount() == 83u);
}

TEST_CASE("TerrainStreamer PNG chunk loaded from async filesystem") {
    MockLogger logger;

    auto pack = std::make_unique<MockTerrainPack>();
    const std::string chunkPath = "terrain/world/lod0/chunk_0000_0000.png";
    pack->chunkPaths["0:0:0"] = chunkPath;

    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.push_back(std::move(pack));
    AssetManager assets{std::move(packs), logger};
    assets.initialize(nullptr);

    MockAsyncFilesystem asyncFs;
    asyncFs.init();
    // 33318 - 32768 = 550 m elevation
    asyncFs.addFile(chunkPath, [](const std::vector<uint8_t>& v) { return v; }(makeFlatPng16(513, 513, 33318)));

    MockRenderer renderer;
    fl::TerrainStreamer ts{worldManifest(), assets, asyncFs, &renderer};

    // Queue the async read
    ts.update({0.0, 0.0, 0.0});
    // Fire completion callback
    asyncFs.service();
    // Allow any remaining procedural chunks to load
    driveToSteadyState(ts, {0.0, 0.0, 0.0});

    REQUIRE(!ts.getRenderItems({0.0, 0.0, 0.0}).empty());
    CHECK(ts.heightAt(0.0, 0.0) == Catch::Approx(550.0).margin(5.0));
}

TEST_CASE("TerrainStreamer evicts chunks when camera moves far away") {
    MockLogger logger;
    std::vector<std::unique_ptr<IContentPack>> packs;
    AssetManager assets{std::move(packs), logger};
    assets.initialize(nullptr);

    MockAsyncFilesystem asyncFs;
    asyncFs.init();

    fl::TerrainStreamer ts{worldManifest(), assets, asyncFs, nullptr};
    driveToSteadyState(ts, {0.0, 0.0, 0.0});
    REQUIRE(ts.chunkCount() == 83u);

    // Move camera 10 chunks away — the origin LOD0 chunk should be evicted.
    // driveToSteadyState would exit immediately (count is already 83), so call
    // update() explicitly at the new position to trigger eviction and re-loading.
    const double farX = 15360.0 * 10.0;
    for (int i = 0; i < 200; ++i)
        ts.update({farX, 0.0, 0.0});

    CHECK(ts.heightAt(0.0, 0.0) == 0.0); // LOD0 origin chunk no longer loaded
    CHECK(ts.chunkCount() == 83u);       // same total, different chunks
}

TEST_CASE("TerrainStreamer async read error falls back to procedural") {
    MockLogger logger;

    auto pack = std::make_unique<MockTerrainPack>();
    const std::string chunkPath = "terrain/world/lod0/chunk_0000_0000.png";
    pack->chunkPaths["0:0:0"] = chunkPath;
    // No file added to asyncFs → service() fires Error callback

    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.push_back(std::move(pack));
    AssetManager assets{std::move(packs), logger};
    assets.initialize(nullptr);

    MockAsyncFilesystem asyncFs;
    asyncFs.init();

    fl::TerrainStreamer ts{worldManifest(), assets, asyncFs, nullptr};
    ts.update({0.0, 0.0, 0.0});
    asyncFs.service(); // fires Error → procedural fallback
    driveToSteadyState(ts, {0.0, 0.0, 0.0});

    CHECK(ts.heightAt(0.0, 0.0) > 0.0);
}

TEST_CASE("TerrainStreamer surfaceAt returns zero") {
    MockLogger logger;
    std::vector<std::unique_ptr<IContentPack>> packs;
    AssetManager assets{std::move(packs), logger};
    assets.initialize(nullptr);

    MockAsyncFilesystem asyncFs;
    asyncFs.init();

    fl::TerrainStreamer ts{worldManifest(), assets, asyncFs, nullptr};
    CHECK(ts.surfaceAt(0.0, 0.0) == 0);
}

TEST_CASE("TerrainStreamer heightAt returns zero when only LOD1 loaded") {
    MockLogger logger;
    std::vector<std::unique_ptr<IContentPack>> packs;
    AssetManager assets{std::move(packs), logger};
    assets.initialize(nullptr);

    MockAsyncFilesystem asyncFs;
    asyncFs.init();

    fl::TerrainStreamer ts{worldManifest(), assets, asyncFs, nullptr};

    // Place camera so chunk (0,0) is in the LOD1 ring but NOT the LOD0 ring.
    // Manifest originX = -7680, chunkSize = 15360.
    // chunk (0,0) world X range: [-7680, 7680). Camera at X = 15360*1.5 = 23040.
    // cameraCx = floor((23040 - (-7680)) / 15360) = floor(30720/15360) = 2.
    // dist to (0,0): |0-2|=2 → within LOD1 ring (he=2), outside LOD0 ring (he=1).
    const double camX = 15360.0 * 1.5;
    driveToSteadyState(ts, {camX, 0.0, 0.0});

    // LOD0 for chunk (0,0) was never loaded — heightAt must return 0.0.
    CHECK(ts.heightAt(0.0, 0.0) == 0.0);
}

TEST_CASE("TerrainMeshBuilder returns non-empty bytes for valid input") {
    const int size = 513;
    std::vector<uint16_t> heights(static_cast<std::size_t>(size) * size, 33318u);
    auto glb = fl::buildTerrainMeshGlb(heights, size, 128, 15360.0f);

    REQUIRE(!glb.empty());
    // GLB magic: 'g','l','T','F' = 0x46546C67 LE
    REQUIRE(glb.size() >= 12u);
    CHECK(glb[0] == 0x67u);
    CHECK(glb[1] == 0x6Cu);
    CHECK(glb[2] == 0x54u);
    CHECK(glb[3] == 0x46u);
    // Version field (bytes 4-7) must be 2
    uint32_t version = 0;
    std::memcpy(&version, glb.data() + 4, 4);
    CHECK(version == 2u);
}

TEST_CASE("TerrainMeshBuilder returns empty for invalid input") {
    const int size = 513;
    std::vector<uint16_t> heights(static_cast<std::size_t>(size) * size, 32768u);

    // Empty heights
    CHECK(fl::buildTerrainMeshGlb({}, size, 128, 15360.0f).empty());
    // Zero meshGrid
    CHECK(fl::buildTerrainMeshGlb(heights, size, 0, 15360.0f).empty());
    // heightmapSize < 2
    CHECK(fl::buildTerrainMeshGlb(heights, 1, 128, 15360.0f).empty());
    // heightmapSize < meshGrid + 1
    CHECK(fl::buildTerrainMeshGlb(heights, 5, 128, 15360.0f).empty());
}

TEST_CASE("TerrainMeshBuilder vertex count matches meshGrid") {
    const int size = 129;
    const int meshGrid = 32;
    std::vector<uint16_t> heights(static_cast<std::size_t>(size) * size, 32768u);
    auto glb = fl::buildTerrainMeshGlb(heights, size, meshGrid, 15360.0f);
    REQUIRE(!glb.empty());

    // Find JSON chunk: starts at byte 20 (after 12-byte header + 8-byte chunk header)
    REQUIRE(glb.size() > 20u);
    const char* jsonStart = reinterpret_cast<const char*>(glb.data() + 20);
    const std::size_t jsonChunkLen = [&] {
        uint32_t v = 0;
        std::memcpy(&v, glb.data() + 12, 4);
        return static_cast<std::size_t>(v);
    }();
    const std::string json(jsonStart, std::min(jsonChunkLen, glb.size() - 20u));

    // Expected counts: vertCount = (meshGrid+1)^2 = 33*33 = 1089
    //                  indexCount = meshGrid^2 * 6 = 32*32*6 = 6144
    CHECK(json.find("\"count\":" + std::to_string((meshGrid + 1) * (meshGrid + 1))) != std::string::npos);
    CHECK(json.find("\"count\":" + std::to_string(meshGrid * meshGrid * 6)) != std::string::npos);
}

TEST_CASE("TerrainStreamer cancelled read during eviction does not crash") {
    MockLogger logger;

    auto pack = std::make_unique<MockTerrainPack>();
    const std::string chunkPath = "terrain/world/lod0/chunk_0000_0000.png";
    pack->chunkPaths["0:0:0"] = chunkPath;
    // No file in asyncFs — will fire Error if service() is ever called

    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.push_back(std::move(pack));
    AssetManager assets{std::move(packs), logger};
    assets.initialize(nullptr);

    MockAsyncFilesystem asyncFs;
    asyncFs.init();

    MockRenderer renderer;
    fl::TerrainStreamer ts{worldManifest(), assets, asyncFs, &renderer};

    // Queue async read for chunk (0,0) LOD0
    ts.update({0.0, 0.0, 0.0});
    CHECK(ts.chunkCount() > 0u);

    // Move camera far away before service() fires — triggers evictChunk → cancelRead
    const double farX = 15360.0 * 10.0;
    ts.update({farX, 0.0, 0.0});

    // service() fires Cancelled callback — TerrainStreamer must not crash or re-insert
    REQUIRE_NOTHROW(asyncFs.service());
    REQUIRE_NOTHROW(ts.update({farX, 0.0, 0.0}));

    // Origin chunk must not be present
    CHECK(ts.heightAt(0.0, 0.0) == 0.0);
}

// ---------------------------------------------------------------------------
// Spherical terrain correction
// ---------------------------------------------------------------------------

TEST_CASE("TerrainStreamer: curvature correction is zero at origin", "[spherical]") {
    MockLogger logger;
    std::vector<std::unique_ptr<IContentPack>> packs;
    AssetManager assets{std::move(packs), logger};
    assets.initialize(nullptr);
    MockAsyncFilesystem asyncFs;
    asyncFs.init();

    fl::TerrainStreamer ts{worldManifest(), assets, asyncFs, nullptr};
    driveToSteadyState(ts, {0.0, 0.0, 0.0});

    const double h = ts.heightAt(0.0, 0.0); // default Earth radius
    // At origin (x=0, z=0) the correction is sqrt(R^2) - R = 0 regardless of radius
    ts.setPlanetRadius(3'000'000.0); // Mars-ish radius: correction still 0 at origin
    CHECK(ts.heightAt(0.0, 0.0) == Catch::Approx(h).margin(1e-3));
}

TEST_CASE("TerrainStreamer: curvature correction is negative at lateral position", "[spherical]") {
    MockLogger logger;
    std::vector<std::unique_ptr<IContentPack>> packs;
    AssetManager assets{std::move(packs), logger};
    assets.initialize(nullptr);
    MockAsyncFilesystem asyncFs;
    asyncFs.init();

    fl::TerrainStreamer ts{worldManifest(), assets, asyncFs, nullptr};
    const double D = 100'000.0; // 100 km offset
    driveToSteadyState(ts, {D, 0.0, 0.0});

    // Use an astronomically large radius as a "flat" baseline (correction ~ 0)
    ts.setPlanetRadius(1e15);
    const double flatH = ts.heightAt(D, 0.0);

    const double R = 6'371'000.0;
    const double expectedCorrection = std::sqrt(std::max(0.0, R * R - D * D)) - R;
    ts.setPlanetRadius(R);
    const double sphereH = ts.heightAt(D, 0.0);
    CHECK(sphereH == Catch::Approx(flatH + expectedCorrection).margin(1e-3));
    CHECK(sphereH < flatH); // correction is negative for any D > 0
}

TEST_CASE("TerrainStreamer: curvature correction magnitude at 100 km", "[spherical]") {
    // Analytical: correction ~ -D^2 / (2R) for D << R
    const double D = 100'000.0;
    const double R = 6'371'000.0;
    const double correction = std::sqrt(R * R - D * D) - R;
    const double approxCorrection = -(D * D) / (2.0 * R);
    // Should agree to within ~1 m (second-order term is tiny)
    CHECK(correction == Catch::Approx(approxCorrection).margin(1.0));
}

TEST_CASE("TerrainStreamer: curvature applied at default radius without explicit setPlanetRadius", "[spherical]") {
    MockLogger logger;
    std::vector<std::unique_ptr<IContentPack>> packs;
    AssetManager assets{std::move(packs), logger};
    assets.initialize(nullptr);
    MockAsyncFilesystem asyncFs;
    asyncFs.init();

    fl::TerrainStreamer ts{worldManifest(), assets, asyncFs, nullptr};
    const double D = 100'000.0;
    driveToSteadyState(ts, {D, 0.0, 0.0});

    // Without calling setPlanetRadius the default Earth radius is applied.
    // Use a very large radius as a flat proxy to verify the default is not flat.
    const double hDefault = ts.heightAt(D, 0.0);
    ts.setPlanetRadius(1e15); // effectively flat
    const double hFlat = ts.heightAt(D, 0.0);
    // Default spherical correction is negative at D > 0
    CHECK(hDefault < hFlat);
}

TEST_CASE("TerrainStreamer: heightAt is callable from a background thread", "[spherical][threading]") {
    MockLogger logger;
    std::vector<std::unique_ptr<IContentPack>> packs;
    AssetManager assets{std::move(packs), logger};
    assets.initialize(nullptr);
    MockAsyncFilesystem asyncFs;
    asyncFs.init();

    fl::TerrainStreamer ts{worldManifest(), assets, asyncFs, nullptr};
    driveToSteadyState(ts, {0.0, 0.0, 0.0});

    double result = 0.0;
    std::thread t([&] { result = ts.heightAt(0.0, 0.0); });
    t.join();
    // Result is deterministic (procedural FBM at origin); just verify no crash / data race.
    CHECK(std::isfinite(result));
}

// ===========================================================================
// Per-vertex mesh curvature tests
// ===========================================================================

TEST_CASE("TerrainMeshBuilder: spherical correction at world origin corner is zero") {
    // At (0, 0) the sphere surface is exactly at Y=0: sqrt(R^2 - 0) - R = 0.
    // For vertices away from the origin the correction becomes negative.
    const int meshGrid = 32; // LOD 2
    const int hmSize = 129;
    const float chunkSizeM = 15360.0f;
    const double R = 6'371'000.0;

    std::vector<uint16_t> flat(static_cast<std::size_t>(hmSize) * hmSize, 32768u); // 0 m elevation
    auto glb = fl::buildTerrainMeshGlb(flat, hmSize, meshGrid, chunkSizeM, 0.0, 0.0, R);
    REQUIRE(!glb.empty());

    // Corner vertex (index 0): world position (0, 0) → correction = 0
    const float y0 = getVertexY(glb, 0);
    CHECK(y0 == Catch::Approx(0.0f).margin(1e-3f));

    // Last vertex on row 0 (index meshGrid): world X = chunkSizeM → correction is negative
    const float yFar = getVertexY(glb, meshGrid);
    const double D = static_cast<double>(chunkSizeM);
    const float expected = static_cast<float>(std::sqrt(R * R - D * D) - R);
    CHECK(yFar == Catch::Approx(expected).margin(0.1f));
    CHECK(yFar < 0.0f);
}

TEST_CASE("TerrainMeshBuilder: per-vertex Y varies monotonically at non-zero world position") {
    // Build flat terrain at chunkWorldX = 100 km. Every vertex has zero elevation but
    // different spherical corrections (increasing X = further from origin = more negative).
    const int meshGrid = 32;
    const int hmSize = 129;
    const float chunkSizeM = 15360.0f;
    const double chunkWorldX = 100'000.0;
    const double R = 6'371'000.0;

    std::vector<uint16_t> flat(static_cast<std::size_t>(hmSize) * hmSize, 32768u);

    // Flat (no radius): all vertex Y should be 0
    auto glbFlat = fl::buildTerrainMeshGlb(flat, hmSize, meshGrid, chunkSizeM, chunkWorldX, 0.0, 0.0);
    REQUIRE(!glbFlat.empty());
    CHECK(getVertexY(glbFlat, 0) == Catch::Approx(0.0f).margin(1e-4f));
    CHECK(getVertexY(glbFlat, meshGrid) == Catch::Approx(0.0f).margin(1e-4f));

    // Spherical: vertex at col=0 is at world X=100 km; col=meshGrid is at X=115.36 km
    auto glbSphere = fl::buildTerrainMeshGlb(flat, hmSize, meshGrid, chunkSizeM, chunkWorldX, 0.0, R);
    REQUIRE(!glbSphere.empty());

    const float y0 = getVertexY(glbSphere, 0);
    const float yFar = getVertexY(glbSphere, meshGrid);

    // Both corrections are negative (D > 0 in both cases)
    CHECK(y0 < 0.0f);
    CHECK(yFar < 0.0f);

    // Further vertex has a more negative correction (correction = sqrt(R^2-D^2)-R, monotone decreasing)
    CHECK(yFar < y0);

    // Spot-check vertex at col=0 against analytical value
    const float expected0 = static_cast<float>(std::sqrt(R * R - chunkWorldX * chunkWorldX) - R);
    CHECK(y0 == Catch::Approx(expected0).margin(0.1f));
}

TEST_CASE("TerrainMeshBuilder: normals account for spherical curvature gradient") {
    // At 100 km from origin the spherical gradient ≈ -vx/R ≈ -0.0157, producing a
    // non-zero normal X component tilting the surface toward the origin.
    const int meshGrid = 32;
    const int hmSize = 129;
    const float chunkSizeM = 15360.0f;
    const double chunkWorldX = 100'000.0;
    const double R = 6'371'000.0;
    const int vertCount = (meshGrid + 1) * (meshGrid + 1);

    std::vector<uint16_t> flat(static_cast<std::size_t>(hmSize) * hmSize, 32768u);

    // Flat (no spherical): normal X at vertex 0 should be ≈ 0
    auto glbFlat = fl::buildTerrainMeshGlb(flat, hmSize, meshGrid, chunkSizeM, chunkWorldX, 0.0, 0.0);
    REQUIRE(!glbFlat.empty());
    const float nxFlat = getVertexNX(glbFlat, 0, vertCount);
    CHECK(nxFlat == Catch::Approx(0.0f).margin(1e-4f));

    // Spherical: normal X should be positive (surface tilts toward +X origin direction)
    auto glbSphere = fl::buildTerrainMeshGlb(flat, hmSize, meshGrid, chunkSizeM, chunkWorldX, 0.0, R);
    REQUIRE(!glbSphere.empty());
    const float nxSphere = getVertexNX(glbSphere, 0, vertCount);
    CHECK(nxSphere > 0.01f);
}

TEST_CASE("TerrainStreamer: getRenderItems Y transform is zero with spherical radius") {
    // All spherical curvature is baked into vertex Y; getRenderItems must place each
    // chunk at Y = 0 in world space regardless of its distance from origin.
    MockLogger logger;
    std::vector<std::unique_ptr<IContentPack>> packs;
    AssetManager assets{std::move(packs), logger};
    assets.initialize(nullptr);
    MockAsyncFilesystem asyncFs;
    asyncFs.init();
    MockRenderer renderer;

    fl::TerrainStreamer ts{worldManifest(), assets, asyncFs, &renderer};
    // Default Earth radius is already set; drive to steady state
    driveToSteadyState(ts, {0.0, 0.0, 0.0});

    const auto items = ts.getRenderItems({0.0, 0.0, 0.0});
    REQUIRE(!items.empty());
    for (const auto& item : items)
        CHECK(item.transform[3][1] == Catch::Approx(0.0f).margin(0.001f));
}

TEST_CASE("TerrainStreamer: getRenderItems Y transform is zero with radius set to zero") {
    // Flat mode (radius=0) must also place chunks at Y = 0 (regression guard).
    MockLogger logger;
    std::vector<std::unique_ptr<IContentPack>> packs;
    AssetManager assets{std::move(packs), logger};
    assets.initialize(nullptr);
    MockAsyncFilesystem asyncFs;
    asyncFs.init();
    MockRenderer renderer;

    fl::TerrainStreamer ts{worldManifest(), assets, asyncFs, &renderer};
    ts.setPlanetRadius(0.0);
    driveToSteadyState(ts, {0.0, 0.0, 0.0});

    const auto items = ts.getRenderItems({0.0, 0.0, 0.0});
    REQUIRE(!items.empty());
    for (const auto& item : items)
        CHECK(item.transform[3][1] == Catch::Approx(0.0f).margin(0.001f));
}

TEST_CASE("TerrainStreamer: heightAt returns uncorrected elevation when radius is zero") {
    // When setPlanetRadius(0) the sqrt(max(0, 0-D^2))-0 = 0 guard fires for any D,
    // so heightAt returns raw elevation with no spherical adjustment.
    MockLogger logger;
    std::vector<std::unique_ptr<IContentPack>> packs;
    AssetManager assets{std::move(packs), logger};
    assets.initialize(nullptr);
    MockAsyncFilesystem asyncFs;
    asyncFs.init();

    fl::TerrainStreamer ts{worldManifest(), assets, asyncFs, nullptr};
    const double D = 100'000.0;
    driveToSteadyState(ts, {D, 0.0, 0.0});

    ts.setPlanetRadius(1e15); // effectively flat proxy
    const double hFlat = ts.heightAt(D, 0.0);

    ts.setPlanetRadius(0.0); // explicit zero radius
    const double hZero = ts.heightAt(D, 0.0);

    // Both should return the same raw elevation (zero correction in both cases)
    CHECK(hZero == Catch::Approx(hFlat).margin(1e-3));
}
