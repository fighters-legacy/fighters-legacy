// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/SceneRenderer.h"
#include "render/BuiltinGeometry.h"
#include "render/ParticleSystem.h"
#include "render/RenderSnapshot.h"
#include "render/SimRenderBridge.h"
#include "render/TerrainStreamer.h"

#include "audio/SubtitleQueue.h"
#include "content/AssetManager.h"

#include "ILogger.h"
#include "IRenderer.h"

#include <glm/gtc/matrix_transform.hpp> // glm::translate
#include <glm/gtc/quaternion.hpp>       // glm::mat4_cast

#include <algorithm> // std::sort
#include <cstdio>    // std::snprintf

namespace fl {

SceneRenderer::SceneRenderer(SimRenderBridge& bridge, MeshNameResolver resolver, AssetManager& assets,
                             IRenderer& renderer)
    : m_bridge(bridge), m_resolver(std::move(resolver)), m_assets(assets), m_renderer(renderer) {}

SceneRenderer::~SceneRenderer() = default;

void SceneRenderer::setDrawDistance(float distanceKm) noexcept {
    const float meters = distanceKm * 1000.0f;
    m_drawDistanceSq = meters * meters;
}

void SceneRenderer::setBuiltinFloor(bool show) noexcept {
    m_showBuiltinFloor = show;
}

void SceneRenderer::setTerrainStreamer(TerrainStreamer* ts) noexcept {
    m_terrainStreamer = ts;
}

void SceneRenderer::setLogger(ILogger* logger) noexcept {
    m_logger = logger;
}

void SceneRenderer::setHiddenEntity(uint32_t entityIdx, uint32_t entityGen) noexcept {
    m_hiddenEntityIdx = entityIdx;
    m_hiddenEntityGen = entityGen;
}

void SceneRenderer::ensureBuiltins() {
    if (m_builtinEntityMesh.valid())
        return;

    m_builtinEntityMesh = m_renderer.createMesh({"builtin:entity", builtinTetrahedronGlb()});
    m_builtinFloorMesh = m_renderer.createMesh({"builtin:floor", builtinFloorPlaneGlb()});

    // 6-color opaque palette: gives each entity a distinct look in the no-content sandbox.
    // All entries are opaque so entities are always clearly visible regardless of camera angle
    // or background. Cycled by entry.entityIdx % kPaletteSize.
    struct PaletteEntry {
        float r, g, b, a;
        bool alphaBlend;
    };
    static constexpr PaletteEntry kPalette[kPaletteSize] = {
        {1.00f, 0.25f, 0.15f, 1.00f, false}, // red
        {0.20f, 0.75f, 0.30f, 1.00f, false}, // green
        {0.15f, 0.45f, 1.00f, 1.00f, false}, // blue
        {0.90f, 0.80f, 0.10f, 1.00f, false}, // yellow
        {0.65f, 0.10f, 0.90f, 1.00f, false}, // purple
        {0.10f, 0.85f, 0.90f, 1.00f, false}, // cyan
    };
    for (int i = 0; i < kPaletteSize; ++i) {
        MaterialDesc md{};
        md.baseColorFactor = {kPalette[i].r, kPalette[i].g, kPalette[i].b, kPalette[i].a};
        md.roughnessFactor = 0.6f;
        md.alphaBlend = kPalette[i].alphaBlend;
        m_builtinPalette[i] = m_renderer.createMaterial(md);
    }

    MaterialDesc fmd{};
    fmd.baseColorFactor = {0.35f, 0.45f, 0.30f, 1.0f}; // olive-gray
    fmd.roughnessFactor = 0.95f;
    m_builtinFloorMat = m_renderer.createMaterial(fmd);

    // Shaded grey fallback: used for resolved meshes lacking explicit material data, and (in
    // release builds) for the builtin placeholder entity in place of the per-face debug colour.
    MaterialDesc emd{};
    emd.baseColorFactor = {0.60f, 0.60f, 0.62f, 1.0f}; // neutral grey
    emd.metallicFactor = 0.10f;
    emd.roughnessFactor = 0.60f;
    m_fallbackEntityMat = m_renderer.createMaterial(emd);
}

void SceneRenderer::setParticleSystem(ParticleSystem* ps, EffectResolver effectResolver) noexcept {
    m_particleSystem = ps;
    m_effectResolver = std::move(effectResolver);
}

void SceneRenderer::setSubtitleQueue(SubtitleQueue* queue) noexcept {
    m_subtitleQueue = queue;
}

void SceneRenderer::renderFrame(float alpha, const CameraView& camera, const EnvironmentState& env,
                                std::span<const ParticleEmitterState> extraEmitters) {
    ensureBuiltins();
    m_bridge.tryAdvance();
    m_items.clear();

    if (m_particleSystem)
        m_particleSystem->reset();

    // Build subtitle span from queue (data only; VkRenderer ignores until Phase 4 IGui).
    m_subtitleEntries.clear();
    if (m_subtitleQueue) {
        for (const auto& r : m_subtitleQueue->records())
            m_subtitleEntries.push_back({r.text, 1.0f});
    }

    if (!m_bridge.hasSnapshot()) {
        FrameScene scene{};
        scene.camera = camera;
        scene.environment = env;
        scene.particleEmitters = extraEmitters;
        scene.subtitles = m_subtitleEntries;
        m_renderer.setScene(scene);
        return;
    }

    const RenderSnapshot& snap = m_bridge.current();
    m_items.reserve(snap.entries.size());

    for (const auto& entry : snap.entries) {
        // The hidden entity (player's own aircraft in cockpit view) is rendered shadow-only:
        // the camera sits at its origin so the mesh would fill the view, but it should still
        // cast a shadow on the ground. gen == 0 disables the filter.
        const bool shadowOnly =
            m_hiddenEntityGen != 0 && entry.entityIdx == m_hiddenEntityIdx && entry.entityGen == m_hiddenEntityGen;

        // Resolve typeIndex → mesh names (cached after first call per type).
        auto nameIt = m_typeNameCache.find(entry.typeIndex);
        if (nameIt == m_typeNameCache.end()) {
            std::string mesh, dmg;
            bool resolved = m_resolver(entry.typeIndex, mesh, dmg);
            nameIt = m_typeNameCache.emplace(entry.typeIndex, std::make_pair(mesh, dmg)).first;
            if (!resolved) {
                // Mark as unresolved by leaving names empty; fallback handled below.
                nameIt->second = {"", ""};
            }
        }
        const auto& [meshName, damageMeshName] = nameIt->second;

        // Pick damage variant when entity is damaged and a variant mesh exists.
        const std::string& activeMesh = (entry.damageLevel > 0 && !damageMeshName.empty()) ? damageMeshName : meshName;

        MeshHandle mesh{};
        MaterialHandle mat{};
        bool useBuiltin = activeMesh.empty();

        if (!useBuiltin) {
            mesh = getOrUploadMesh(activeMesh);
            if (mesh.valid())
                mat = getOrUploadMaterial(activeMesh);
            else
                useBuiltin = true;
        }

        if (useBuiltin) {
            if (!m_builtinEntityMesh.valid())
                continue; // builtins not yet uploaded — skip
            mesh = m_builtinEntityMesh;
#ifdef NDEBUG
            // Release: shaded grey so placeholder entities read as real geometry.
            mat = m_fallbackEntityMat;
#else
            // Debug: per-entity palette colour + per-face debug tint (orientation aid).
            mat = m_builtinPalette[entry.entityIdx % static_cast<uint32_t>(kPaletteSize)];
#endif
        }

        // Velocity extrapolation: advance position by alpha × tick period.
        glm::dvec3 worldPos = entry.position + glm::dvec3(entry.velocity * (alpha * kTickDt));

        // Camera-relative position: subtract two dvec3 values, then narrow to vec3 (float32-safe).
        glm::vec3 relPos = glm::vec3(worldPos - camera.worldOrigin);

        // Distance cull — skip entities beyond the configured draw distance.
        float distSq = relPos.x * relPos.x + relPos.y * relPos.y + relPos.z * relPos.z;
        if (distSq > m_drawDistanceSq)
            continue;

        // TRS model matrix (no scale — entities are unit-scale in world space).
        glm::mat4 model = glm::translate(glm::mat4(1.0f), relPos) * glm::mat4_cast(entry.orientation);

        RenderItem item{};
        item.mesh = mesh;
        item.material = mat;
        item.transform = model;
        item.lod = 0;
        item.flags = (entry.damageLevel > 0) ? kRenderFlagDamaged : 0u;
        if (shadowOnly)
            item.flags |= kRenderFlagShadowOnly;
#ifndef NDEBUG
        if (useBuiltin)
            item.flags |= kRenderFlagDebugFaceColor; // distinct per-face colours on the placeholder
#endif
        m_items.push_back(item);
    }

    // Emit per-entity damage particle effects (uses snapshot positions — thread-safe).
    if (m_particleSystem && m_effectResolver) {
        for (const auto& entry : snap.entries) {
            if (entry.damageLevel == 0)
                continue;
            std::string effect = m_effectResolver(entry.typeIndex, entry.damageLevel);
            if (!effect.empty())
                m_particleSystem->emit(effect.c_str(), glm::vec3(entry.position));
        }
    }

    // Sort front-to-back by squared camera-relative distance to minimise overdraw.
    std::sort(m_items.begin(), m_items.end(), [](const RenderItem& a, const RenderItem& b) {
        // transform[3] is the translation column (glm is column-major).
        const glm::vec4& ta = a.transform[3];
        const glm::vec4& tb = b.transform[3];
        float da = ta.x * ta.x + ta.y * ta.y + ta.z * ta.z;
        float db = tb.x * tb.x + tb.y * tb.y + tb.z * tb.z;
        return da < db;
    });

    // Terrain chunks — appended after entity sort, before the fallback floor plane.
    if (m_terrainStreamer) {
        auto terrainItems = m_terrainStreamer->getRenderItems(camera.worldOrigin);
        m_items.insert(m_items.end(), terrainItems.begin(), terrainItems.end());
    }

    // Builtin floor plane — appended after sort so it sits at the back of the opaque list.
    // Camera-relative rebase: floor is at world origin, so relPos = -camera.worldOrigin.
    if (m_showBuiltinFloor && m_builtinFloorMesh.valid()) {
        RenderItem floor{};
        floor.mesh = m_builtinFloorMesh;
        floor.material = m_builtinFloorMat;
        floor.transform = glm::translate(glm::mat4(1.0f), -glm::vec3(camera.worldOrigin));
        m_items.push_back(floor);
    }

    // Merge entity damage effects (if any) with caller-supplied extra emitters.
    std::span<const ParticleEmitterState> emitters = extraEmitters;
    if (m_particleSystem && !m_particleSystem->emitters().empty())
        emitters = m_particleSystem->emitters();

    if (m_logger) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "renderFrame: %zu items submitted (snapshot entries: %zu)", m_items.size(),
                      snap.entries.size());
        m_logger->log(LogLevel::Trace, __FILE__, __LINE__, buf);
    }

    FrameScene scene{};
    scene.camera = camera;
    scene.renderItems = m_items;
    scene.environment = env;
    scene.particleEmitters = emitters;
    scene.subtitles = m_subtitleEntries;
    m_renderer.setScene(scene);
}

MeshHandle SceneRenderer::getOrUploadMesh(const std::string& name) {
    auto it = m_meshCache.find(name);
    if (it != m_meshCache.end())
        return it->second;

    auto data = m_assets.loadMesh(name.c_str());
    if (!data || data->bytes.empty()) {
        m_meshCache[name] = MeshHandle{};
        return MeshHandle{};
    }

    MeshUploadDesc desc{name, data->bytes};
    MeshHandle h = m_renderer.createMesh(desc);
    m_meshCache[name] = h;
    return h;
}

MaterialHandle SceneRenderer::getOrUploadMaterial(const std::string& meshName) {
    auto it = m_materialCache.find(meshName);
    if (it != m_materialCache.end())
        return it->second;

    // Share the pre-created shaded-grey fallback across every mesh name that lacks explicit
    // material data (ensureBuiltins() always runs before this method). Avoids creating one
    // identical material per mesh name.
    m_materialCache[meshName] = m_fallbackEntityMat;
    return m_fallbackEntityMat;
}

} // namespace fl
