// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IAsyncFilesystem.h"
#include "RenderTypes.h"
#include "render/CubeSphere.h"
#include "render/SurfaceType.h"
#include "render/TerrainManifest.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

namespace fl {
class AssetManager;
class IRenderer;
} // namespace fl

namespace fl {

// Cube-sphere quadtree terrain streamer (#472, spherical-Earth epic #468).
//
// Replaces the planar ring-LOD chunk streamer: the world is the six-face CubeSphere
// quadtree (TileKey), every tile carries a uniform kTileHeightmapSize^2 heightmap
// (resolution comes from quadtree depth, not per-tile LOD pyramids), and refinement is
// screen-space-error driven — a tile splits while its projected geometric error exceeds
// kSseTauPx, up to manifest.maxTileLevel. A 2:1 edge-balance pass (restricted quadtree,
// CubeSphere::neighbor) bounds the level delta across shared edges; residual hairline
// cracks are hidden by mesh skirts (buildTileMeshGlb skirtDepthM).
//
// Tile content: content packs are probed first via
// AssetManager::resolveTilePath(terrainId, face, level, i, j, layer) (#473 — resolves
// terrain/<id>/f<face>/l<level>/tile_<i>_<j>{.png,_lc.png}); misses fall back to
// generateProceduralTile — deterministic global-sphere
// FBM, so a headless server and every client generate bit-identical terrain with no
// wire transfer. Async reads are tracked per {TileKey, layer} (height + optional
// land-cover); a tile finalizes when its required layers arrive.
//
// Height queries take full world positions (dvec3) and are radial: heightAt returns
// the terrain height ABOVE THE SPHERE DATUM along the radial through the query point
// (the `h` of CubeSphere::tileToWorld). Transitional (x, z) overloads preserve the old
// near-side world-Y semantics for existing callers and are removed by the radial
// ground floor (#477).
//
// Implements IAsyncFilesystemHandler; registers as the sole event handler on
// construction and deregisters on destruction. Only one TerrainStreamer may be live
// per IAsyncFilesystem instance.
//
// Threading: update(), getRenderItems(), and tileCount() are main-thread only.
// heightAt()/heightReadyAt()/surfaceAt() are safe from any thread (m_tileMutex).
class TerrainStreamer : public IAsyncFilesystemHandler {
  public:
    // renderer may be nullptr for headless operation (height queries work;
    // getRenderItems returns an empty vector).
    TerrainStreamer(fl::TerrainManifest manifest, AssetManager& assets, IAsyncFilesystem& asyncFs, IRenderer* renderer);
    ~TerrainStreamer() override;

    TerrainStreamer(const TerrainStreamer&) = delete;
    TerrainStreamer& operator=(const TerrainStreamer&) = delete;

    // Rebuild the desired tile tree for cameraWorldPos (SSE refinement + 2:1 balance),
    // queue loads for missing tiles (procedural generation rate-limited per call), and
    // evict least-recently-desired tiles beyond the residency cap. Call once per frame
    // before getRenderItems (or pump on the server until heightReadyAt).
    void update(glm::dvec3 cameraWorldPos);

    // One RenderItem per rendered tile: the desired tree is walked top-down and a
    // subtree is emitted only when it fully covers its region (a Ready interior tile
    // may stand in for children that are still loading); otherwise the walk rolls
    // back to the coarser ancestor — coverage never overlaps and never double-draws
    // a region. worldOrigin is subtracted for camera-relative rendering. Empty if
    // update() has never been called or renderer is null.
    std::vector<RenderItem> getRenderItems(glm::dvec3 worldOrigin) const;

    // Radial terrain height (m ABOVE THE SPHERE DATUM) along the radial through
    // worldPos, bilinearly sampled from the deepest Ready tile covering it.
    // Returns 0.0 when no tile covers the position yet. Thread-safe.
    double heightAt(glm::dvec3 worldPos) const noexcept;

    // True when the deepest Ready tile covering worldPos is at least
    // min(manifest.maxTileLevel, kHeightReadyMinLevel) deep — i.e. heightAt returns a
    // spawn-accurate elevation rather than a coarse or 0.0 placeholder. The server
    // pumps update()/service() against this before caching spawn points. Thread-safe.
    [[nodiscard]] bool heightReadyAt(glm::dvec3 worldPos) const noexcept;

    // Nearest-neighbour land-cover class from the deepest Ready tile carrying a
    // land-cover layer; 0 when none. Thread-safe.
    uint8_t surfaceAt(glm::dvec3 worldPos) const noexcept;

