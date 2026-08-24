// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/PreviewScene.h"

#include "render/BuiltinGeometry.h"
#include "render/BuiltinTextures.h"
#include "render/CameraController.h"
#include "render/MeshTextureResolver.h"

#include "content/AssetManager.h"

#include "ILogger.h"
#include "IRenderer.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <glm/gtc/matrix_transform.hpp> // glm::translate
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace fl {

PreviewOrbit frameBounds(const glm::vec3& minB, const glm::vec3& maxB, float fovY, float margin) {
    PreviewOrbit o;
    if (minB.x > maxB.x || minB.y > maxB.y || minB.z > maxB.z) {
        o.distance = 5.0f; // degenerate/empty box — a sane stand-off
        return o;
    }
    o.focus = 0.5f * (minB + maxB);
    const float radius = 0.5f * glm::length(maxB - minB);
    const float sinHalf = std::sin(fovY * 0.5f);
    // distance so the bounding sphere fits the vertical FoV, with margin slack. Floor keeps a
    // near-zero model (a flat plane, a point) from collapsing the camera onto the focus.
    o.distance = radius > 0.001f && sinHalf > 0.001f ? margin * radius / sinHalf : 5.0f;
    return o;
}

CameraView previewCameraView(const PreviewOrbit& orbit, float aspect, float fovY, float near) {
    const float yaw = glm::radians(orbit.yawDeg);
    const float pitch = glm::radians(orbit.pitchDeg);
    // Spherical eye around the focus. yaw 0 places the eye on -X looking at the nose (+X), matching
    // the engine body convention (+X nose, +Y up, +Z starboard).
    const glm::vec3 dir{
        -std::cos(pitch) * std::cos(yaw),
        std::sin(pitch),
        -std::cos(pitch) * std::sin(yaw),
    };
    const glm::vec3 eye = orbit.focus + dir * orbit.distance;
    const glm::vec3 forward = orbit.focus - eye;
    return makeCameraView(glm::dvec3(eye), forward, glm::vec3{0.0f, 1.0f, 0.0f}, aspect, fovY, near);
}

PreviewScene::PreviewScene(IRenderer& renderer, AssetManager* assets, ILogger* logger)
    : m_renderer(renderer), m_assets(assets), m_logger(logger) {
    // Deterministic, golden-friendly lighting.
    m_env.sunDirection = glm::normalize(glm::vec3{0.6f, 1.0f, 0.4f});
    m_env.sunColor = glm::vec3{1.0f, 0.97f, 0.9f};
    m_env.ambientColor = glm::vec3{0.18f, 0.20f, 0.24f};
    m_env.fogDensity = 0.0f;
    m_env.cloudCoverage = 0.0f;
    m_env.timeOfDay = 12.0f;
    m_env.celestialValid = false;
}

PreviewScene::~PreviewScene() {
    destroyHandles();
    if (m_greyFallback.valid())
        m_renderer.destroyMaterial(m_greyFallback);
    if (m_fallbackBaseTex.valid())
        m_renderer.destroyTexture(m_fallbackBaseTex);
    if (m_fallbackNormalTex.valid())
        m_renderer.destroyTexture(m_fallbackNormalTex);
    if (m_fallbackOrmTex.valid())
        m_renderer.destroyTexture(m_fallbackOrmTex);
}

MaterialHandle PreviewScene::ensureFallbackMaterial() {
    if (m_greyFallback.valid())
        return m_greyFallback;
    // Mirror SceneRenderer::ensureBuiltins' m_fallbackEntityMat: the builtin procedural PBR maps
    // (#867) so the albedo/normal/ORM sampling path runs — a material with only scalar factors leaves
    // its texture slots on the renderer defaults, and a material-less mesh must preview as the game
    // draws it, not as flat-black.
    // One shared name for all three, as before -- the preview's textures are not distinguished in
    // captures the way the scene renderer's are.
    const BuiltinPbrMaps maps = uploadBuiltinPbrMaps(m_renderer, "builtin:preview-fallback-tex",
                                                     "builtin:preview-fallback-tex", "builtin:preview-fallback-tex");
    m_fallbackBaseTex = maps.baseColor;
    m_fallbackNormalTex = maps.normal;
    m_fallbackOrmTex = maps.orm;

    MaterialDesc gm{};
    gm.baseColorTexture = m_fallbackBaseTex;
    gm.normalTexture = m_fallbackNormalTex;
    gm.ormTexture = m_fallbackOrmTex;
    gm.baseColorFactor = glm::vec4{0.72f, 0.72f, 0.75f, 1.0f};
    gm.roughnessFactor = 0.6f;
    gm.metallicFactor = 0.1f;
    m_greyFallback = m_renderer.createMaterial(gm);
    return m_greyFallback;
}

void PreviewScene::destroyHandles() {
    if (m_mesh.valid())
        m_renderer.destroyMesh(m_mesh);
    if (m_damageMesh.valid())
        m_renderer.destroyMesh(m_damageMesh);
    m_mesh = {};
    m_damageMesh = {};
    m_material = {};
    m_damageMaterial = {};
}

MeshHandle PreviewScene::uploadMesh(const std::string& assetName, std::span<const uint8_t> bareBytes, glm::vec3& outMin,
                                    glm::vec3& outMax) {
    std::shared_ptr<MeshData> data; // keeps pack bytes alive across createMesh
    MeshUploadDesc desc{};
    desc.name = assetName;
    desc.contentForward = m_desc.contentForward;

    if (!bareBytes.empty()) {
        desc.bytes = bareBytes;
        // Bare-file texture resolver: read <glbDir>/<uri> from disk, rejecting any parent-escape.
        const std::filesystem::path dir = m_desc.glbDir;
        desc.textureResolver = [dir](std::string_view uri) -> std::vector<uint8_t> {
            std::filesystem::path rel(uri);
            if (rel.is_absolute())
                return {};
            for (const auto& seg : rel)
                if (seg == "..")
                    return {}; // no parent-directory escape
            std::ifstream f(dir / rel, std::ios::binary);
            if (!f)
                return {};
            return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        };
    } else {
        if (!m_assets)
            return {};
        data = m_assets->loadMesh(assetName.c_str());
        if (!data || data->bytes.empty())
            return {};
        desc.bytes = data->bytes;
        desc.textureResolver = makeMeshTextureResolver(*m_assets, m_desc.liveryOverrides);
    }

    MeshHandle h = m_renderer.createMesh(desc);
    if (h.valid())
        m_renderer.getMeshBounds(h, outMin, outMax);
    return h;
}

bool PreviewScene::load(ModelDesc desc) {
    destroyHandles();
    m_desc = std::move(desc);
    m_loadedContent = false;
    m_damaged = false;

    glm::vec3 mn(0.0f), mx(0.0f);
    m_mesh = uploadMesh(m_desc.meshAssetName, m_desc.glbBytes, mn, mx);

    if (m_mesh.valid()) {
        m_loadedContent = true;
        m_boundsMin = mn;
        m_boundsMax = mx;
        MaterialHandle mat = m_renderer.getMeshMaterial(m_mesh);
        if (!mat.valid())
            mat = ensureFallbackMaterial();
        m_material = mat;

        // Damage variant (pack mode only; a bare .glb has no separate damage file).
        if (m_assets && !m_desc.damageMeshAssetName.empty()) {
            glm::vec3 dmn(0.0f), dmx(0.0f);
            m_damageMesh = uploadMesh(m_desc.damageMeshAssetName, {}, dmn, dmx);
            if (m_damageMesh.valid()) {
                MaterialHandle dm = m_renderer.getMeshMaterial(m_damageMesh);
                m_damageMaterial = dm.valid() ? dm : m_material;
            }
        }
        return true;
    }

    // Nothing loaded — draw the builtin placeholder so the scene is still framed and honest.
    if (m_logger) {
        char buf[192];
        std::snprintf(buf, sizeof(buf), "preview: no content mesh for '%s' — drawing builtin placeholder",
                      m_desc.meshAssetName.empty() ? "<bare .glb>" : m_desc.meshAssetName.c_str());
        m_logger->log(LogLevel::Warn, __FILE__, __LINE__, buf);
    }
    MeshUploadDesc bd{};
    bd.name = "builtin:preview-fallback";
    bd.bytes = builtinShapeGlb(m_desc.fallbackShape);
    bd.contentForward = false; // builtin geometry is already in the body frame
    m_mesh = m_renderer.createMesh(bd);
    if (m_mesh.valid())
        m_renderer.getMeshBounds(m_mesh, m_boundsMin, m_boundsMax);
    m_material = ensureFallbackMaterial();
    return false;
}

bool PreviewScene::reload() {
    return load(m_desc);
}

void PreviewScene::setDamaged(bool on) noexcept {
    m_damaged = on;
}

void PreviewScene::setDebugView(PreviewDebugView view) noexcept {
    m_view = view;
}

bool PreviewScene::hasDamageMesh() const noexcept {
    return m_damageMesh.valid();
}

bool PreviewScene::loadedFromContent() const noexcept {
    return m_loadedContent;
}

uint32_t PreviewScene::debugFlags() const noexcept {
    switch (m_view) {
    case PreviewDebugView::FaceColor:
        return kRenderFlagDebugFaceColor;
    case PreviewDebugView::Wireframe:
        return kRenderFlagWireframe; // renderer ignores it when the device lacks fillModeNonSolid
    case PreviewDebugView::Normals:
        return kRenderFlagDebugNormals;
    case PreviewDebugView::Shaded:
    default:
        return 0;
    }
}

std::span<const RenderItem> PreviewScene::buildItems(const glm::dvec3& cameraWorldOrigin) {
    m_items.clear();
    const bool useDamage = m_damaged && m_damageMesh.valid();
    RenderItem it{};
    it.mesh = useDamage ? m_damageMesh : m_mesh;
    it.material = useDamage ? m_damageMaterial : m_material;
    // Model at the world origin, rebased camera-relative on the CPU (the rebase-once invariant).
    it.transform = glm::translate(glm::mat4(1.0f), glm::vec3(-cameraWorldOrigin));
    it.flags = debugFlags();
    if (useDamage)
        it.flags |= kRenderFlagDamaged;
    if (it.mesh.valid())
        m_items.push_back(it);
    return {m_items.data(), m_items.size()};
}

} // namespace fl
