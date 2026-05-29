// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/SceneRenderer.h"
#include "render/RenderSnapshot.h"
#include "render/SimRenderBridge.h"

#include "content/AssetManager.h"
#include "content/AssetTypes.h"

#include "IRenderer.h"

#include <glm/gtc/matrix_transform.hpp> // glm::translate
#include <glm/gtc/quaternion.hpp>       // glm::mat4_cast

#include <algorithm> // std::sort

namespace fl {

SceneRenderer::SceneRenderer(SimRenderBridge& bridge, MeshNameResolver resolver, AssetManager& assets,
                             IRenderer& renderer)
    : m_bridge(bridge), m_resolver(std::move(resolver)), m_assets(assets), m_renderer(renderer) {}

SceneRenderer::~SceneRenderer() = default;

void SceneRenderer::renderFrame(float alpha, const CameraView& camera, const EnvironmentState& env) {
    m_bridge.tryAdvance();
    m_items.clear();

    if (!m_bridge.hasSnapshot()) {
        FrameScene scene{};
        scene.camera = camera;
        scene.environment = env;
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
            if (!m_resolver(entry.typeIndex, mesh, dmg))
                continue; // unknown type — skip
            nameIt = m_typeNameCache.emplace(entry.typeIndex, std::make_pair(mesh, dmg)).first;
        }
        const auto& [meshName, damageMeshName] = nameIt->second;
        if (meshName.empty())
            continue;

        // Pick damage variant when entity is damaged and a variant mesh exists.
        const std::string& activeMesh = (entry.damageLevel > 0 && !damageMeshName.empty()) ? damageMeshName : meshName;

        MeshHandle mesh = getOrUploadMesh(activeMesh);
        if (!mesh.valid())
            continue;

        MaterialHandle mat = getOrUploadMaterial(activeMesh);

        // Velocity extrapolation: advance position by alpha × tick period.
        glm::vec3 worldPos = entry.position + entry.velocity * (alpha * kTickDt);

        // Camera-relative position (float32-safe at arbitrary theater scale).
        glm::vec3 relPos = worldPos - camera.worldOrigin;

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

    // Sort front-to-back by squared camera-relative distance to minimise overdraw.
    std::sort(m_items.begin(), m_items.end(), [](const RenderItem& a, const RenderItem& b) {
        // transform[3] is the translation column (glm is column-major).
        const glm::vec4& ta = a.transform[3];
        const glm::vec4& tb = b.transform[3];
        float da = ta.x * ta.x + ta.y * ta.y + ta.z * ta.z;
        float db = tb.x * tb.x + tb.y * tb.y + tb.z * tb.z;
        return da < db;
    });

    FrameScene scene{};
    scene.camera = camera;
    scene.renderItems = m_items;
    scene.environment = env;
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