    // The typed surface classification at worldPos (#475): surfaceAt() mapped through the ESA
    // WorldCover → SurfaceType table. SurfaceType::Unknown where no land-cover layer covers the point.
    // Thread-safe. This is what gameplay/physics should query (e.g. gear-down on Water vs. Grass).
    [[nodiscard]] SurfaceType surfaceTypeAt(glm::dvec3 worldPos) const noexcept {
        return surfaceTypeFromWorldCover(surfaceAt(worldPos));
    }

    // Ocean depth (metres, positive DOWN) below the sphere datum (mean sea level) at worldPos (#476).
    // When bathymetry is present in the elevation source, `heightAt` returns real negative elevation
    // over ocean; this reports its magnitude, and 0 over land or where the tile is not yet loaded.
    // Thread-safe (delegates to heightAt).
    [[nodiscard]] float oceanDepthAt(glm::dvec3 worldPos) const noexcept {
        const double h = heightAt(worldPos);
        return h < 0.0 ? static_cast<float>(-h) : 0.f;
    }

    // Coarse deep/shallow classification for physics + surface typing (#476): shallow water (a
    // survivable gear-up ditching, continental-shelf depths) vs. deep ocean. `shallowMaxM` is the
    // depth boundary (default ≈ continental-shelf edge). False on land (depth 0).
    [[nodiscard]] bool isShallowWater(glm::dvec3 worldPos, float shallowMaxM = 200.f) const noexcept {
        const float d = oceanDepthAt(worldPos);
        return d > 0.f && d <= shallowMaxM;
    }

    // Total resident tile entries (all states). Exposed for tests.
    std::size_t tileCount() const noexcept;

    // Snapshot of the desired-tree leaves from the last update(). Test/telemetry
    // hook (e.g. asserting the 2:1 edge-balance property). Main-thread only.
    [[nodiscard]] std::vector<TileKey> desiredLeaves() const;

    // Override the resident-tile LRU cap (default kDefaultMaxResidentTiles). Tiles
    // outside the desired tree are cached up to this total and evicted
    // least-recently-desired first. Main-thread only; clamped to >= 1.
    void setResidencyCap(std::size_t cap) noexcept;

    // Override the planet radius (m). Default is 6 371 000 m (Earth). Tiles bake the
    // radius (curvature + procedural elevations) at generation time, so a radius
    // change drops every resident tile and cancels in-flight reads; the next update()
    // regenerates at the new radius. Same-value calls are no-ops; values <= 0 are
    // ignored. Main-thread only (like update()).
    void setPlanetRadius(double radius_m);

    // Runway terrain flattening seam (#486). `pointFn(worldPos, rawHeight)` returns the possibly
    // flattened terrain height above the datum; it is applied to BOTH heightAt() (the physics floor,
    // spawn priming, client prediction) AND every tile-mesh vertex, so the visible terrain and the
    // authoritative floor agree by construction. `regionFn(tileCentreWorld, tileRadiusM)` is the
    // per-tile early-out — false means "no modification can occur within this tile", so the per-vertex
    // flatten pass is skipped for the vast majority of tiles no airport touches (null = never skip).
    // Both callables come from the AirportRegistry (AirportRegistry::flattenedHeight / a bounding
    // test); routing them through std::function keeps engine-render free of an engine-world dep.
    // Setting a modifier drops every resident tile + cancels in-flight reads (setPlanetRadius
    // semantics), so no stale un-flattened mesh survives. Main-thread only.
    using HeightModifier = std::function<double(glm::dvec3 worldPos, double rawHeight)>;
    using HeightModifierRegion = std::function<bool(glm::dvec3 tileCentreWorld, double tileRadiusM)>;
    void setHeightModifier(HeightModifier pointFn, HeightModifierRegion regionFn = nullptr);

    // Current planet radius (m) the resident tiles were baked at. Used by callers that need to
    // convert world positions to radial (geodetic) altitude — e.g. the AGL overlay/HUD readouts.
    [[nodiscard]] double planetRadiusM() const noexcept {
        return m_planetRadiusM;
    }

    // Screen height (px) and vertical FOV (rad) for the SSE refinement metric.
    // Defaults (1080 px, 60 deg) are fine headless — server refinement only needs
    // "deep near the pumped position", not visual parity.
    void setViewParams(float screenHeightPx, float fovYRad) noexcept;

    // SSE refinement threshold (px): refine while projected error exceeds this.
    static constexpr float kSseTauPx = 3.0f;
    // heightReadyAt: minimum covering-tile level considered spawn-accurate.
    static constexpr int kHeightReadyMinLevel = 10;

