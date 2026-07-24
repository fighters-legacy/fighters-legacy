// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/SceneRenderer.h"
#include "render/AirportRenderer.h"
#include "render/BuiltinGeometry.h"
#include "render/BuiltinTextures.h"
#include "render/MeshTextureResolver.h"
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

void SceneRenderer::setAirportRegistry(const AirportRegistry* reg) {
    m_airportRegistry = reg;
    if (reg && !m_airportRenderer)
        m_airportRenderer = std::make_unique<AirportRenderer>(m_renderer);
    if (m_airportRenderer)
        m_airportRenderer->setRegistry(reg);
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

void SceneRenderer::setInsetView(const CameraView* view, glm::vec4 rect) noexcept {
    m_insetEnabled = (view != nullptr);
    if (view)
        m_insetCamera = *view;
    m_insetRect = rect;
}

void SceneRenderer::setCockpitMesh(const std::string& meshName) {
    m_cockpitMesh = meshName;
}

void SceneRenderer::ensureBuiltins() {
    if (m_builtinShapeMeshes[0].valid())
        return;

    // One placeholder silhouette per BuiltinShape (#886), plus the slumped wreck variants. A
    // shape with no wreck of its own shares the intact mesh handle (builtinDamagedShapeGlb
    // returns the intact span, and the name-keyed createMesh call is idempotent per name).
    static constexpr const char* kShapeNames[kShapeCount] = {
        "builtin:shape-unknown",      "builtin:shape-aircraft",  "builtin:shape-missile",
        "builtin:shape-bomb",         "builtin:shape-rocket",    "builtin:shape-ground-vehicle",
        "builtin:shape-naval-vessel", "builtin:shape-structure", "builtin:shape-parachute",
    };
    for (size_t i = 0; i < kShapeCount; ++i) {
        const auto shape = static_cast<BuiltinShape>(i);
        m_builtinShapeMeshes[i] = m_renderer.createMesh({kShapeNames[i], builtinShapeGlb(shape)});
        const auto damaged = builtinDamagedShapeGlb(shape);
        if (damaged.data() == builtinShapeGlb(shape).data()) {
            m_builtinDamagedShapeMeshes[i] = m_builtinShapeMeshes[i]; // no wreck — reuse intact
        } else {
            const std::string dmgName = std::string(kShapeNames[i]) + "-damaged";
            m_builtinDamagedShapeMeshes[i] = m_renderer.createMesh({dmgName, damaged});
        }
    }
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

    // Builtin procedural PBR textures (#867): base color / normal / ORM, uploaded raw-RGBA so the
    // albedo/normal/ORM SAMPLING path runs zero-pack (the builtin material otherwise used only PBR
    // scalar factors). A helper uploads one BuiltinRgbaTexture via the raw-RGBA fallback.
    auto uploadRaw = [this](const char* name, const BuiltinRgbaTexture& tex, bool srgb) -> TextureHandle {
        TextureUploadDesc td{};
        td.name = name;
        td.bytes = tex.pixels;
        td.srgb = srgb;
        td.rawWidth = static_cast<uint32_t>(tex.width);
        td.rawHeight = static_cast<uint32_t>(tex.height);
        return m_renderer.createTexture(td);
    };
    m_builtinBaseColorTex = uploadRaw("builtin:base-color", builtinBaseColorTexture(), /*srgb=*/true);
    m_builtinNormalTex = uploadRaw("builtin:normal", builtinNormalTexture(), /*srgb=*/false);
    m_builtinOrmTex = uploadRaw("builtin:orm", builtinOrmTexture(), /*srgb=*/false);

    // Shaded fallback: used for resolved meshes lacking explicit material data, and (in release
    // builds) for the builtin placeholder entity. Now TEXTURED, so a builtin entity samples the
    // albedo/normal/ORM maps rather than a flat factor (#867).
    MaterialDesc emd{};
    emd.baseColorTexture = m_builtinBaseColorTex;
    emd.normalTexture = m_builtinNormalTex;
    emd.ormTexture = m_builtinOrmTex;
    emd.baseColorFactor = {0.60f, 0.60f, 0.62f, 1.0f}; // tints the base-color map
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

    // Planet centre for the terrain shader's radial "up" (#475): {0,-R,0} from the streamer's baked
    // radius (Earth default when there is no terrain streamer). Terrain shading is the only consumer.
    const double planetR = m_terrainStreamer ? m_terrainStreamer->planetRadiusM() : 6371000.0;
    const glm::dvec3 planetCenterWorld{0.0, -planetR, 0.0};

    if (!m_bridge.hasSnapshot()) {
        FrameScene scene{};
        scene.camera = camera;
        scene.camera.planetCenter = planetCenterWorld;
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

        // Resolve typeIndex → mesh names + placeholder shape (cached after first call per type).
        auto nameIt = m_typeNameCache.find(entry.typeIndex);
        if (nameIt == m_typeNameCache.end()) {
            ResolvedMesh resolvedMesh;
            const bool resolved = m_resolver ? m_resolver(entry.typeIndex, resolvedMesh) : false;
            if (!resolved)
                resolvedMesh = ResolvedMesh{}; // unknown type: empty names + the Unknown error beacon
            nameIt = m_typeNameCache.emplace(entry.typeIndex, std::move(resolvedMesh)).first;
            // Explain the placeholder ONCE per type (#832). A mesh-less type renders as a builtin
            // placeholder shape, which is otherwise indistinguishable from a broken or missing
            // mesh — the silence cost a multi-hour debugging session on the F-5E.
            if (m_logger && nameIt->second.meshName.empty()) {
                char buf[176];
                std::snprintf(buf, sizeof(buf), "entity type index %u has no mesh (%s) — drawing placeholder",
                              entry.typeIndex, resolved ? "the type declares none" : "type not found in registry");
                m_logger->log(LogLevel::Warn, __FILE__, __LINE__, buf);
            }
        }
        const ResolvedMesh& resolved = nameIt->second;

        // Pick damage variant when entity is damaged and a variant mesh exists.
        const std::string& activeMesh =
            (entry.damageLevel > 0 && !resolved.damageMeshName.empty()) ? resolved.damageMeshName : resolved.meshName;

        MeshHandle mesh{};
        MaterialHandle mat{};
        bool useBuiltin = activeMesh.empty();

        if (!useBuiltin) {
            // Livery (#845): swaps textures per material slot, never geometry. The cache key folds in
            // the livery id so two liveries of the same mesh get distinct GPU materials.
            const LiveryTextureSet* livery = resolveLivery(entry.typeIndex);
            // Cache key folds in BOTH the livery id and the variant tag (#882): two liveries, or a
            // single-seat and a two-seat variant of one family mesh, are distinct GPU meshes.
            std::string cacheKey = activeMesh;
            if (!resolved.variant.empty())
                cacheKey += "#" + resolved.variant;
            if (livery)
                cacheKey += "@@" + livery->id;
            mesh = getOrUploadMesh(activeMesh, cacheKey, livery ? livery->overrides : m_noOverrides, resolved.variant);
            if (mesh.valid())
                mat = getOrUploadMaterial(cacheKey);
            else
                useBuiltin = true; // failed pack mesh: fall back to the type's category shape (#832 warned)
        }

        if (useBuiltin) {
            if (!m_builtinShapeMeshes[0].valid())
                continue; // builtins not yet uploaded — skip
            // Per-category placeholder silhouette (#886); the ordinal is defensive-clamped to
            // Unknown. A damaged builtin swaps to the shape's wreck variant (#864) so the
            // mesh-swap path runs zero-pack, exactly as a pack entity swaps to its
            // classicDamageMesh (shapes without a wreck keep their intact mesh).
            size_t shapeIdx = static_cast<size_t>(resolved.shape);
            if (shapeIdx >= kShapeCount)
                shapeIdx = static_cast<size_t>(BuiltinShape::Unknown);
            mesh = (entry.damageLevel > 0 && m_builtinDamagedShapeMeshes[shapeIdx].valid())
                       ? m_builtinDamagedShapeMeshes[shapeIdx]
                       : m_builtinShapeMeshes[shapeIdx];
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

        // Cockpit interior (#870): in Cockpit view the ownship is the hidden (shadow-only) entity and
        // the camera sits at its origin. If a cockpit mesh is set, draw it at the ownship's transform
        // (camera-relative, so it surrounds the camera and turns with the airframe when the pilot
        // looks around) as a normal opaque, depth-composited item — NOT shadow-only. Empty cockpit
        // mesh keeps the HUD-only cockpit (today's behavior).
        if (shadowOnly && !m_cockpitMesh.empty()) {
            const MeshHandle cockpit = getOrUploadMesh(m_cockpitMesh);
            if (cockpit.valid()) {
                RenderItem ci{};
                ci.mesh = cockpit;
                ci.material = getOrUploadMaterial(m_cockpitMesh);
                if (!ci.material.valid())
                    ci.material = m_fallbackEntityMat;
                ci.transform = model; // locked to the airframe, at the camera origin
                ci.lod = 0;
                m_items.push_back(ci);
            }
        }
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

    // Runway slabs (#487) — appended beside the terrain items, drawn a few cm above the flattened pad.
    if (m_airportRenderer)
        m_airportRenderer->appendRenderItems(camera.worldOrigin, m_items);

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
    scene.camera.planetCenter = planetCenterWorld;
    scene.renderItems = m_items;
    scene.environment = env;
    scene.particleEmitters = emitters;
    scene.subtitles = m_subtitleEntries;
    if (m_insetEnabled) {
        scene.insetEnabled = true;
        scene.insetCamera = m_insetCamera;
        scene.insetCamera.planetCenter = planetCenterWorld; // same planet frame as the main camera
        scene.insetRect[0] = m_insetRect.x;
        scene.insetRect[1] = m_insetRect.y;
        scene.insetRect[2] = m_insetRect.z;
        scene.insetRect[3] = m_insetRect.w;
    }
    m_renderer.setScene(scene);
}

MeshHandle SceneRenderer::getOrUploadMesh(const std::string& name) {
    static const std::string kNoVariant;
    return getOrUploadMesh(name, name, m_noOverrides, kNoVariant);
}

MeshHandle SceneRenderer::getOrUploadMesh(const std::string& meshAssetName, const std::string& cacheKey,
                                          const std::unordered_map<std::string, std::string>& liveryOverrides,
                                          const std::string& variant) {
    auto it = m_meshCache.find(cacheKey);
    if (it != m_meshCache.end())
        return it->second;

    auto data = m_assets.loadMesh(meshAssetName.c_str());
    if (!data || data->bytes.empty()) {
        // Warn once per key (#832): the empty handle is cached, so this path runs only on the first
        // miss. Distinguish this "no bytes" cause from an upload failure below.
        if (m_logger) {
            char buf[208];
            std::snprintf(buf, sizeof(buf),
                          "mesh '%s' not found in any content pack (missing, wrong asset name, or empty) "
                          "— drawing placeholder",
                          meshAssetName.c_str());
            m_logger->log(LogLevel::Warn, __FILE__, __LINE__, buf);
        }
        m_meshCache[cacheKey] = MeshHandle{};
        return MeshHandle{};
    }

    MeshUploadDesc desc{meshAssetName, data->bytes};
    // Variant node-set (#882): upload only the nodes this entity type selects. Empty keeps the
    // untagged set — the whole mesh, for every .glb authored before variants existed.
    desc.variant = variant;
    // Pack-authored meshes are in the standard glTF/Blender content convention (nose +Z); the loader
    // rotates them into the engine body frame (nose +X) on upload (#906). The builtin placeholders and
    // terrain are engine-generated in the body/world frame and keep contentForward false.
    desc.contentForward = true;
    // Resolve the glb's external texture URIs to file bytes through the content system (#833). The
    // renderer parses the PBR material and builds the GPU material; we only supply the bytes, because
    // URI → asset-name → file mapping is a content-pack concern, not a GPU-backend one.
    //
    // Livery override (#845): a re-skin swaps the TEXTURE the material's map resolves to, never the
    // geometry/UVs. The shared resolver (MeshTextureResolver.h) derives each URI's base texture, applies
    // the "<slot>.<map>" override with per-map fallback to base, and loads the bytes — a partial/broken
    // livery degrades, never fails. `liveryOverrides` outlives this call (owned by the caller /
    // m_liveryCache) and the resolver is invoked synchronously inside createMesh below.
    //
    // Wrap the resolver to record which Texture asset names this mesh actually consumed (#152), so a
    // hot-reload texture change re-uploads exactly the meshes that referenced it.
    auto baseResolver = makeMeshTextureResolver(m_assets, liveryOverrides);
    std::vector<std::string>& deps = m_meshTextureDeps[cacheKey];
    deps.clear();
    desc.textureResolver = [&deps, &liveryOverrides,
                            baseResolver = std::move(baseResolver)](std::string_view uri) -> std::vector<uint8_t> {
        // Record both the resolved (livery-override) name and the base name — a change to either
        // must re-upload this mesh.
        const std::string baseAsset = textureAssetNameFromUri(uri);
        deps.push_back(baseAsset);
        if (!liveryOverrides.empty()) {
            const std::string key = liveryKeyFromBaseAsset(baseAsset);
            if (!key.empty()) {
                if (auto ov = liveryOverrides.find(key); ov != liveryOverrides.end())
                    deps.push_back(ov->second);
            }
        }
        return baseResolver(uri);
    };
    MeshHandle h = m_renderer.createMesh(desc);
    if (!h.valid() && m_logger) {
        char buf[176];
        std::snprintf(buf, sizeof(buf),
                      "mesh '%s' failed to upload (corrupt or unsupported .glb) — drawing placeholder",
                      meshAssetName.c_str());
        m_logger->log(LogLevel::Warn, __FILE__, __LINE__, buf);
    }
    m_meshCache[cacheKey] = h;
    return h;
}

MaterialHandle SceneRenderer::getOrUploadMaterial(const std::string& cacheKey) {
    auto it = m_materialCache.find(cacheKey);
    if (it != m_materialCache.end())
        return it->second;

    // Prefer the material createMesh parsed from the mesh's own glb (#833). getOrUploadMesh runs
    // first for this key (renderFrame calls it immediately before this), so the handle is cached.
    // A mesh with no material — none authored, or the texture resolver missed — falls back to the
    // shared shaded-grey material (ensureBuiltins() always runs before this method).
    MaterialHandle mat = m_fallbackEntityMat;
    if (auto mit = m_meshCache.find(cacheKey); mit != m_meshCache.end() && mit->second.valid()) {
        if (MaterialHandle meshMat = m_renderer.getMeshMaterial(mit->second); meshMat.valid())
            mat = meshMat;
    }
    m_materialCache[cacheKey] = mat;
    return mat;
}

const SceneRenderer::LiveryTextureSet* SceneRenderer::resolveLivery(uint32_t typeIndex) {
    if (!m_liveryResolver)
        return nullptr;
    auto it = m_liveryCache.find(typeIndex);
    if (it == m_liveryCache.end()) {
        LiveryTextureSet set;
        if (!m_liveryResolver(typeIndex, set))
            set = LiveryTextureSet{}; // resolved: no livery (empty id) — cache the negative result too
        it = m_liveryCache.emplace(typeIndex, std::move(set)).first;
    }
    return it->second.id.empty() ? nullptr : &it->second;
}

void SceneRenderer::setLiveryResolver(LiveryResolver resolver) noexcept {
    m_liveryResolver = std::move(resolver);
    m_liveryCache.clear(); // a new resolver may re-skin already-seen types
}

// ── Hot-reload invalidation (#152) ──────────────────────────────────────────

void SceneRenderer::evictMeshCacheKey(const std::string& cacheKey) {
    if (auto it = m_meshCache.find(cacheKey); it != m_meshCache.end()) {
        if (it->second.valid())
            m_renderer.destroyMesh(it->second); // cascades material + textures (#152)
        m_meshCache.erase(it);
    }
    // The material cache entry is either the mesh-owned material (destroyed by the cascade) or the
    // shared grey fallback (must never be destroyed) — so drop the entry without destroyMaterial.
    m_materialCache.erase(cacheKey);
    m_meshTextureDeps.erase(cacheKey);
}

void SceneRenderer::invalidateMesh(std::string_view meshAssetName) {
    const std::string base(meshAssetName);
    // Evict the plain key and every derived key: "<name>#<variant>" (#882) and "<name>@@<livery>"
    // (#845), in either combination. Both separators are illegal in an asset name, so a prefix match
    // up to the first separator identifies exactly this mesh's keys.
    const std::string livery = base + "@@";
    const std::string variant = base + "#";
    std::vector<std::string> keys;
    for (const auto& [k, _] : m_meshCache)
        if (k == base || k.compare(0, livery.size(), livery) == 0 || k.compare(0, variant.size(), variant) == 0)
            keys.push_back(k);
    for (const auto& k : keys)
        evictMeshCacheKey(k);
}

void SceneRenderer::invalidateTexture(std::string_view textureAssetName) {
    const std::string tex(textureAssetName);
    // Re-upload every mesh whose recorded deps include this texture.
    std::vector<std::string> keys;
    for (const auto& [k, deps] : m_meshTextureDeps)
        if (std::find(deps.begin(), deps.end(), tex) != deps.end())
            keys.push_back(k);
    for (const auto& k : keys)
        evictMeshCacheKey(k);
}

void SceneRenderer::invalidateLiveries() {
    m_liveryCache.clear();
    // Every livery-variant cache key carries the "@@" separator.
    std::vector<std::string> keys;
    for (const auto& [k, _] : m_meshCache)
        if (k.find("@@") != std::string::npos)
            keys.push_back(k);
    for (const auto& k : keys)
        evictMeshCacheKey(k);
}

void SceneRenderer::invalidateAllAssets() {
    std::vector<std::string> keys;
    keys.reserve(m_meshCache.size());
    for (const auto& [k, _] : m_meshCache)
        keys.push_back(k);
    for (const auto& k : keys)
        evictMeshCacheKey(k);
    m_meshCache.clear();
    m_materialCache.clear();
    m_meshTextureDeps.clear();
    m_liveryCache.clear();
    m_typeNameCache.clear();
    // Builtin placeholders + floor (m_builtin*, m_fallbackEntityMat) are untouched — they never come
    // from a pack, so a content change can't stale them.
}

} // namespace fl
