// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "RenderTypes.h"
#include "render/ArtChannel.h"
#include "render/BuiltinShape.h"
#include "render/MeshArticulation.h"

#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fl {
class AssetManager;
class ILogger;
class IRenderer;
class ParticleSystem;
class SimRenderBridge;
class SubtitleQueue;
class TerrainStreamer;
class AirportRegistry;
class AirportRenderer;
struct EntityRenderEntry; // render/RenderSnapshot.h
} // namespace fl

namespace fl {

// textureAssetNameFromUri / liveryKeyFromBaseAsset / makeMeshTextureResolver moved to
// render/MeshTextureResolver.h (#836) so the authoring-tools PreviewScene shares the exact
// URI->asset-name->file mapping and the two cannot drift.

// Converts the per-tick entity snapshot from SimRenderBridge into a FrameScene and submits
// it to IRenderer::setScene each frame.
//
// Threading: all methods must be called from the main (render) thread.
//
// Dependency injection:
//   MeshNameResolver breaks the circular CMake dep between engine-render and engine-entity.
//   Caller (Game.cpp) provides a lambda that captures &EntityTypeRegistry and resolves
//   typeIndex → ResolvedMesh (mesh names + the builtin placeholder shape, via
//   fl::builtinShapeFor in engine/entity/BuiltinShapeMap.h). Returns false if the
//   typeIndex is unknown — the entity then renders as the Unknown error beacon.
class SceneRenderer {
  public:
    // What a typeIndex resolves to for rendering (#886). An empty meshName means "draw the
    // builtin placeholder for `shape`"; an empty damageMeshName means "no authored damage
    // variant" (a builtin-rendered entity swaps to the shape's wreck instead).
    struct ResolvedMesh {
        std::string meshName;
        std::string damageMeshName;
        BuiltinShape shape{BuiltinShape::Unknown};
        // Variant node-set selector (#882), from EntityDef::meshVariant. Empty = the untagged node
        // set, i.e. the whole mesh for every .glb authored before variants existed. Folded into the
        // mesh cache key, so two variants of one family mesh are two GPU meshes.
        std::string variant;
    };

    // Fills `out` for a typeIndex. Returns true if the type is known; false leaves the
    // entity on the Unknown placeholder (and warns once per type, #832).
    using MeshNameResolver = std::function<bool(uint32_t typeIndex, ResolvedMesh& out)>;

    // Given a typeIndex and damageLevel (uint8_t cast of DamageLevel), returns the visual
    // effect preset name, or empty string if none.  Used to emit particle effects from
    // damaged entities without introducing a CMake dep on engine-entity.
    using EffectResolver = std::function<std::string(uint32_t typeIndex, uint8_t damageLevel)>;

    // A resolved livery for one aircraft type (#845): the livery id (used only to key the mesh/material
    // cache) plus the "<slot>.<map>" -> replacement Texture asset-name overrides. Empty id + empty
    // overrides = no livery (the mesh keeps its base textures). Built by the game from an engine-content
    // LiveryDef and passed in here, so SceneRenderer.h stays free of engine-content types.
    struct LiveryTextureSet {
        std::string id;
        std::unordered_map<std::string, std::string> overrides;
    };
    // Given an entity type index, fills `out` with the livery to apply and returns true, or returns
    // false for no livery. Called at most once per type index (cached). Pass nullptr to disable.
    using LiveryResolver = std::function<bool(uint32_t typeIndex, LiveryTextureSet& out)>;

    SceneRenderer(SimRenderBridge& bridge, MeshNameResolver resolver, AssetManager& assets, IRenderer& renderer);
    ~SceneRenderer();

    // Optional: wire a ParticleSystem to emit per-entity damage effects each frame.
    // effectResolver is called for each entity with damageLevel > 0; the returned preset
    // name is forwarded to ParticleSystem::emit(). Pass nullptr/empty to disable.
    void setParticleSystem(ParticleSystem* ps, EffectResolver effectResolver) noexcept;

