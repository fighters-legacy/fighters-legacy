// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/TerrainStreamer.h"

#include "IRenderer.h"
#include "content/AssetManager.h"
#include "render/BuiltinBiomes.h"
#include "render/ProceduralTerrainChunk.h"
#include "render/TerrainChunkIO.h"
#include "render/TerrainMeshBuilder.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <numbers>
#include <optional>
#include <shared_mutex>
#include <string>

#include <glm/gtc/matrix_transform.hpp>

namespace fl {

namespace {

constexpr int kTileMeshGrid = kTileHeightmapSize - 1;                  // 128 quads per tile axis
constexpr double kQuarterCircumferenceFactor = std::numbers::pi / 2.0; // face arc / R

} // namespace

// ---------------------------------------------------------------------------
// TileKeyHash
// ---------------------------------------------------------------------------

std::size_t TerrainStreamer::TileKeyHash::operator()(const TileKey& k) const noexcept {
    std::size_t h = std::hash<uint32_t>{}((static_cast<uint32_t>(k.face) << 8) | k.level);
    h ^= std::hash<uint32_t>{}(k.i) + 0x9e3779b9u + (h << 6) + (h >> 2);
    h ^= std::hash<uint32_t>{}(k.j) + 0x9e3779b9u + (h << 6) + (h >> 2);
    return h;
}

// ---------------------------------------------------------------------------
// Constructor / destructor
// ---------------------------------------------------------------------------

TerrainStreamer::TerrainStreamer(fl::TerrainManifest manifest, AssetManager& assets, IAsyncFilesystem& asyncFs,
                                 IRenderer* renderer)
    : m_manifest(std::move(manifest)), m_assets(assets), m_asyncFs(asyncFs), m_renderer(renderer) {
    m_asyncFs.setEventHandler(this);
    uploadBiomeTextures();
}

// Terrain biome texture arrays (#446): a pack's biome_basecolor/biome_normalorm array KTX2 if
// present, else the compiled-in procedural biome set (so the sandbox has textured terrain zero-pack).
// Null renderer (fl-server headless) = no-op.
void TerrainStreamer::uploadBiomeTextures() {
    if (!m_renderer)
        return;
    auto uploadPackOrBuiltin = [&](const char* assetName, bool srgb,
                                   const std::array<BuiltinRgbaTexture, kBiomeLayerCount>& builtin) -> TextureHandle {
        // A pack-provided KTX2 array wins.
        if (auto tex = m_assets.loadTexture(assetName); tex && !tex->bytes.empty()) {
            TextureUploadDesc td{};
            td.name = assetName;
            td.bytes = tex->bytes;
            td.srgb = srgb;
            const TextureHandle h = m_renderer->createTextureArray(td);
            if (h.valid())
                return h;
        }
        // Builtin fallback: concatenate the layer-major RGBA8 and upload as a raw array.
        std::vector<uint8_t> raw;
        raw.reserve(static_cast<std::size_t>(kBuiltinTexSize) * kBuiltinTexSize * 4u * builtin.size());
        for (const auto& layer : builtin)
            raw.insert(raw.end(), layer.pixels.begin(), layer.pixels.end());
        TextureUploadDesc td{};
        td.name = assetName;
        td.bytes = raw;
        td.srgb = srgb;
        td.rawWidth = kBuiltinTexSize;
        td.rawHeight = kBuiltinTexSize;
        td.rawLayers = static_cast<uint32_t>(builtin.size());
        return m_renderer->createTextureArray(td);
    };

    m_biomeColorTex = uploadPackOrBuiltin("biome_basecolor", /*srgb=*/true, builtinBiomeBaseColorLayers());
    m_biomeNormalOrmTex = uploadPackOrBuiltin("biome_normalorm", /*srgb=*/false, builtinBiomeNormalOrmLayers());
    m_renderer->setTerrainBiomeTextures(m_biomeColorTex, m_biomeNormalOrmTex, kBiomeLayerCount);
}

TerrainStreamer::~TerrainStreamer() {
    // Deregister first so any late service() calls hit a null handler, not dead this.
    m_asyncFs.setEventHandler(nullptr);
    // Cancel all in-flight reads (callbacks now go nowhere).
    for (auto& [id, pending] : m_pendingByReadId)
        m_asyncFs.cancelRead(id);
    // Destroy GPU resources.
    if (m_renderer) {
        for (auto& [key, tile] : m_tiles) {
            if (tile.mesh.valid())
                m_renderer->destroyMesh(tile.mesh);
        }
        if (m_terrainMat.valid())
            m_renderer->destroyMaterial(m_terrainMat);
    }
}

// ---------------------------------------------------------------------------
// SSE refinement
// ---------------------------------------------------------------------------

double TerrainStreamer::tileExtentM(int level) const noexcept {
    return kQuarterCircumferenceFactor * m_planetRadiusM / static_cast<double>(uint64_t{1} << level);
}

double TerrainStreamer::skirtDepthFor(int level) const noexcept {
    return std::clamp(tileExtentM(level) / 64.0, 2.0, 200.0);
}

bool TerrainStreamer::shouldRefine(const TileKey& key, glm::dvec3 camPos) const noexcept {
    if (static_cast<int>(key.level) >= m_manifest.maxTileLevel)
        return false;
    const double ext = tileExtentM(key.level);
    const glm::dvec3 centre = tileToWorld(key, 0.5, 0.5, 0.0, m_planetRadiusM);
    // Distance to the tile's bounding sphere (half-diagonal; terrain amplitude deliberately
    // NOT added -- a fat slack forces max-depth refinement for every tile within slack range,
    // exploding the desired set. Under-refining a peak directly below the camera is benign).
    const double bound = ext * 0.75;
    const double d = std::max(glm::length(camPos - centre) - bound, 1.0);
    // Geometric error proxy: a fraction of one mesh quad (the height deviation a coarser
    // level introduces is far smaller than the full quad extent). Projected size in pixels:
    const double eps = ext * (kGeomErrorFactor / static_cast<double>(kTileMeshGrid));
    const double sse =
        eps * static_cast<double>(m_screenHeightPx) / (2.0 * d * std::tan(static_cast<double>(m_fovYRad) * 0.5));
    return sse > static_cast<double>(kSseTauPx);
}

void TerrainStreamer::refine(const TileKey& key, glm::dvec3 camPos, std::vector<TileKey>& leaves) const {
    if (shouldRefine(key, camPos)) {
        for (uint8_t q = 0; q < 4; ++q)
            refine(child(key, q), camPos, leaves);
        return;
    }
    leaves.push_back(key);
}

// Restricted-quadtree pass: no leaf may border a leaf more than one level coarser
// across a shared edge. Splits the too-coarse leaf and repeats until stable.
void TerrainStreamer::balanceLeaves(TileSet& leaves) const {
    bool changed = true;
    while (changed) {
        changed = false;
        const std::vector<TileKey> snapshot(leaves.begin(), leaves.end());
        for (const TileKey& t : snapshot) {
            if (t.level < 2 || leaves.find(t) == leaves.end())
                continue;
            for (int e = 0; e < 4; ++e) {
                const TileKey nb = neighbor(t, static_cast<TileEdge>(e));
                // Find the desired leaf covering the neighbour region.
                TileKey cover = nb;
                bool found = false;
                while (true) {
                    if (leaves.find(cover) != leaves.end()) {
                        found = true;
                        break;
                    }
                    if (cover.level == 0)
                        break;
                    cover = parent(cover);
                }
                if (found && static_cast<int>(cover.level) < static_cast<int>(t.level) - 1) {
                    leaves.erase(cover);
                    for (uint8_t q = 0; q < 4; ++q)
                        leaves.insert(child(cover, q));
                    changed = true;
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// update
// ---------------------------------------------------------------------------

void TerrainStreamer::update(glm::dvec3 cameraWorldPos) {
    ++m_frame;
    m_updated = true;

    // 1. SSE-driven desired leaves from the six face roots.
    std::vector<TileKey> leafVec;
    for (uint8_t f = 0; f < kCubeFaceCount; ++f)
        refine(TileKey{f, 0, 0, 0}, cameraWorldPos, leafVec);
    TileSet leaves(leafVec.begin(), leafVec.end());

    // 2. 2:1 edge balance (crack rule).
    balanceLeaves(leaves);

    // 3. Desired tree = leaves plus every ancestor (coarse fallbacks + walk chain).
    m_desiredLeaves = std::move(leaves);
    m_desiredAll.clear();
    for (const TileKey& leaf : m_desiredLeaves) {
        TileKey k = leaf;
        while (true) {
            if (!m_desiredAll.insert(k).second)
                break; // ancestors above already inserted via another leaf
            if (k.level == 0)
                break;
            k = parent(k);
        }
    }

    // 4. Touch resident desired tiles; queue loads for missing ones. Missing tiles are
    //    sorted coarse-first, then nearest-to-camera within a level: the fallback
    //    ancestors load before detail, and the camera's covering chain (which
    //    heightReadyAt walks) becomes Ready within the first few pumps instead of
    //    waiting behind far-side tiles.
    std::vector<std::pair<double, TileKey>> missing;
    for (const TileKey& k : m_desiredAll) {
        auto it = m_tiles.find(k);
        if (it != m_tiles.end()) {
            it->second.lastDesiredFrame = m_frame;
            continue;
        }
        const glm::dvec3 centre = tileToWorld(k, 0.5, 0.5, 0.0, m_planetRadiusM);
        const glm::dvec3 dvec = cameraWorldPos - centre;
        missing.emplace_back(glm::dot(dvec, dvec), k);
    }
    std::sort(missing.begin(), missing.end(), [](const auto& a, const auto& b) {
        if (a.second.level != b.second.level)
            return a.second.level < b.second.level;
        return a.first < b.first;
    });
    int proceduralCount = 0;
    for (const auto& [d2, k] : missing)
        loadTile(k, proceduralCount);

    // 5. LRU eviction: tiles outside the desired tree are cached until the residency
    //    cap, then evicted least-recently-desired first.
    if (m_tiles.size() > m_maxResidentTiles) {
        std::vector<std::pair<uint64_t, TileKey>> evictable;
        for (const auto& [key, tile] : m_tiles) {
            if (m_desiredAll.find(key) == m_desiredAll.end())
                evictable.emplace_back(tile.lastDesiredFrame, key);
        }
        std::sort(evictable.begin(), evictable.end(), [](const auto& a, const auto& b) {
            if (a.first != b.first)
                return a.first < b.first;
            // Equal last-desired frame: evict deeper tiles first, so a covering chain
            // never loses an ancestor while a deeper descendant stays resident
            // (deepestReadyTile walks the chain contiguously from the root).
            return a.second.level > b.second.level;
        });
        for (const auto& [frame, key] : evictable) {
            if (m_tiles.size() <= m_maxResidentTiles)
                break;
            evictTile(key);
        }
    }
}

// ---------------------------------------------------------------------------
// loadTile
// ---------------------------------------------------------------------------

void TerrainStreamer::loadTile(const TileKey& key, int& proceduralCount) {
    // Resolve the pack tile path for this TileKey (terrain/<id>/f<face>/l<level>/tile_<i>_<j>.png).
    // Skipped entirely when no packs are loaded — the builtin-terrain common case — so the per-frame
    // missing-tile sweep does no pack probing.
    std::optional<std::string> path;
    if (m_assets.hasPacks()) {
        path = m_assets.resolveTilePath(m_manifest.terrainId.c_str(), key.face, key.level, key.i, key.j,
                                        fl::TileLayer::Height);
    }
    if (!path) {
        // Procedural fallback — rate-limited to avoid a first-frame hitch.
        if (proceduralCount >= kMaxProceduralPerUpdate)
            return;
        ++proceduralCount;
        loadTileProcedural(key);
        return;
    }

    {
        std::unique_lock lock(m_tileMutex);
        Tile& tile = m_tiles[key];
        tile.state = TileState::Loading;
        tile.lastDesiredFrame = m_frame;
    }
    const AsyncReadId id = m_asyncFs.readFileAsync(PathDomain::Assets, path->c_str());
    if (id == 0) {
        // readFileAsync failed — remove the entry and retry next frame.
        std::unique_lock lock(m_tileMutex);
        m_tiles.erase(key);
        return;
    }
    {
        std::unique_lock lock(m_tileMutex);
        m_tiles[key].pendingHeightRead = id;
    }
    m_pendingByReadId[id] = PendingRead{key, TileLayer::Height};

    // Optional land-cover layer (terrain/<id>/f<face>/l<level>/tile_<i>_<j>_lc.png).
    auto coverPath = m_assets.resolveTilePath(m_manifest.terrainId.c_str(), key.face, key.level, key.i, key.j,
                                              fl::TileLayer::LandCover);
    if (coverPath) {
        const AsyncReadId cid = m_asyncFs.readFileAsync(PathDomain::Assets, coverPath->c_str());
        if (cid != 0) {
            {
                std::unique_lock lock(m_tileMutex);
                m_tiles[key].pendingCoverRead = cid;
            }
            m_pendingByReadId[cid] = PendingRead{key, TileLayer::LandCover};
        }
    }
}

void TerrainStreamer::loadTileProcedural(const TileKey& key) {
    auto heights = generateProceduralTile(key, m_planetRadiusM, kBuiltinProceduralParams);
    {
        std::unique_lock lock(m_tileMutex);
        Tile& tile = m_tiles[key];
        tile.heightmap = std::move(heights);
        tile.lastDesiredFrame = m_frame;
    }
    finalizeTile(key);
}

// ---------------------------------------------------------------------------
// onReadComplete
// ---------------------------------------------------------------------------

void TerrainStreamer::onReadComplete(AsyncReadId id, AsyncReadStatus status, const void* data, std::size_t bytesRead,
                                     const char* /*errorMsg*/) {
    auto pendIt = m_pendingByReadId.find(id);
    if (pendIt == m_pendingByReadId.end())
        return; // already evicted — ignore
    const PendingRead pending = pendIt->second;
    m_pendingByReadId.erase(pendIt);

    auto tileIt = m_tiles.find(pending.key);
    if (tileIt == m_tiles.end())
        return; // evicted between cancelRead() and this callback

    if (pending.layer == TileLayer::Height) {
        if (status == AsyncReadStatus::Success && data != nullptr && bytesRead > 0) {
            int w = 0, h = 0;
            auto heights = fl::decodeTerrainChunkPng(static_cast<const uint8_t*>(data), bytesRead, &w, &h);
            if (!heights.empty() && w == kTileHeightmapSize && h == kTileHeightmapSize) {
                bool coverStillPending = false;
                {
                    std::unique_lock lock(m_tileMutex);
                    Tile& tile = tileIt->second;
                    tile.heightmap = std::move(heights);
                    tile.pendingHeightRead = 0;
                    coverStillPending = tile.pendingCoverRead != 0;
                }
                if (!coverStillPending)
                    finalizeTile(pending.key);
                return;
            }
            std::fprintf(stderr,
                         "[TerrainStreamer] tile f%u L%u %u,%u: expected %dx%d PNG, got %dx%d — "
                         "falling back to procedural\n",
                         pending.key.face, pending.key.level, pending.key.i, pending.key.j, kTileHeightmapSize,
                         kTileHeightmapSize, w, h);
        }
        // Error, cancelled, decode failure, or size mismatch — procedural fallback.
        // Cancel a cover read still in flight, then rebuild the entry from scratch.
        {
            std::unique_lock lock(m_tileMutex);
            Tile& tile = tileIt->second;
            if (tile.pendingCoverRead != 0) {
                m_asyncFs.cancelRead(tile.pendingCoverRead);
                m_pendingByReadId.erase(tile.pendingCoverRead);
            }
            m_tiles.erase(tileIt);
        }
        loadTileProcedural(pending.key);
        return;
    }

    // LandCover layer: optional — attach on success, proceed without on failure.
    {
        std::unique_lock lock(m_tileMutex);
        Tile& tile = tileIt->second;
        tile.pendingCoverRead = 0;
        if (status == AsyncReadStatus::Success && data != nullptr && bytesRead > 0) {
            int w = 0, h = 0;
            auto cover16 = fl::decodeTerrainChunkPng(static_cast<const uint8_t*>(data), bytesRead, &w, &h);
            if (!cover16.empty() && w == kTileHeightmapSize && h == kTileHeightmapSize) {
                tile.landCover.resize(cover16.size());
                for (std::size_t px = 0; px < cover16.size(); ++px)
                    tile.landCover[px] = static_cast<uint8_t>(cover16[px] & 0xFFu); // low byte = class
            }
        }
    }
    bool heightArrived = false;
    {
        std::shared_lock lock(m_tileMutex);
        const Tile& tile = tileIt->second;
        heightArrived = !tile.heightmap.empty() && tile.pendingHeightRead == 0;
    }
    if (heightArrived)
        finalizeTile(pending.key);
}

// ---------------------------------------------------------------------------
// finalizeTile
// ---------------------------------------------------------------------------

void TerrainStreamer::finalizeTile(const TileKey& key) {
    // The main thread is the sole writer of m_tiles, so reading the tile's decoded
    // data needs no lock and the expensive mesh build + GPU upload run unlocked —
    // concurrent sim-thread height queries (shared_lock) never stall behind them.
    // Only the state/mesh publication at the end takes the writer lock.
    auto it = m_tiles.find(key);
    if (it == m_tiles.end() || it->second.heightmap.empty())
        return;
    Tile& tile = it->second;

    MeshHandle mesh{};
    if (m_renderer) {
        // Create shared terrain material on first use.
        if (!m_terrainMat.valid()) {
            MaterialDesc md{};
            md.baseColorFactor = {0.55f, 0.50f, 0.35f, 1.0f}; // sandy tan
            md.roughnessFactor = 0.9f;
            md.metallicFactor = 0.0f;
            m_terrainMat = m_renderer->createMaterial(md);
        }

        const glm::dvec3 origin = tileToWorld(key, 0.5, 0.5, 0.0, m_planetRadiusM);

        // Runway flattening (#486): when a height modifier is installed and this tile's bounding
        // sphere could hold a runway footprint, flatten a COPY of the heightmap so the mesh vertices
        // match the modified heightAt(). The stored heightmap stays raw (heightAt re-applies the
        // modifier — never double-counted); the per-tile region test keeps this off tiles no airport
        // touches, and with no modifier the output is byte-identical to before.
        const std::vector<uint16_t>* heights = &tile.heightmap;
        std::vector<uint16_t> flattened;
        if (m_heightModifier) {
            const glm::dvec3 corner = tileToWorld(key, 0.0, 0.0, 0.0, m_planetRadiusM);
            const double radius = glm::length(corner - origin);
            if (!m_heightModifierRegion || m_heightModifierRegion(origin, radius)) {
                const int S = kTileHeightmapSize;
                flattened = tile.heightmap;
                for (int row = 0; row < S; ++row) {
                    const double t = static_cast<double>(row) / static_cast<double>(S - 1);
                    for (int col = 0; col < S; ++col) {
                        const double sCoord = static_cast<double>(col) / static_cast<double>(S - 1);
                        const std::size_t idx = static_cast<std::size_t>(row) * S + col;
                        const double raw = static_cast<double>(tile.heightmap[idx]) - 32768.0;
                        const glm::dvec3 wp = tileToWorld(key, sCoord, t, raw, m_planetRadiusM);
                        const double v = std::clamp(m_heightModifier(wp, raw) + 32768.0, 0.0, 65535.0);
                        flattened[idx] = static_cast<uint16_t>(v + 0.5);
                    }
                }
                heights = &flattened;
            }
        }

        auto glb =
            buildTileMeshGlb(*heights, kTileHeightmapSize, kTileMeshGrid, key, m_planetRadiusM, origin,
                             tile.landCover.empty() ? nullptr : tile.landCover.data(), true, skirtDepthFor(key.level));
        if (!glb.empty()) {
            const std::string meshName = "tile:" + m_manifest.terrainId + ":f" + std::to_string(key.face) + ":L" +
                                         std::to_string(key.level) + ":" + std::to_string(key.i) + ":" +
                                         std::to_string(key.j);
            mesh = m_renderer->createMesh({meshName, glb});
        }
        // Mesh-build failure still publishes Ready: the heightmap is valid (height
        // queries and the covering chain must not stall on it); the render walk
        // skips tiles without a valid mesh.
    }

    std::unique_lock lock(m_tileMutex);
    tile.state = TileState::Ready;
    tile.pendingHeightRead = 0;
    tile.mesh = mesh;
}

// ---------------------------------------------------------------------------
// evictTile
// ---------------------------------------------------------------------------

void TerrainStreamer::evictTile(const TileKey& key) {
    std::unique_lock lock(m_tileMutex);
    auto it = m_tiles.find(key);
    if (it == m_tiles.end())
        return;
    Tile& tile = it->second;
    if (tile.pendingHeightRead != 0) {
        m_asyncFs.cancelRead(tile.pendingHeightRead);
        m_pendingByReadId.erase(tile.pendingHeightRead);
    }
    if (tile.pendingCoverRead != 0) {
        m_asyncFs.cancelRead(tile.pendingCoverRead);
        m_pendingByReadId.erase(tile.pendingCoverRead);
    }
    if (m_renderer && tile.mesh.valid())
        m_renderer->destroyMesh(tile.mesh);
    m_tiles.erase(it);
}

// ---------------------------------------------------------------------------
// getRenderItems
// ---------------------------------------------------------------------------

bool TerrainStreamer::emitRenderTiles(const TileKey& key, std::vector<const Tile*>& outTiles,
                                      std::vector<TileKey>& outKeys) const {
    if (m_desiredLeaves.find(key) == m_desiredLeaves.end()) {
        // Interior node: try to emit the child subtrees; if any part cannot render
        // yet, roll back its emissions and fall through to drawing this tile as the
        // coarse fallback — coverage never overlaps (a finer tile is never drawn
        // under a fallback ancestor).
        const std::size_t mark = outTiles.size();
        bool allOk = true;
        for (uint8_t q = 0; q < 4; ++q) {
            const TileKey c = child(key, q);
            if (m_desiredAll.find(c) == m_desiredAll.end() || !emitRenderTiles(c, outTiles, outKeys)) {
                allOk = false;
                break;
            }
        }
        if (allOk)
            return true;
        outTiles.resize(mark);
        outKeys.resize(mark);
    }
    // Leaf, or interior whose subtree is still loading: draw this tile if it can.
    auto it = m_tiles.find(key);
    if (it != m_tiles.end() && it->second.state == TileState::Ready && it->second.mesh.valid()) {
        outTiles.push_back(&it->second);
        outKeys.push_back(key);
        return true;
    }
    return false; // brief hole while the coarse ancestor itself is still loading
}

std::vector<RenderItem> TerrainStreamer::getRenderItems(glm::dvec3 worldOrigin) const {
    if (!m_updated || !m_renderer)
        return {};

    std::vector<const Tile*> tiles;
    std::vector<TileKey> keys;
    tiles.reserve(m_desiredLeaves.size());
    keys.reserve(m_desiredLeaves.size());
    for (uint8_t f = 0; f < kCubeFaceCount; ++f)
        emitRenderTiles(TileKey{f, 0, 0, 0}, tiles, keys);

    std::vector<RenderItem> items;
    items.reserve(tiles.size());
    for (std::size_t t = 0; t < tiles.size(); ++t) {
        // Camera-relative translation: tile rebase origin in world -> relative to camera.
        // All curvature is baked into the vertices relative to this origin (#471).
        const glm::dvec3 origin = tileToWorld(keys[t], 0.5, 0.5, 0.0, m_planetRadiusM);
        const glm::vec3 relOrigin = glm::vec3(origin - worldOrigin);

        RenderItem item;
        item.mesh = tiles[t]->mesh;
        item.material = m_terrainMat;
        item.transform = glm::translate(glm::mat4(1.0f), relOrigin);
        item.flags = kRenderFlagTerrain; // forward pass applies elevation/slope shading
        items.push_back(item);
    }
    return items;
}

// ---------------------------------------------------------------------------
// Height / surface queries
// ---------------------------------------------------------------------------

const TerrainStreamer::Tile* TerrainStreamer::deepestReadyTile(const TileCoord& tc, TileKey* outKey,
                                                               bool requireLandCover) const noexcept {
    const Tile* best = nullptr;
    TileKey bestKey{};
    for (int level = 0; level <= m_manifest.maxTileLevel; ++level) {
        TileKey k;
        k.face = tc.key.face;
        k.level = static_cast<uint8_t>(level);
        k.i = tileIndexForUv(tc.s, k.level);
        k.j = tileIndexForUv(tc.t, k.level);
        auto it = m_tiles.find(k);
        if (it == m_tiles.end() || it->second.state != TileState::Ready)
            break; // finer never loaded (or still loading) — stop at the last Ready level
        if (requireLandCover && it->second.landCover.empty())
            continue; // keep walking: a deeper tile may carry the layer
        best = &it->second;
        bestKey = k;
    }
    if (outKey)
        *outKey = bestKey;
    return best;
}

double TerrainStreamer::heightAt(glm::dvec3 worldPos) const noexcept {
    std::shared_lock lock(m_tileMutex);

    const TileCoord tc = worldToTile(worldPos, m_planetRadiusM);
    TileKey key{};
    const Tile* tile = deepestReadyTile(tc, &key);
    if (!tile || tile->heightmap.empty())
        return 0.0;

    // Tile-local (s, t) of the query point within the found tile.
    const double scale = static_cast<double>(uint64_t{1} << key.level);
    const double ls = std::clamp(tc.s * scale - static_cast<double>(key.i), 0.0, 1.0);
    const double lt = std::clamp(tc.t * scale - static_cast<double>(key.j), 0.0, 1.0);

    const int s = kTileHeightmapSize;
    const double px = ls * static_cast<double>(s - 1);
    const double pz = lt * static_cast<double>(s - 1);
    const int ix = std::clamp(static_cast<int>(px), 0, s - 2);
    const int iz = std::clamp(static_cast<int>(pz), 0, s - 2);
    const double fx = px - static_cast<double>(ix);
    const double fz = pz - static_cast<double>(iz);

    auto h = [&](int col, int row) noexcept -> double {
        return static_cast<double>(tile->heightmap[static_cast<std::size_t>(row) * s + col]) - 32768.0;
    };

    const double raw =
        glm::mix(glm::mix(h(ix, iz), h(ix + 1, iz), fx), glm::mix(h(ix, iz + 1), h(ix + 1, iz + 1), fx), fz);
    // Runway flattening (#486): the modifier overrides the sampled terrain inside a runway footprint
    // (returning `raw` untouched elsewhere). The same function flattens the tile mesh, so the physics
    // floor and the visible terrain agree. Cheap when no airport is near (the modifier early-outs).
    return m_heightModifier ? m_heightModifier(worldPos, raw) : raw;
}

bool TerrainStreamer::heightReadyAt(glm::dvec3 worldPos) const noexcept {
    std::shared_lock lock(m_tileMutex);
    const TileCoord tc = worldToTile(worldPos, m_planetRadiusM);
    TileKey key{};
    const Tile* tile = deepestReadyTile(tc, &key);
    const int required = std::min(m_manifest.maxTileLevel, kHeightReadyMinLevel);
    return tile != nullptr && static_cast<int>(key.level) >= required;
}

uint8_t TerrainStreamer::surfaceAt(glm::dvec3 worldPos) const noexcept {
    std::shared_lock lock(m_tileMutex);

    const TileCoord tc = worldToTile(worldPos, m_planetRadiusM);
    // Deepest Ready tile that carries a land-cover layer.
    TileKey bestKey{};
    const Tile* best = deepestReadyTile(tc, &bestKey, /*requireLandCover=*/true);
    if (!best)
        return 0;

    const double scale = static_cast<double>(uint64_t{1} << bestKey.level);
    const double ls = std::clamp(tc.s * scale - static_cast<double>(bestKey.i), 0.0, 1.0);
    const double lt = std::clamp(tc.t * scale - static_cast<double>(bestKey.j), 0.0, 1.0);
    const int s = kTileHeightmapSize;
    const int ix = std::clamp(static_cast<int>(ls * (s - 1) + 0.5), 0, s - 1);
    const int iz = std::clamp(static_cast<int>(lt * (s - 1) + 0.5), 0, s - 1);
    return best->landCover[static_cast<std::size_t>(iz) * s + ix];
}

// ---------------------------------------------------------------------------
// Misc
// ---------------------------------------------------------------------------

std::size_t TerrainStreamer::tileCount() const noexcept {
    std::shared_lock lock(m_tileMutex);
    return m_tiles.size();
}

std::vector<TileKey> TerrainStreamer::desiredLeaves() const {
    return {m_desiredLeaves.begin(), m_desiredLeaves.end()};
}

void TerrainStreamer::setResidencyCap(std::size_t cap) noexcept {
    m_maxResidentTiles = cap > 0 ? cap : 1;
}

void TerrainStreamer::setPlanetRadius(double radius_m) {
    if (!(radius_m > 0.0) || radius_m == m_planetRadiusM)
        return;
    // Tiles bake the radius (curvature + procedural elevations) at generation time:
    // drop every resident tile and in-flight read so the next update() regenerates
    // at the new radius instead of leaving stale-curvature terrain behind.
    std::unique_lock lock(m_tileMutex);
    for (auto& [id, pending] : m_pendingByReadId)
        m_asyncFs.cancelRead(id);
    m_pendingByReadId.clear();
    if (m_renderer) {
        for (auto& [key, tile] : m_tiles) {
            if (tile.mesh.valid())
                m_renderer->destroyMesh(tile.mesh);
        }
    }
    m_tiles.clear();
    m_desiredLeaves.clear();
    m_desiredAll.clear();
    m_planetRadiusM = radius_m;
}

void TerrainStreamer::setHeightModifier(HeightModifier pointFn, HeightModifierRegion regionFn) {
    // Drop every resident tile + in-flight read so the next update() re-generates meshes with the
    // flatten applied — mirrors setPlanetRadius, so wiring the modifier at any time can never leave a
    // stale un-flattened mesh (or a flattened one after the modifier is cleared) resident.
    std::unique_lock lock(m_tileMutex);
    for (auto& [id, pending] : m_pendingByReadId)
        m_asyncFs.cancelRead(id);
    m_pendingByReadId.clear();
    if (m_renderer) {
        for (auto& [key, tile] : m_tiles) {
            if (tile.mesh.valid())
                m_renderer->destroyMesh(tile.mesh);
        }
    }
    m_tiles.clear();
    m_desiredLeaves.clear();
    m_desiredAll.clear();
    m_heightModifier = std::move(pointFn);
    m_heightModifierRegion = std::move(regionFn);
}

void TerrainStreamer::setViewParams(float screenHeightPx, float fovYRad) noexcept {
    if (screenHeightPx > 0.f)
        m_screenHeightPx = screenHeightPx;
    if (fovYRad > 0.f)
        m_fovYRad = fovYRad;
}

} // namespace fl
