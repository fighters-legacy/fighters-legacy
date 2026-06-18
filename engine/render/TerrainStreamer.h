// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IAsyncFilesystem.h"
#include "RenderTypes.h"
#include "render/TerrainManifest.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

class AssetManager;
class IRenderer;

namespace fl {

// Manages terrain chunk lifecycle: async load via IAsyncFilesystem, LOD ring
// transitions, GPU mesh upload, and CPU-side height queries.
//
// Implements IAsyncFilesystemHandler; registers as the sole event handler on
// construction and deregisters on destruction. Only one TerrainStreamer may be
// live per IAsyncFilesystem instance.
//
// Threading: update(), getRenderItems(), chunkCount(), and surfaceAt() are main-thread only.
// heightAt() is safe to call from any thread (protected by m_chunkMutex).
class TerrainStreamer : public IAsyncFilesystemHandler {
  public:
    // renderer may be nullptr for headless operation (heightAt works; getRenderItems
    // returns an empty vector).
    TerrainStreamer(fl::TerrainManifest manifest, AssetManager& assets, IAsyncFilesystem& asyncFs, IRenderer* renderer);
    ~TerrainStreamer() override;

    TerrainStreamer(const TerrainStreamer&) = delete;
    TerrainStreamer& operator=(const TerrainStreamer&) = delete;

    // Compute the desired chunk set for cameraWorldPos, queue async reads for new
    // chunks, and evict out-of-range chunks. Call once per frame before getRenderItems.
    void update(glm::dvec3 cameraWorldPos);

    // Return a RenderItem for each Ready chunk (best available LOD per chunk
    // position). worldOrigin is subtracted for camera-relative rendering.
    // Returns empty if update() has never been called.
    std::vector<RenderItem> getRenderItems(glm::dvec3 worldOrigin) const;

    // Bilinear elevation query from the nearest LOD0 chunk (metres).
    // Returns 0.0 if no LOD0 chunk is loaded at that position.
    double heightAt(double x, double z) const noexcept;

    // True if the LOD0 chunk covering (x, z) is loaded and Ready, i.e. heightAt(x, z)
    // returns a real elevation rather than the 0.0 not-loaded sentinel. Used at server
    // startup to pump update()/service() until spawn-point terrain is ready, so entities
    // spawn at the correct elevation instead of y~0 (which the floor query later corrects,
    // producing a visible camera "snap up" on the client). Thread-safe.
    [[nodiscard]] bool heightReadyAt(double x, double z) const noexcept;

    // Nearest-neighbour surface class. Phase 2 stub: always returns 0.
    uint8_t surfaceAt(double x, double z) const noexcept;

    // Total loaded chunk entries across all LODs. Exposed for tests.
    std::size_t chunkCount() const noexcept;

    // Override the planet radius (m) used for terrain curvature.
    // Default is 6 371 000 m (Earth). Must be called before the first update(); changing it
    // after chunks have been loaded leaves stale meshes with the old curvature baked in.
    void setPlanetRadius(double radius_m) noexcept;

  private:
    // IAsyncFilesystemHandler
    void onReadComplete(AsyncReadId id, AsyncReadStatus status, const void* data, std::size_t bytesRead,
                        const char* errorMsg) override;

    // -------------------------------------------------------------------------
    struct ChunkKey {
        int32_t cx{0}, cy{0};
        uint32_t lod{0};
        bool operator==(const ChunkKey& o) const noexcept {
            return cx == o.cx && cy == o.cy && lod == o.lod;
        }
    };

    struct ChunkKeyHash {
        std::size_t operator()(const ChunkKey& k) const noexcept;
    };

    enum class ChunkState : uint8_t { Loading, Ready };

    struct Chunk {
        ChunkState state{ChunkState::Loading};
        AsyncReadId pendingReadId{0};
        std::vector<uint16_t> heightmap;
        int hmSize{0}; // number of samples per axis (513 / 257 / 129)
        MeshHandle mesh{};
    };

    // -------------------------------------------------------------------------
    // Called from update() with a per-frame rate-limit counter.
    void loadChunk(const ChunkKey& key, int& proceduralCount);
    // Generates heightmap from the builtin FBM and calls finalizeChunk immediately.
    void loadChunkProcedural(const ChunkKey& key);
    // Stores heights, sets state = Ready, uploads GPU mesh if renderer is set.
    void finalizeChunk(const ChunkKey& key, std::vector<uint16_t> heights, int hmSize);
    // Cancels any in-flight read, destroys GPU mesh, erases from m_chunks.
    void evictChunk(const ChunkKey& key);

    static int hmSizeForLod(int lod) noexcept;   // 513, 257, 129
    static int meshGridForLod(int lod) noexcept; // 128,  64,  32

    // -------------------------------------------------------------------------
    fl::TerrainManifest m_manifest;
    AssetManager& m_assets;
    IAsyncFilesystem& m_asyncFs;
    IRenderer* m_renderer{nullptr};
    MaterialHandle m_terrainMat{}; // single shared material, created on first use

    std::unordered_map<ChunkKey, Chunk, ChunkKeyHash> m_chunks;
    std::unordered_map<AsyncReadId, ChunkKey> m_pendingByReadId;

    // Planet radius (m) for terrain curvature. Default = Earth (6 371 000 m).
    double m_sphericalRadius{6'371'000.0};

    // Protects m_chunks for concurrent reads (heightAt, sim thread) vs. writes (update, main thread).
    mutable std::shared_mutex m_chunkMutex;

    // Last-known camera center chunk; sentinel = update() never called.
    // getRenderItems() returns empty immediately when m_lastCx is the sentinel.
    static constexpr int32_t kNeverUpdated = std::numeric_limits<int32_t>::min();
    int32_t m_lastCx{kNeverUpdated};
    int32_t m_lastCy{kNeverUpdated};

    // LOD ring half-extents: LOD 0 → 3×3 (he=1), LOD 1 → 5×5 (he=2), LOD 2 → 7×7 (he=3)
    static constexpr int kNumLods = 3;
    static constexpr int kLodHalfExtent[kNumLods] = {1, 2, 3};
    // Maximum procedural chunks generated per update() call to avoid a first-frame hitch.
    static constexpr int kMaxProceduralPerUpdate = 8;
};

} // namespace fl
