// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "RenderTypes.h"
#include "render/BuiltinShape.h"

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
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
} // namespace fl

namespace fl {

// Map a glTF image URI to a Texture asset name (#833). The pack convention places textures under the
// `textures/` asset directory, so a mesh references "../../textures/<name>.ktx2" (a bare
// "<name>.ktx2" is also accepted); both yield asset name "<name>", which AssetManager::loadTexture
// resolves back to textures/<name>.ktx2 (.png fallback). Takes the path after the last "textures/"
// segment (or the bare basename) and strips the extension. Exposed for unit testing the convention.
std::string textureAssetNameFromUri(std::string_view uri);

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

  private:
    // No-livery form: caches under the mesh asset name, no texture overrides.
    MeshHandle getOrUploadMesh(const std::string& name);
    // Livery-aware form (#845): loads bytes from `meshAssetName`, caches under `cacheKey` (mesh name
    // plus livery id so two liveries of one mesh do not collide), and applies `liveryOverrides` in the
    // texture resolver. `cacheKey` also keys the material.
    MeshHandle getOrUploadMesh(const std::string& meshAssetName, const std::string& cacheKey,
                               const std::unordered_map<std::string, std::string>& liveryOverrides);
    MaterialHandle getOrUploadMaterial(const std::string& cacheKey);

    // Resolve (and cache) the livery for an entity type index; nullptr = no livery.
    const LiveryTextureSet* resolveLivery(uint32_t typeIndex);

    // Upload builtin meshes and materials on first call; no-op thereafter.
    void ensureBuiltins();

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
    std::vector<RenderItem> m_items; // reused each frame; avoids per-frame allocation

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

    // Secondary-camera inset (#698); set each frame via setInsetView(), written into FrameScene.
    bool m_insetEnabled{false};
    CameraView m_insetCamera{};
    glm::vec4 m_insetRect{0.0f};
};

} // namespace fl
