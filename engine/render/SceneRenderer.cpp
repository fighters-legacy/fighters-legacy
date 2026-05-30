// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/SceneRenderer.h"
#include "render/BuiltinGeometry.h"
#include "render/ParticleSystem.h"
#include "render/RenderSnapshot.h"
#include "render/SimRenderBridge.h"

#include "content/AssetManager.h"

#include "IRenderer.h"

#include <glm/gtc/matrix_transform.hpp> // glm::translate
#include <glm/gtc/quaternion.hpp>       // glm::mat4_cast

#include <algorithm> // std::sort

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

void SceneRenderer::ensureBuiltins() {
    if (m_builtinEntityMesh.valid())
        return;

    m_builtinEntityMesh = m_renderer.createMesh({"builtin:entity", builtinTetrahedronGlb()});
    m_builtinFloorMesh = m_renderer.createMesh({"builtin:floor", builtinFloorPlaneGlb()});

    // 6-color palette: first 3 opaque (forward pass), last 3 semi-transparent (transparent pass).
    // Cycled by entry.entityIdx % kPaletteSize — gives each entity a distinct look and exercises
    // both the opaque and transparent render passes simultaneously in the sandbox.
    struct PaletteEntry {
        float r, g, b, a;
        bool alphaBlend;
    };
    static constexpr PaletteEntry kPalette[kPaletteSize] = {
        {1.00f, 0.25f, 0.15f, 1.00f, false}, // red,    opaque
        {0.20f, 0.75f, 0.30f, 1.00f, false}, // green,  opaque
        {0.15f, 0.45f, 1.00f, 1.00f, false}, // blue,   opaque
        {0.90f, 0.80f, 0.10f, 0.50f, true},  // yellow, glass
        {0.65f, 0.10f, 0.90f, 0.45f, true},  // purple, glass
        {0.10f, 0.85f, 0.90f, 0.50f, true},  // cyan,   glass
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
}

void SceneRenderer::setParticleSystem(ParticleSystem* ps, EffectResolver effectResolver) noexcept {
    m_particleSystem = ps;
    m_effectResolver = std::move(effectResolver);
}

void SceneRenderer::renderFrame(float alpha, const CameraView& camera, const EnvironmentState& env,
                                std::span<const ParticleEmitterState> extraEmitters) {
    ensureBuiltins();
    m_bridge.tryAdvance();
    m_items.clear();

    if (m_particleSystem)
        m_particleSystem->reset();

    if (!m_bridge.hasSnapshot()) {
        FrameScene scene{};
        scene.camera = camera;
        scene.environment = env;
        scene.particleEmitters = extraEmitters;
        m_renderer.setScene(scene);
        return;
    }

    const RenderSnapshot& snap = m_bridge.current();
    m_items.reserve(snap.entries.size());

    for (const auto& entry : snap.entries) {
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
            mat = m_builtinPalette[entry.entityIdx % static_cast<uint32_t>(kPaletteSize)];
        }

        // Velocity extrapolation: advance position by alpha × tick period.
        glm::vec3 worldPos = entry.position + entry.velocity * (alpha * kTickDt);

        // Camera-relative position (float32-safe at arbitrary theater scale).
        glm::vec3 relPos = worldPos - camera.worldOrigin;

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
        m_items.push_back(item);
    }

    // Emit per-entity damage particle effects (uses snapshot positions — thread-safe).
    if (m_particleSystem && m_effectResolver) {
        for (const auto& entry : snap.entries) {
            if (entry.damageLevel == 0)
                continue;
            std::string effect = m_effectResolver(entry.typeIndex, entry.damageLevel);
            if (!effect.empty())
                m_particleSystem->emit(effect.c_str(), entry.position);
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

    // Builtin floor plane — appended after sort so it sits at the back of the opaque list.
    // Camera-relative rebase: floor is at world origin, so relPos = -camera.worldOrigin.
    if (m_showBuiltinFloor && m_builtinFloorMesh.valid()) {
        RenderItem floor{};
        floor.mesh = m_builtinFloorMesh;
        floor.material = m_builtinFloorMat;
        floor.transform = glm::translate(glm::mat4(1.0f), -camera.worldOrigin);
        m_items.push_back(floor);
    }

    // Merge entity damage effects (if any) with caller-supplied extra emitters.
    std::span<const ParticleEmitterState> emitters = extraEmitters;
    if (m_particleSystem && !m_particleSystem->emitters().empty())
        emitters = m_particleSystem->emitters();

    FrameScene scene{};
    scene.camera = camera;
    scene.renderItems = m_items;
    scene.environment = env;
    scene.particleEmitters = emitters;
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

    // Default opaque PBR material — no textures, white base color.
    MaterialDesc desc{};
    MaterialHandle h = m_renderer.createMaterial(desc);
    m_materialCache[meshName] = h;
    return h;
}

} // namespace fl
