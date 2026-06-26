// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "RenderTypes.h"

#include <functional>
#include <span>
#include <string>
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
} // namespace fl

namespace fl {

// Converts the per-tick entity snapshot from SimRenderBridge into a FrameScene and submits
// it to IRenderer::setScene each frame.
//
// Threading: all methods must be called from the main (render) thread.
//
// Dependency injection:
//   MeshNameResolver breaks the circular CMake dep between engine-render and engine-entity.
//   Caller (main.cpp) provides a lambda that captures &EntityTypeRegistry and resolves
//   typeIndex → (meshName, classicDamageMeshName). Returns false if the typeIndex is unknown.
class SceneRenderer {
  public:
    // Given a typeIndex, fills meshName and classicDamageMeshName (empty if no damage variant).
    // Returns true if the type is known; false to skip the entity entirely.
    using MeshNameResolver =
        std::function<bool(uint32_t typeIndex, std::string& meshName, std::string& damageMeshName)>;

    // Given a typeIndex and damageLevel (uint8_t cast of DamageLevel), returns the visual
    // effect preset name, or empty string if none.  Used to emit particle effects from
    // damaged entities without introducing a CMake dep on engine-entity.
    using EffectResolver = std::function<std::string(uint32_t typeIndex, uint8_t damageLevel)>;

    SceneRenderer(SimRenderBridge& bridge, MeshNameResolver resolver, AssetManager& assets, IRenderer& renderer);
    ~SceneRenderer();

    // Optional: wire a ParticleSystem to emit per-entity damage effects each frame.
    // effectResolver is called for each entity with damageLevel > 0; the returned preset
    // name is forwarded to ParticleSystem::emit(). Pass nullptr/empty to disable.
    void setParticleSystem(ParticleSystem* ps, EffectResolver effectResolver) noexcept;

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

    // Render one entity shadow-only (kRenderFlagShadowOnly): it still casts a shadow but is not
    // drawn in the color pass. Used for the player's own aircraft in cockpit view, where the
    // camera sits at the entity origin and the mesh would otherwise fill the view, yet its
    // shadow on the ground should remain visible. Matches on both idx and gen; pass gen == 0 to
    // disable. Set each frame before renderFrame(). Does not affect particle/damage effects.
    void setHiddenEntity(uint32_t entityIdx, uint32_t entityGen) noexcept;

    // Optional: wire a logger to emit Trace-level diagnostics at pipeline boundaries.
    // Pass nullptr to disable (default). Logger must outlive SceneRenderer.
    void setLogger(ILogger* logger) noexcept;

  private:
    MeshHandle getOrUploadMesh(const std::string& name);
    MaterialHandle getOrUploadMaterial(const std::string& meshName);

    // Upload builtin meshes and materials on first call; no-op thereafter.
    void ensureBuiltins();

    SimRenderBridge& m_bridge;
    MeshNameResolver m_resolver;
    AssetManager& m_assets;
    IRenderer& m_renderer;

    ParticleSystem* m_particleSystem{nullptr};
    EffectResolver m_effectResolver;
    SubtitleQueue* m_subtitleQueue{nullptr};
    std::vector<SubtitleEntry> m_subtitleEntries; // backing storage for FrameScene::subtitles span

    // Per-typeIndex resolved names, cached so the resolver is called at most once per type.
    std::unordered_map<uint32_t, std::pair<std::string, std::string>> m_typeNameCache;

    std::unordered_map<std::string, MeshHandle> m_meshCache;
    std::unordered_map<std::string, MaterialHandle> m_materialCache;
    std::vector<RenderItem> m_items; // reused each frame; avoids per-frame allocation

    float m_drawDistanceSq{50000.0f * 50000.0f}; // squared cull distance in meters (default 50 km)

    // Nominal tick period used for velocity-based position extrapolation.
    static constexpr float kTickDt = 1.0f / 60.0f;

    // Builtin fallback resources — uploaded once on first renderFrame call.
    // Entity mesh: tetrahedron; palette cycles 6 colors by entityIdx (3 opaque, 3 glass).
    // Floor mesh: 4 km flat plane, olive-gray material.
    static constexpr int kPaletteSize = 6;

    MeshHandle m_builtinEntityMesh{};
    MaterialHandle m_builtinPalette[kPaletteSize]{};
    MeshHandle m_builtinFloorMesh{};
    MaterialHandle m_builtinFloorMat{};
    // Shaded grey PBR fallback for meshes without an explicit material (and, in release builds,
    // the builtin placeholder entity). metallic 0.1, roughness 0.6.
    MaterialHandle m_fallbackEntityMat{};
    bool m_showBuiltinFloor{false};
    TerrainStreamer* m_terrainStreamer{nullptr};
    ILogger* m_logger{nullptr};

    // Entity to omit from rendering (player's own aircraft in cockpit view). gen == 0 = disabled.
    uint32_t m_hiddenEntityIdx{0};
    uint32_t m_hiddenEntityGen{0};
};

} // namespace fl