  private:
    // IAsyncFilesystemHandler
    void onReadComplete(AsyncReadId id, AsyncReadStatus status, const void* data, std::size_t bytesRead,
                        const char* errorMsg) override;

    // -------------------------------------------------------------------------
    struct TileKeyHash {
        std::size_t operator()(const TileKey& k) const noexcept;
    };

    enum class TileState : uint8_t { Loading, Ready };
    enum class TileLayer : uint8_t { Height, LandCover };

    struct Tile {
        TileState state{TileState::Loading};
        AsyncReadId pendingHeightRead{0};
        AsyncReadId pendingCoverRead{0};
        std::vector<uint16_t> heightmap; // kTileHeightmapSize^2 when present
        std::vector<uint8_t> landCover;  // same dims; empty = no land-cover layer
        MeshHandle mesh{};
        uint64_t lastDesiredFrame{0};
    };

    struct PendingRead {
        TileKey key;
        TileLayer layer{TileLayer::Height};
    };

    using TileSet = std::unordered_set<TileKey, TileKeyHash>;

    // -------------------------------------------------------------------------
    // update() helpers (main thread)
    void refine(const TileKey& key, glm::dvec3 camPos, std::vector<TileKey>& leaves) const;
    [[nodiscard]] bool shouldRefine(const TileKey& key, glm::dvec3 camPos) const noexcept;
    void balanceLeaves(TileSet& leaves) const;
    void loadTile(const TileKey& key, int& proceduralCount);
    void loadTileProcedural(const TileKey& key);
    void finalizeTile(const TileKey& key);
    void evictTile(const TileKey& key);

    // Render-walk helper: emits the subtree under `key` into the out lists and returns
    // true when the whole desired subtree is covered; on false the caller (parent)
    // rolls back and draws its own coarser tile instead.
    bool emitRenderTiles(const TileKey& key, std::vector<const Tile*>& outTiles, std::vector<TileKey>& outKeys) const;

    // Deepest Ready tile covering the face-uv position (with a non-empty land-cover
    // layer when requireLandCover); nullptr when none (not even the root). Caller must
    // hold m_tileMutex (shared) or be the main thread.
    const Tile* deepestReadyTile(const TileCoord& tc, TileKey* outKey, bool requireLandCover = false) const noexcept;

    [[nodiscard]] double tileExtentM(int level) const noexcept;
    [[nodiscard]] double skirtDepthFor(int level) const noexcept;

    // -------------------------------------------------------------------------
    fl::TerrainManifest m_manifest;
    AssetManager& m_assets;
    IAsyncFilesystem& m_asyncFs;
    IRenderer* m_renderer{nullptr};
    MaterialHandle m_terrainMat{}; // single shared material, created on first use

    std::unordered_map<TileKey, Tile, TileKeyHash> m_tiles;
    std::unordered_map<AsyncReadId, PendingRead> m_pendingByReadId;

    // Desired tree from the last update(): leaves + every ancestor up to the roots.
    TileSet m_desiredLeaves;
    TileSet m_desiredAll;

    double m_planetRadiusM{6'371'000.0};
    float m_screenHeightPx{1080.f};
    float m_fovYRad{1.047197551f}; // 60 deg
    uint64_t m_frame{0};
    bool m_updated{false};

    // Runway terrain-flattening seam (#486). Guarded by m_tileMutex like the tile data: heightAt()
    // (any thread, shared lock) reads m_heightModifier, and setHeightModifier() (main thread) writes
    // it under the unique lock while dropping resident tiles.
    HeightModifier m_heightModifier;
    HeightModifierRegion m_heightModifierRegion;

    // Protects m_tiles for concurrent reads (height queries, sim thread) vs writes
    // (update/finalize/evict, main thread).
    mutable std::shared_mutex m_tileMutex;

    // Maximum procedural tiles generated per update() call (first-frame hitch guard).
    static constexpr int kMaxProceduralPerUpdate = 8;
    // Default resident-tile LRU cap (see setResidencyCap).
    static constexpr std::size_t kDefaultMaxResidentTiles = 1024;
    std::size_t m_maxResidentTiles{kDefaultMaxResidentTiles};
    // Fraction of a mesh quad used as the SSE geometric-error proxy (the height
    // deviation a coarser level introduces is a fraction of the quad extent).
    static constexpr double kGeomErrorFactor = 0.25;
};

} // namespace fl