    // Optional: wire a livery resolver (#845). When set, each content-mesh entity's textures are
    // resolved through the livery's "<slot>.<map>" overrides (per-map fallback to the base texture),
    // never its geometry. Pass nullptr to disable (default = every aircraft flies its base scheme).
    void setLiveryResolver(LiveryResolver resolver) noexcept;

    // Optional: wire a SubtitleQueue so renderFrame() populates FrameScene::subtitles.
    // Pass nullptr to disable. Rendering is deferred to Phase 4 IGui;
    // VkRenderer currently ignores the subtitles field.
    void setSubtitleQueue(SubtitleQueue* queue) noexcept;

    // Advance to the latest sim snapshot and submit a FrameScene to the renderer.
    // Must be called between IRenderer::beginFrame() and endFrame().
    // alpha — render-interpolation factor from GameLoop::shellTick(), in [0, 1].
    // extraEmitters — additional emitters beyond entity damage effects (may be empty).
    void renderFrame(float alpha, const CameraView& camera, const EnvironmentState& env,
                     std::span<const ParticleEmitterState> extraEmitters = {});

    // Set the maximum entity draw distance.  Entities beyond this range are
    // culled before building RenderItems.  Default is 50 km.
    void setDrawDistance(float distanceKm) noexcept;

    // When enabled, a 4 km flat floor plane is appended to every submitted FrameScene
    // as the last opaque RenderItem.  Uses the builtin olive-gray floor material.
    void setBuiltinFloor(bool show) noexcept;

    // Optional: wire a TerrainStreamer to append terrain chunk RenderItems to every
    // frame. Pass nullptr to disable (default). Streamer must outlive SceneRenderer.
    void setTerrainStreamer(TerrainStreamer* ts) noexcept;

    // Optional: wire an AirportRegistry to draw runways each frame (#487) via an owned AirportRenderer
    // (created lazily). Pass nullptr to disable. The registry must outlive SceneRenderer.
    void setAirportRegistry(const AirportRegistry* reg);

    // Secondary-camera inset viewport (#698/#695): render the same scene a second time from `view`
    // into the normalized `rect` (x, y, w, h, top-left origin). Pass view == nullptr to disable. Set
    // each frame before renderFrame(); the view is copied.
    void setInsetView(const CameraView* view, glm::vec4 rect) noexcept;

    // Render one entity shadow-only (kRenderFlagShadowOnly): it still casts a shadow but is not
    // drawn in the color pass. Used for the player's own aircraft in cockpit view, where the
    // camera sits at the entity origin and the mesh would otherwise fill the view, yet its
    // shadow on the ground should remain visible. Matches on both idx and gen; pass gen == 0 to
    // disable. Set each frame before renderFrame(). Does not affect particle/damage effects.
    void setHiddenEntity(uint32_t entityIdx, uint32_t entityGen) noexcept;

    // Optional: wire a logger to emit Trace-level diagnostics at pipeline boundaries.
    // Pass nullptr to disable (default). Logger must outlive SceneRenderer.
    void setLogger(ILogger* logger) noexcept;

    // Cockpit interior mesh (#870): the ASSET NAME of the ownship's cockpit interior, drawn locked to
    // the hidden (shadow-only) ownship's transform in Cockpit view. Set it each frame in Cockpit mode
    // (with the ownship set via setHiddenEntity); pass "" in every other mode to keep the HUD-only
    // cockpit. Consumes EntityDef::cockpitMesh (#813), which nothing rendered until now.
    void setCockpitMesh(const std::string& meshName);

    // ── Hot-reload invalidation (#152) ──────────────────────────────────────
    // Drop this mesh's GPU handles (mesh + its cascaded material/textures, via IRenderer::destroyMesh)
    // and cache entries — including a cached-invalid negative entry and every "<name>@@<livery>" livery
    // variant — so the next frame re-uploads from the (changed) bytes. Call after
    // AssetManager::processHotReload reports a Mesh change.
    void invalidateMesh(std::string_view meshAssetName);
    // A texture change re-uploads exactly the meshes that consumed it (textures are baked into a mesh's
    // material at createMesh time), tracked per mesh at upload. Call on a Texture change.
    void invalidateTexture(std::string_view textureAssetName);
    // Drop the livery cache + every livery-variant mesh/material, so re-skins re-resolve. Call on a
    // Livery change.
    void invalidateLiveries();
    // Drop every pack-derived mesh/material/livery/type-name cache entry (the console reload_content
    // full reload). Builtin placeholders + the floor are untouched.
    void invalidateAllAssets();

