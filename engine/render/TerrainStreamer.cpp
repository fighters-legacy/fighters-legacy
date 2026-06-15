// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/TerrainStreamer.h"

#include "IRenderer.h"
#include "content/AssetManager.h"
#include "render/ProceduralTerrainChunk.h"
#include "render/TerrainChunkIO.h"
#include "render/TerrainMeshBuilder.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <glm/gtc/matrix_transform.hpp>

namespace fl {

// ---------------------------------------------------------------------------
// ChunkKeyHash
// ---------------------------------------------------------------------------

std::size_t TerrainStreamer::ChunkKeyHash::operator()(const ChunkKey& k) const noexcept {
    std::size_t h = std::hash<int32_t>{}(k.cx);
    h ^= std::hash<int32_t>{}(k.cy) + 0x9e3779b9u + (h << 6) + (h >> 2);
    h ^= std::hash<uint32_t>{}(k.lod) + 0x9e3779b9u + (h << 6) + (h >> 2);
    return h;
}

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------

int TerrainStreamer::hmSizeForLod(int lod) noexcept {
    switch (lod) {
    case 0:
        return 513;
    case 1:
        return 257;
    default:
        return 129;
    }
}

int TerrainStreamer::meshGridForLod(int lod) noexcept {
    switch (lod) {
    case 0:
        return 128;
    case 1:
        return 64;
    default:
        return 32;
    }
}

// ---------------------------------------------------------------------------
// Constructor / destructor
// ---------------------------------------------------------------------------

TerrainStreamer::TerrainStreamer(fl::TerrainManifest manifest, AssetManager& assets, IAsyncFilesystem& asyncFs,
                                 IRenderer* renderer)
    : m_manifest(std::move(manifest)), m_assets(assets), m_asyncFs(asyncFs), m_renderer(renderer) {
    m_asyncFs.setEventHandler(this);
}

TerrainStreamer::~TerrainStreamer() {
    // Deregister first so any late service() calls hit a null handler, not dead this.
    m_asyncFs.setEventHandler(nullptr);
    // Cancel all in-flight reads (callbacks now go nowhere).
    for (auto& [id, key] : m_pendingByReadId)
        m_asyncFs.cancelRead(id);
    // Destroy GPU resources.
    if (m_renderer) {
        for (auto& [key, chunk] : m_chunks) {
            if (chunk.mesh.valid())
                m_renderer->destroyMesh(chunk.mesh);
        }
        if (m_terrainMat.valid())
            m_renderer->destroyMaterial(m_terrainMat);
    }
}

// ---------------------------------------------------------------------------
// update
// ---------------------------------------------------------------------------

void TerrainStreamer::update(glm::dvec3 cameraWorldPos) {
    const int centerCx = static_cast<int>(std::floor((cameraWorldPos.x - m_manifest.originX) / m_manifest.chunkSizeM));
    const int centerCy = static_cast<int>(std::floor((cameraWorldPos.z - m_manifest.originZ) / m_manifest.chunkSizeM));

    m_lastCx = centerCx;
    m_lastCy = centerCy;

    // Build desired set
    std::unordered_map<ChunkKey, bool, ChunkKeyHash> desired;
    desired.reserve(83);
    for (int lod = 0; lod < kNumLods; ++lod) {
        const int he = kLodHalfExtent[lod];
        for (int dy = -he; dy <= he; ++dy) {
            for (int dx = -he; dx <= he; ++dx) {
                ChunkKey key;
                key.cx = centerCx + dx;
                key.cy = centerCy + dy;
                key.lod = static_cast<uint32_t>(lod);
                desired[key] = true;
            }
        }
    }

    // Evict chunks no longer wanted (collect first to avoid iterator invalidation)
    std::vector<ChunkKey> toEvict;
    for (auto& [key, chunk] : m_chunks) {
        if (desired.find(key) == desired.end())
            toEvict.push_back(key);
    }
    for (auto& key : toEvict)
        evictChunk(key);

    // Load new chunks (rate-limited for procedural fallback)
    int proceduralCount = 0;
    for (auto& [key, _] : desired) {
        if (m_chunks.find(key) == m_chunks.end())
            loadChunk(key, proceduralCount);
    }
}

// ---------------------------------------------------------------------------
// loadChunk
// ---------------------------------------------------------------------------

void TerrainStreamer::loadChunk(const ChunkKey& key, int& proceduralCount) {
    auto path = m_assets.resolveTerrainChunk(m_manifest.terrainId.c_str(), static_cast<uint32_t>(key.cx),
                                             static_cast<uint32_t>(key.cy), key.lod);
    if (!path) {
        // Procedural fallback — rate-limited to avoid first-frame hitch
        if (proceduralCount >= kMaxProceduralPerUpdate)
            return;
        ++proceduralCount;
        loadChunkProcedural(key);
        return;
    }

    // Queue async read
    Chunk& chunk = m_chunks[key];
    chunk.state = ChunkState::Loading;
    chunk.pendingReadId = 0;

    const AsyncReadId id = m_asyncFs.readFileAsync(PathDomain::Assets, path->c_str());
    if (id == 0) {
        // readFileAsync failed — remove the entry and retry next frame
        m_chunks.erase(key);
        return;
    }
    chunk.pendingReadId = id;
    m_pendingByReadId[id] = key;
}

// ---------------------------------------------------------------------------
// loadChunkProcedural (no rate limiting — used from error fallback path too)
// ---------------------------------------------------------------------------

void TerrainStreamer::loadChunkProcedural(const ChunkKey& key) {
    const int lod = static_cast<int>(key.lod);
    const int hmSize = hmSizeForLod(lod);
    static constexpr int kFullSize = 513;

    auto full = fl::generateProceduralChunk(key.cx, key.cy, m_manifest, fl::kBuiltinProceduralParams);

    // Subsample full 513×513 down to target hmSize
    const std::size_t step = static_cast<std::size_t>((kFullSize - 1) / (hmSize - 1)); // 1, 2, or 4
    std::vector<uint16_t> heights(static_cast<std::size_t>(hmSize) * hmSize);
    for (int r = 0; r < hmSize; ++r) {
        for (int c = 0; c < hmSize; ++c) {
            heights[static_cast<std::size_t>(r) * static_cast<std::size_t>(hmSize) + static_cast<std::size_t>(c)] =
                full[static_cast<std::size_t>(r) * step * kFullSize + static_cast<std::size_t>(c) * step];
        }
    }
    finalizeChunk(key, std::move(heights), hmSize);
}

// ---------------------------------------------------------------------------
// onReadComplete
// ---------------------------------------------------------------------------

void TerrainStreamer::onReadComplete(AsyncReadId id, AsyncReadStatus status, const void* data, std::size_t bytesRead,
                                     const char* /*errorMsg*/) {
    auto pendIt = m_pendingByReadId.find(id);
    if (pendIt == m_pendingByReadId.end())
        return; // already evicted — ignore

    const ChunkKey key = pendIt->second;
    m_pendingByReadId.erase(pendIt);

    // If the chunk was evicted between cancelRead() and this callback, bail out.
    if (m_chunks.find(key) == m_chunks.end())
        return;

    if (status == AsyncReadStatus::Success && data != nullptr && bytesRead > 0) {
        int w = 0, h = 0;
        auto heights = fl::decodeTerrainChunkPng(static_cast<const uint8_t*>(data), bytesRead, &w, &h);
        if (!heights.empty()) {
            const int expected = hmSizeForLod(static_cast<int>(key.lod));
            if (w == expected && h == expected) {
                finalizeChunk(key, std::move(heights), w);
                return;
            }
            std::fprintf(stderr,
                         "[TerrainStreamer] chunk %d,%d lod%u: expected %dx%d PNG, got %dx%d — "
                         "falling back to procedural\n",
                         key.cx, key.cy, key.lod, expected, expected, w, h);
        }
    }

    // Error, Cancelled, decode failure, or size mismatch — fall back to procedural.
    // Erase the Loading entry so loadChunkProcedural can re-insert as Ready.
    m_chunks.erase(key);
    loadChunkProcedural(key);
}

// ---------------------------------------------------------------------------
// finalizeChunk
// ---------------------------------------------------------------------------

void TerrainStreamer::finalizeChunk(const ChunkKey& key, std::vector<uint16_t> heights, int hmSize) {
    Chunk& chunk = m_chunks[key];
    chunk.heightmap = std::move(heights);
    chunk.hmSize = hmSize;
    chunk.state = ChunkState::Ready;
    chunk.pendingReadId = 0;

    if (!m_renderer)
        return;

    // Create shared terrain material on first use
    if (!m_terrainMat.valid()) {
        MaterialDesc md{};
        md.baseColorFactor = {0.55f, 0.50f, 0.35f, 1.0f}; // sandy tan
        md.roughnessFactor = 0.9f;
        md.metallicFactor = 0.0f;
        m_terrainMat = m_renderer->createMaterial(md);
    }

    const int lod = static_cast<int>(key.lod);
    const int meshGrid = meshGridForLod(lod);
    auto glb = fl::buildTerrainMeshGlb(chunk.heightmap, hmSize, meshGrid, m_manifest.chunkSizeM);
    if (glb.empty())
        return;

    const std::string meshName = "terrain:" + m_manifest.terrainId + ":" + std::to_string(key.cx) + ":" +
                                 std::to_string(key.cy) + ":lod" + std::to_string(lod);
    chunk.mesh = m_renderer->createMesh({meshName, glb});
}

// ---------------------------------------------------------------------------
// evictChunk
// ---------------------------------------------------------------------------

void TerrainStreamer::evictChunk(const ChunkKey& key) {
    auto it = m_chunks.find(key);
    if (it == m_chunks.end())
        return;
    Chunk& chunk = it->second;
    if (chunk.state == ChunkState::Loading && chunk.pendingReadId != 0) {
        m_asyncFs.cancelRead(chunk.pendingReadId);
        m_pendingByReadId.erase(chunk.pendingReadId);
    }
    if (m_renderer && chunk.mesh.valid())
        m_renderer->destroyMesh(chunk.mesh);
    m_chunks.erase(it);
}

// ---------------------------------------------------------------------------
// getRenderItems
// ---------------------------------------------------------------------------

std::vector<RenderItem> TerrainStreamer::getRenderItems(glm::dvec3 worldOrigin) const {
    if (m_lastCx == kNeverUpdated)
        return {};

    std::vector<RenderItem> items;
    items.reserve(49); // up to 7×7 unique positions

    const int he = kLodHalfExtent[kNumLods - 1]; // outermost ring half-extent = 3
    for (int dy = -he; dy <= he; ++dy) {
        for (int dx = -he; dx <= he; ++dx) {
            const int cx = m_lastCx + dx;
            const int cy = m_lastCy + dy;
            const int dist = std::max(std::abs(dx), std::abs(dy));

            // Determine target LOD for this position
            const int targetLod = (dist <= kLodHalfExtent[0]) ? 0 : (dist <= kLodHalfExtent[1]) ? 1 : 2;

            // Find best ready LOD (target first, then fallback to coarser)
            const Chunk* best = nullptr;
            for (int lod = targetLod; lod < kNumLods; ++lod) {
                ChunkKey key;
                key.cx = cx;
                key.cy = cy;
                key.lod = static_cast<uint32_t>(lod);
                auto it = m_chunks.find(key);
                if (it != m_chunks.end() && it->second.state == ChunkState::Ready) {
                    best = &it->second;
                    break;
                }
            }

            if (!best || !best->mesh.valid())
                continue;

            // Camera-relative translation: chunk local-origin in world → relative to camera
            const double chunkCornerX = m_manifest.originX + static_cast<double>(cx) * m_manifest.chunkSizeM;
            const double chunkCornerZ = m_manifest.originZ + static_cast<double>(cy) * m_manifest.chunkSizeM;
            double yOffset = 0.0;
            if (m_sphericalRadius > 0.0) {
                const double R = m_sphericalRadius;
                const double D2 = chunkCornerX * chunkCornerX + chunkCornerZ * chunkCornerZ;
                yOffset = std::sqrt(std::max(0.0, R * R - D2)) - R;
            }
            const glm::dvec3 chunkOrigin{chunkCornerX, yOffset, chunkCornerZ};
            const glm::vec3 relOrigin = glm::vec3(chunkOrigin - worldOrigin);

            RenderItem item;
            item.mesh = best->mesh;
            item.material = m_terrainMat;
            item.transform = glm::translate(glm::mat4(1.0f), relOrigin);
            items.push_back(item);
        }
    }
    return items;
}

// ---------------------------------------------------------------------------
// heightAt / surfaceAt / chunkCount
// ---------------------------------------------------------------------------

double TerrainStreamer::heightAt(double x, double z) const noexcept {
    const int cx = static_cast<int>(std::floor((x - m_manifest.originX) / m_manifest.chunkSizeM));
    const int cy = static_cast<int>(std::floor((z - m_manifest.originZ) / m_manifest.chunkSizeM));

    ChunkKey key;
    key.cx = cx;
    key.cy = cy;
    key.lod = 0u; // LOD0 only for highest accuracy

    auto it = m_chunks.find(key);
    if (it == m_chunks.end() || it->second.state != ChunkState::Ready)
        return 0.0;

    const Chunk& chunk = it->second;
    const int s = chunk.hmSize;

    // Local position within chunk [0, chunkSizeM)
    const double lx = (x - m_manifest.originX) - static_cast<double>(cx) * m_manifest.chunkSizeM;
    const double lz = (z - m_manifest.originZ) - static_cast<double>(cy) * m_manifest.chunkSizeM;

    // Pixel coords [0, s-1]
    const double px = lx / m_manifest.chunkSizeM * static_cast<double>(s - 1);
    const double pz = lz / m_manifest.chunkSizeM * static_cast<double>(s - 1);

    const int ix = std::clamp(static_cast<int>(px), 0, s - 2);
    const int iz = std::clamp(static_cast<int>(pz), 0, s - 2);
    const double fx = px - static_cast<double>(ix);
    const double fz = pz - static_cast<double>(iz);

    auto h = [&](int col, int row) noexcept -> double {
        return static_cast<double>(chunk.heightmap[static_cast<std::size_t>(row) * s + col]) - 32768.0;
    };

    double elevation =
        glm::mix(glm::mix(h(ix, iz), h(ix + 1, iz), fx), glm::mix(h(ix, iz + 1), h(ix + 1, iz + 1), fx), fz);

    if (m_sphericalRadius > 0.0) {
        const double R = m_sphericalRadius;
        const double D2 = x * x + z * z;
        elevation += std::sqrt(std::max(0.0, R * R - D2)) - R;
    }

    return elevation;
}

uint8_t TerrainStreamer::surfaceAt(double /*x*/, double /*z*/) const noexcept {
    return 0;
}

std::size_t TerrainStreamer::chunkCount() const noexcept {
    return m_chunks.size();
}

void TerrainStreamer::setSphericalPlanetRadius(double radius_m) noexcept {
    m_sphericalRadius = radius_m;
}

} // namespace fl