    // ── articulation debug scrub (#841) ─────────────────────────────────────
    // Force one channel of one entity to a value, overriding whatever the snapshot carries. This is
    // how the whole path — clip → sampler → pose arena → per-node draw — is demonstrable before the
    // simulation (#842) or the wire (#843) drive it, and it stays useful afterwards for isolating
    // "the clip is wrong" from "the sim is wrong". Driven by the `art` console command.
    void setArtChannelOverride(uint32_t entityIdx, ArtChannel channel, float value);
    void clearArtChannelOverrides() noexcept;
    // The rig a mesh asset resolved to (built lazily on first draw); nullptr if never uploaded. Test
    // and console seam — the console prints which channels a spawned entity's mesh actually models.
    [[nodiscard]] const ArticulationRig* articulationRigFor(const std::string& cacheKey) const;

  private:
    // The articulation rig for a mesh (#841), built lazily from the SAME bytes the mesh uploaded
    // from and cached under the same key, so hot-reload invalidates them together.
    const ArticulationRig* getOrBuildRig(const std::string& meshAssetName, const std::string& cacheKey);
    // Scrub every channel this entity's mesh models into the frame pose arena.
    void sampleEntityArticulation(const ArticulationRig& rig, const EntityRenderEntry& entry, float dt);

    // No-livery, no-variant form: caches under the mesh asset name, no texture overrides.
    MeshHandle getOrUploadMesh(const std::string& name);
    // Livery- and variant-aware form (#845/#882): loads bytes from `meshAssetName`, caches under
    // `cacheKey` (mesh name plus livery id and variant tag, so two liveries or two variants of one
    // mesh do not collide), applies `liveryOverrides` in the texture resolver, and uploads only the
    // node set `variant` selects. `cacheKey` also keys the material.
    MeshHandle getOrUploadMesh(const std::string& meshAssetName, const std::string& cacheKey,
                               const std::unordered_map<std::string, std::string>& liveryOverrides,
                               const std::string& variant);
    MaterialHandle getOrUploadMaterial(const std::string& cacheKey);

    // Resolve (and cache) the livery for an entity type index; nullptr = no livery.
    const LiveryTextureSet* resolveLivery(uint32_t typeIndex);

    // Upload builtin meshes and materials on first call; no-op thereafter.
    void ensureBuiltins();

    // Evict one mesh/material cacheKey: destroy the GPU handle (cascades material+textures) and erase
    // the mesh + material + texture-dep cache entries. Handles a cached-invalid (negative) entry.
    void evictMeshCacheKey(const std::string& cacheKey);

    SimRenderBridge& m_bridge;
    MeshNameResolver m_resolver;
    AssetManager& m_assets;
    IRenderer& m_renderer;

    ParticleSystem* m_particleSystem{nullptr};
    EffectResolver m_effectResolver;
    LiveryResolver m_liveryResolver;
    // Per-typeIndex resolved livery, cached so the resolver runs at most once per type. A cached entry
    // with empty id = "resolved, no livery" (distinct from "not yet resolved" = absent key).
    std::unordered_map<uint32_t, LiveryTextureSet> m_liveryCache;
    const std::unordered_map<std::string, std::string> m_noOverrides{}; // the empty-override sentinel
    SubtitleQueue* m_subtitleQueue{nullptr};
    std::vector<SubtitleEntry> m_subtitleEntries; // backing storage for FrameScene::subtitles span

    // Per-typeIndex resolution, cached so the resolver is called at most once per type.
    std::unordered_map<uint32_t, ResolvedMesh> m_typeNameCache;

    std::unordered_map<std::string, MeshHandle> m_meshCache;
    std::unordered_map<std::string, MaterialHandle> m_materialCache;
    // Per mesh cacheKey: the Texture asset names its material actually consumed at upload (#152), so a
    // texture change invalidates exactly the meshes that referenced it.
    std::unordered_map<std::string, std::vector<std::string>> m_meshTextureDeps;
    std::vector<RenderItem> m_items; // reused each frame; avoids per-frame allocation

    // ── articulation (#841) ─────────────────────────────────────────────────
    // Per mesh cacheKey, the rig parsed from the same .glb bytes the mesh uploaded from. Built lazily
    // beside the mesh, invalidated with it. A mesh with no clips caches an EMPTY rig, so the miss is
    // paid once and every later frame takes the zero-cost path.
    std::unordered_map<std::string, ArticulationRig> m_rigCache;
    // Frame pose arena. Poses are appended here and each item's span is patched in AFTER the entity
    // loop from a recorded (offset, count) — so a reallocation mid-loop cannot leave a live span
    // dangling, which reserving-and-hoping would only make unlikely rather than impossible.
    std::vector<NodePose> m_poseArena;
    std::vector<std::pair<std::size_t, std::size_t>> m_poseSpans; // (offset, count) per item index
    // Per-entity spin playhead (prop/rotor/wheel). Cosmetic and render-side: never simulated, never
    // wired — a propeller's angle is not world state.
    std::unordered_map<uint32_t, float> m_spinPhase;
    // Debug channel overrides, keyed by entity index (#841 `art` console command).
    std::unordered_map<uint32_t, std::array<float, kArtChannelCount>> m_artOverride;
    std::unordered_map<uint32_t, uint32_t> m_artOverrideMask; // bit n = channel n is overridden

    float m_drawDistanceSq{50000.0f * 50000.0f}; // squared cull distance in meters (default 50 km)

    // Nominal tick period used for velocity-based position extrapolation.
    static constexpr float kTickDt = 1.0f / 60.0f;

    // Builtin fallback resources — uploaded once on first renderFrame call.
    // Entity meshes: one placeholder silhouette per BuiltinShape (#886) + a wreck variant per
    // persistent category; palette cycles 6 colors by entityIdx (3 opaque, 3 glass).
    // Floor mesh: 4 km flat plane, olive-gray material.
    static constexpr int kPaletteSize = 6;
    static constexpr size_t kShapeCount = static_cast<size_t>(BuiltinShape::Count);

    MeshHandle m_builtinShapeMeshes[kShapeCount]{};        // indexed by BuiltinShape ordinal
    MeshHandle m_builtinDamagedShapeMeshes[kShapeCount]{}; // wreck variants; shown at damageLevel > 0
    TextureHandle m_builtinBaseColorTex{}; // builtin PBR texture set (#867), sampled by m_fallbackEntityMat
    TextureHandle m_builtinNormalTex{};
    TextureHandle m_builtinOrmTex{};
    MaterialHandle m_builtinPalette[kPaletteSize]{};
    MeshHandle m_builtinFloorMesh{};
    MaterialHandle m_builtinFloorMat{};
    // Shaded grey PBR fallback for meshes without an explicit material (and, in release builds,
    // the builtin placeholder entity). metallic 0.1, roughness 0.6.
    MaterialHandle m_fallbackEntityMat{};
    bool m_showBuiltinFloor{false};
    TerrainStreamer* m_terrainStreamer{nullptr};
    const AirportRegistry* m_airportRegistry{nullptr};
    std::unique_ptr<AirportRenderer> m_airportRenderer; // lazily created when a registry is set (#487)
    ILogger* m_logger{nullptr};

    // Entity to omit from rendering (player's own aircraft in cockpit view). gen == 0 = disabled.
    uint32_t m_hiddenEntityIdx{0};
    uint32_t m_hiddenEntityGen{0};

    // Ownship cockpit interior asset name (#870); empty = HUD-only cockpit. Set per frame in Cockpit.
    std::string m_cockpitMesh;

    // Wall-clock frame timing, used only to advance the looping spin channel (#841).
    std::chrono::steady_clock::time_point m_lastFrameTime{};

    // Secondary-camera inset (#698); set each frame via setInsetView(), written into FrameScene.
    bool m_insetEnabled{false};
    CameraView m_insetCamera{};
    glm::vec4 m_insetRect{0.0f};
};

} // namespace fl
