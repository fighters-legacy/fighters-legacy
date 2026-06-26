// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "render/BuiltinGeometry.h"
#include "render/CameraController.h"
#include "render/RenderSnapshot.h"
#include "render/SceneRenderer.h"
#include "render/SimRenderBridge.h"

#include "content/AssetManager.h"
#include "content/AssetTypes.h"
#include "content/IContentPack.h"

#include "mock_content.h"
#include "mock_hal.h"

#include <cmath>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace fl;

// ---------------------------------------------------------------------------
// Minimal mock content pack for asset loading tests
// ---------------------------------------------------------------------------

// Serves meshes from an in-memory map; everything else null-object (see mock_content.h).
struct MockContentPack : NullContentPack {
    std::string packId{"test:mock"};
    std::unordered_map<std::string, std::vector<uint8_t>> meshes;

    const char* name() const override {
        return "MockPack";
    }
    const char* version() const override {
        return "0.0.1";
    }
    const char* id() const override {
        return packId.c_str();
    }
    bool hasAsset(const char* n, AssetType t) const override {
        return t == AssetType::Mesh && meshes.count(n);
    }
    std::optional<MeshData> loadMesh(const char* n) override {
        auto it = meshes.find(n);
        if (it == meshes.end())
            return std::nullopt;
        MeshData d;
        d.name = n;
        d.bytes = it->second;
        return d;
    }
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Build a simple always-false resolver (no types known).
static SceneRenderer::MeshNameResolver noTypes() {
    return [](uint32_t, std::string&, std::string&) { return false; };
}

// Build a resolver that maps typeIndex 0 -> ("f15c", "").
static SceneRenderer::MeshNameResolver oneType(const std::string& mesh = "f15c", const std::string& dmg = "") {
    return [mesh, dmg](uint32_t idx, std::string& m, std::string& d) -> bool {
        if (idx != 0)
            return false;
        m = mesh;
        d = dmg;
        return true;
    };
}

static RenderSnapshot makeSnap(uint64_t tick = 1) {
    RenderSnapshot snap;
    snap.tickIndex = tick;
    return snap;
}

static EntityRenderEntry makeEntry(uint32_t typeIndex = 0, glm::dvec3 pos = {}) {
    EntityRenderEntry e;
    e.typeIndex = typeIndex;
    e.position = pos;
    e.orientation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    return e;
}

// ---------------------------------------------------------------------------
// CameraController — Free mode
// ---------------------------------------------------------------------------

TEST_CASE("CameraController defaults to Cockpit mode") {
    CameraController cam;
    CHECK(cam.mode() == CameraMode::Cockpit);
}

TEST_CASE("CameraController projection encodes near plane in proj[3][2]") {
    CameraController cam;
    float near = 0.5f;
    CameraView cv = cam.view(1.0f, 1.0472f, near);
    // Infinite reverse-Z: proj[3][2] == near.
    CHECK(cv.proj[3][2] == Catch::Approx(near));
}

TEST_CASE("CameraController projection flips Y for Vulkan") {
    CameraController cam;
    CameraView cv = cam.view(1.0f);
    // proj[1][1] is negative (Vulkan Y-flip).
    CHECK(cv.proj[1][1] < 0.0f);
}

TEST_CASE("CameraController proj[2][3] is -1 for RH perspective") {
    CameraController cam;
    CameraView cv = cam.view(1.0f);
    CHECK(cv.proj[2][3] == Catch::Approx(-1.0f));
}

TEST_CASE("CameraController setPose sets worldOrigin to the eye") {
    CameraController cam;
    cam.setPose(glm::dvec3{100.0, 200.0, 300.0}, glm::vec3{1.f, 0.f, 0.f}, glm::vec3{0.f, 1.f, 0.f});
    CameraView cv = cam.view(16.0f / 9.0f);
    CHECK(cv.worldOrigin.x == Catch::Approx(100.0).margin(1e-4));
    CHECK(cv.worldOrigin.y == Catch::Approx(200.0).margin(1e-4));
    CHECK(cv.worldOrigin.z == Catch::Approx(300.0).margin(1e-4));
}

TEST_CASE("CameraController setPose orients the view along forward") {
    CameraController cam;
    // Look along world +X: glm::lookAt stores -forward at view[0][2], so it should be -1.
    cam.setPose(glm::dvec3(0), glm::vec3{1.f, 0.f, 0.f}, glm::vec3{0.f, 1.f, 0.f});
    CameraView cv = cam.view(1.0f);
    CHECK(cv.proj[2][3] == Catch::Approx(-1.0f)); // RH perspective preserved
    CHECK(cv.view[0][2] == Catch::Approx(-1.0f).margin(1e-4f));
}

TEST_CASE("CameraController setMode changes mode") {
    CameraController cam;
    cam.setMode(CameraMode::Cockpit);
    CHECK(cam.mode() == CameraMode::Cockpit);
    cam.setMode(CameraMode::Chase);
    CHECK(cam.mode() == CameraMode::Chase);
    cam.setMode(CameraMode::Free);
    CHECK(cam.mode() == CameraMode::Free);
}

TEST_CASE("CameraController view guards against forward parallel to up") {
    CameraController cam;
    // Forward straight up with up also +Y: the guard must substitute a non-parallel up (no NaN).
    cam.setPose(glm::dvec3(0), glm::vec3{0.f, 1.f, 0.f}, glm::vec3{0.f, 1.f, 0.f});
    CameraView cv = cam.view(1.0f);
    CHECK(!std::isnan(cv.view[0][0]));
    CHECK(!std::isnan(cv.view[1][1]));
    CHECK(!std::isnan(cv.view[2][2]));
}

// ---------------------------------------------------------------------------
// SceneRenderer — no snapshot
// ---------------------------------------------------------------------------

TEST_CASE("SceneRenderer with no snapshot submits empty scene once") {
    MockLogger logger;
    auto pack = std::make_unique<MockContentPack>();
    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.push_back(std::move(pack));
    AssetManager assets{std::move(packs), logger};
    assets.initialize(nullptr);

    MockRenderer renderer;
    SimRenderBridge bridge;

    SceneRenderer sr{bridge, noTypes(), assets, renderer};

    CameraView cam;
    EnvironmentState env;
    sr.renderFrame(0.0f, cam, env);

    CHECK(renderer.setSceneCount == 1);
    CHECK(renderer.lastScene.renderItems.empty());
}

// ---------------------------------------------------------------------------
// SceneRenderer — entity with loadable mesh
// ---------------------------------------------------------------------------

TEST_CASE("SceneRenderer submits one RenderItem when entity has loadable mesh") {
    MockLogger logger;
    auto pack = std::make_unique<MockContentPack>();
    pack->meshes["f15c"] = {'{', 2, 3, 4}; // valid JSON-glTF first byte

    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.push_back(std::move(pack));
    AssetManager assets{std::move(packs), logger};
    assets.initialize(nullptr);

    MockRenderer renderer;
    SimRenderBridge bridge;
    SceneRenderer sr{bridge, oneType(), assets, renderer};

    RenderSnapshot snap = makeSnap();
    snap.entries.push_back(makeEntry(0, {10.0, 0.0, 0.0}));
    bridge.publish(std::move(snap));

    sr.renderFrame(0.0f, CameraView{}, EnvironmentState{});

    REQUIRE(renderer.setSceneCount == 1);
    REQUIRE(renderer.lastScene.renderItems.size() == 1);
    CHECK(renderer.lastScene.renderItems[0].mesh.valid());
    // ensureBuiltins() uploads 2 meshes (entity + floor) + 8 materials (6 palette + 1 floor +
    // 1 shaded-grey fallback); the content mesh adds 1 more createMesh but reuses the shared
    // fallback material (no new createMaterial).
    CHECK(renderer.createMeshCount == 3);
    CHECK(renderer.createMaterialCount == 8);
}

TEST_CASE("SceneRenderer falls back to builtin for entity with unknown typeIndex") {
    MockLogger logger;
    auto pack = std::make_unique<MockContentPack>();
    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.push_back(std::move(pack));
    AssetManager assets{std::move(packs), logger};
    assets.initialize(nullptr);

    MockRenderer renderer;
    SimRenderBridge bridge;
    SceneRenderer sr{bridge, noTypes(), assets, renderer};

    RenderSnapshot snap = makeSnap();
    snap.entries.push_back(makeEntry(42)); // no resolver for type 42
    bridge.publish(std::move(snap));

    sr.renderFrame(0.0f, CameraView{}, EnvironmentState{});

    REQUIRE(renderer.setSceneCount == 1);
    REQUIRE(renderer.lastScene.renderItems.size() == 1);
    CHECK(renderer.lastScene.renderItems[0].mesh.valid()); // builtin tetrahedron
}

TEST_CASE("SceneRenderer falls back to builtin when mesh bytes are empty") {
    MockLogger logger;
    auto pack = std::make_unique<MockContentPack>();
    pack->meshes["empty_mesh"] = {}; // empty bytes — upload will fail

    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.push_back(std::move(pack));
    AssetManager assets{std::move(packs), logger};
    assets.initialize(nullptr);

    MockRenderer renderer;
    SimRenderBridge bridge;
    SceneRenderer sr{bridge, oneType("empty_mesh"), assets, renderer};

    RenderSnapshot snap = makeSnap();
    snap.entries.push_back(makeEntry(0));
    bridge.publish(std::move(snap));

    sr.renderFrame(0.0f, CameraView{}, EnvironmentState{});

    REQUIRE(renderer.setSceneCount == 1);
    REQUIRE(renderer.lastScene.renderItems.size() == 1);
    CHECK(renderer.lastScene.renderItems[0].mesh.valid()); // builtin tetrahedron
    // createMesh was NOT called for the broken empty_mesh; only ensureBuiltins uploads.
    CHECK(renderer.createMeshCount == 2); // builtin entity + builtin floor
}

// ---------------------------------------------------------------------------
// SceneRenderer — damage variant selection
// ---------------------------------------------------------------------------

TEST_CASE("SceneRenderer uses classicDamageMesh when damageLevel is nonzero") {
    MockLogger logger;
    auto pack = std::make_unique<MockContentPack>();
    pack->meshes["f15c"] = {'{', 2, 3};         // valid JSON-glTF first byte
    pack->meshes["f15c_damaged"] = {'{', 5, 6}; // valid JSON-glTF first byte

    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.push_back(std::move(pack));
    AssetManager assets{std::move(packs), logger};
    assets.initialize(nullptr);

    MockRenderer renderer;
    SimRenderBridge bridge;
    // Type 0: mesh="f15c", damageMesh="f15c_damaged"
    SceneRenderer sr{bridge, oneType("f15c", "f15c_damaged"), assets, renderer};

    EntityRenderEntry damaged = makeEntry(0);
    damaged.damageLevel = 1; // any nonzero value

    RenderSnapshot snap = makeSnap();
    snap.entries.push_back(damaged);
    bridge.publish(std::move(snap));

    sr.renderFrame(0.0f, CameraView{}, EnvironmentState{});

    REQUIRE(renderer.setSceneCount == 1);
    REQUIRE(renderer.lastScene.renderItems.size() == 1);
    // Only f15c_damaged was needed for content (f15c was not loaded).
    // ensureBuiltins adds 2 builtin meshes → 3 total.
    CHECK(renderer.createMeshCount == 3);
    CHECK((renderer.lastScene.renderItems[0].flags & kRenderFlagDamaged) != 0);
}

TEST_CASE("SceneRenderer uses primary mesh when damageLevel is zero") {
    MockLogger logger;
    auto pack = std::make_unique<MockContentPack>();
    pack->meshes["f15c"] = {'{', 2, 3}; // valid JSON-glTF first byte

    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.push_back(std::move(pack));
    AssetManager assets{std::move(packs), logger};
    assets.initialize(nullptr);

    MockRenderer renderer;
    SimRenderBridge bridge;
    SceneRenderer sr{bridge, oneType("f15c", "f15c_damaged"), assets, renderer};

    EntityRenderEntry intact = makeEntry(0);
    intact.damageLevel = 0;

    RenderSnapshot snap = makeSnap();
    snap.entries.push_back(intact);
    bridge.publish(std::move(snap));

    sr.renderFrame(0.0f, CameraView{}, EnvironmentState{});

    REQUIRE(renderer.lastScene.renderItems.size() == 1);
    CHECK((renderer.lastScene.renderItems[0].flags & kRenderFlagDamaged) == 0);
    CHECK(renderer.createMeshCount == 3); // ensureBuiltins (2) + "f15c" (1)
}

// ---------------------------------------------------------------------------
// SceneRenderer — mesh caching
// ---------------------------------------------------------------------------

TEST_CASE("SceneRenderer caches mesh handle: createMesh called once for repeated frames") {
    MockLogger logger;
    auto pack = std::make_unique<MockContentPack>();
    pack->meshes["f15c"] = {'{', 2, 3}; // valid JSON-glTF first byte

    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.push_back(std::move(pack));
    AssetManager assets{std::move(packs), logger};
    assets.initialize(nullptr);

    MockRenderer renderer;
    SimRenderBridge bridge;
    SceneRenderer sr{bridge, oneType(), assets, renderer};

    // Frame 1
    {
        RenderSnapshot snap = makeSnap(1);
        snap.entries.push_back(makeEntry(0));
        bridge.publish(std::move(snap));
        sr.renderFrame(0.0f, CameraView{}, EnvironmentState{});
    }
    // Frame 2 — same entity type; mesh must come from cache
    {
        RenderSnapshot snap = makeSnap(2);
        snap.entries.push_back(makeEntry(0));
        bridge.publish(std::move(snap));
        sr.renderFrame(0.0f, CameraView{}, EnvironmentState{});
    }

    // ensureBuiltins on frame 1: 2 meshes + 8 materials (6 palette + floor + fallback). Content
    // "f15c": +1 mesh, reuses the shared fallback material. Frame 2: fully cached.
    CHECK(renderer.createMeshCount == 3);
    CHECK(renderer.createMaterialCount == 8);
    CHECK(renderer.setSceneCount == 2);
}

TEST_CASE("SceneRenderer shares one fallback material across distinct loadable meshes") {
    MockLogger logger;
    auto pack = std::make_unique<MockContentPack>();
    pack->meshes["f15c"] = {'{', 2, 3};
    pack->meshes["mig29"] = {'{', 4, 5};

    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.push_back(std::move(pack));
    AssetManager assets{std::move(packs), logger};
    assets.initialize(nullptr);

    MockRenderer renderer;
    SimRenderBridge bridge;
    // Resolver maps typeIndex 0 -> f15c, 1 -> mig29.
    SceneRenderer sr{bridge,
                     [](uint32_t typeIndex, std::string& mesh, std::string& dmg) {
                         dmg.clear();
                         mesh = (typeIndex == 0) ? "f15c" : "mig29";
                         return true;
                     },
                     assets, renderer};

    RenderSnapshot snap = makeSnap();
    snap.entries.push_back(makeEntry(0, {10.0, 0.0, 0.0})); // typeIndex 0 -> f15c
    snap.entries.push_back(makeEntry(1, {20.0, 0.0, 0.0})); // typeIndex 1 -> mig29
    bridge.publish(std::move(snap));

    sr.renderFrame(0.0f, CameraView{}, EnvironmentState{});

    // Two distinct content meshes uploaded, but no per-mesh material: still just the 8 builtins
    // (6 palette + floor + fallback). Both meshes share m_fallbackEntityMat.
    CHECK(renderer.createMeshCount == 4); // 2 builtin + 2 content
    CHECK(renderer.createMaterialCount == 8);
    REQUIRE(renderer.lastScene.renderItems.size() == 2);
    CHECK(renderer.lastScene.renderItems[0].material.id == renderer.lastScene.renderItems[1].material.id);
}

// ---------------------------------------------------------------------------
// SceneRenderer — camera and environment pass-through
// ---------------------------------------------------------------------------

TEST_CASE("SceneRenderer passes camera and environment through to FrameScene") {
    MockLogger logger;
    auto pack = std::make_unique<MockContentPack>();
    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.push_back(std::move(pack));
    AssetManager assets{std::move(packs), logger};
    assets.initialize(nullptr);

    MockRenderer renderer;
    SimRenderBridge bridge;
    SceneRenderer sr{bridge, noTypes(), assets, renderer};

    CameraView cam;
    cam.worldOrigin = {10.0, 20.0, 30.0};

    EnvironmentState env;
    env.sunDirection = {0.0f, -0.8f, 0.6f};
    env.timeOfDay = 7.5f;

    sr.renderFrame(0.0f, cam, env);

    CHECK(renderer.lastScene.camera.worldOrigin == glm::dvec3(10.0, 20.0, 30.0));
    CHECK(renderer.lastScene.environment.sunDirection == glm::vec3(0.0f, -0.8f, 0.6f));
    CHECK(renderer.lastScene.environment.timeOfDay == Catch::Approx(7.5f));
}

// ---------------------------------------------------------------------------
// SceneRenderer — velocity extrapolation
// ---------------------------------------------------------------------------

TEST_CASE("SceneRenderer applies velocity extrapolation to transform position") {
    MockLogger logger;
    auto pack = std::make_unique<MockContentPack>();
    pack->meshes["f15c"] = {'{', 2, 3}; // valid JSON-glTF first byte

    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.push_back(std::move(pack));
    AssetManager assets{std::move(packs), logger};
    assets.initialize(nullptr);

    MockRenderer renderer;
    SimRenderBridge bridge;
    SceneRenderer sr{bridge, oneType(), assets, renderer};

    EntityRenderEntry e = makeEntry(0, {});
    e.velocity = {60.0f, 0.0f, 0.0f}; // 60 m/s along X

    RenderSnapshot snap = makeSnap();
    snap.entries.push_back(e);
    bridge.publish(std::move(snap));

    // alpha=1.0 → extrapolate 1 full tick period (1/60 s) → Δx = 60 * (1.0 * 1/60) = 1.0 m
    sr.renderFrame(1.0f, CameraView{}, EnvironmentState{});

    REQUIRE(renderer.lastScene.renderItems.size() == 1);
    // transform[3] is the translation column.
    float tx = renderer.lastScene.renderItems[0].transform[3][0];
    CHECK(tx == Catch::Approx(1.0f).margin(1e-4f));
}

// ---------------------------------------------------------------------------
// SceneRenderer -- draw-distance cull
// ---------------------------------------------------------------------------

TEST_CASE("SceneRenderer culls entity beyond draw distance") {
    MockLogger logger;
    auto pack = std::make_unique<MockContentPack>();
    pack->meshes["f15c"] = {'{', 2, 3}; // valid JSON-glTF first byte

    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.push_back(std::move(pack));
    AssetManager assets{std::move(packs), logger};
    assets.initialize(nullptr);

    MockRenderer renderer;
    SimRenderBridge bridge;
    SceneRenderer sr{bridge, oneType(), assets, renderer};

    // Set a tight draw distance: 10 km
    sr.setDrawDistance(10.0f);

    // Entity 11 km away (beyond the 10 km limit)
    RenderSnapshot snap = makeSnap();
    snap.entries.push_back(makeEntry(0, {11000.0, 0.0, 0.0}));
    bridge.publish(std::move(snap));

    sr.renderFrame(0.0f, CameraView{}, EnvironmentState{});

    CHECK(renderer.setSceneCount == 1);
    CHECK(renderer.lastScene.renderItems.empty()); // culled
}

TEST_CASE("SceneRenderer keeps entity within draw distance") {
    MockLogger logger;
    auto pack = std::make_unique<MockContentPack>();
    pack->meshes["f15c"] = {'{', 2, 3}; // valid JSON-glTF first byte

    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.push_back(std::move(pack));
    AssetManager assets{std::move(packs), logger};
    assets.initialize(nullptr);

    MockRenderer renderer;
    SimRenderBridge bridge;
    SceneRenderer sr{bridge, oneType(), assets, renderer};

    // Set draw distance to 10 km; entity at 9 km is within range
    sr.setDrawDistance(10.0f);

    RenderSnapshot snap = makeSnap();
    snap.entries.push_back(makeEntry(0, {9000.0, 0.0, 0.0}));
    bridge.publish(std::move(snap));

    sr.renderFrame(0.0f, CameraView{}, EnvironmentState{});

    REQUIRE(renderer.setSceneCount == 1);
    CHECK(renderer.lastScene.renderItems.size() == 1);
}

// ---------------------------------------------------------------------------
// MockRenderer -- applySettings is a no-op
// ---------------------------------------------------------------------------

TEST_CASE("MockRenderer applySettings accepts any RendererSettings") {
    MockRenderer renderer;
    RendererSettings rs{};
    rs.vsync = RendererVsyncMode::Off;
    rs.aaMode = RendererAAMode::Off;
    rs.bloom = false;
    rs.drawDistanceKm = 20.0f;
    renderer.applySettings(rs);     // must not crash
    CHECK(renderer.initCount == 0); // no side effects on other counters
}

// ---------------------------------------------------------------------------
// SceneRenderer -- builtin palette fallback
// ---------------------------------------------------------------------------

TEST_CASE("SceneRenderer falls back to builtin palette when resolver returns false") {
    MockLogger logger;
    std::vector<std::unique_ptr<IContentPack>> packs;
    AssetManager assets{std::move(packs), logger};
    assets.initialize(nullptr);

    MockRenderer renderer;
    SimRenderBridge bridge;
    SceneRenderer sr{bridge, noTypes(), assets, renderer};

    EntityRenderEntry entry = makeEntry(0);
    entry.entityIdx = 0;
    RenderSnapshot snap = makeSnap();
    snap.entries.push_back(entry);
    bridge.publish(std::move(snap));

    sr.renderFrame(0.0f, CameraView{}, EnvironmentState{});

    REQUIRE(renderer.setSceneCount == 1);
    REQUIRE(renderer.lastScene.renderItems.size() == 1);
    CHECK(renderer.lastScene.renderItems[0].mesh.valid()); // builtin tetrahedron
    CHECK(renderer.lastScene.renderItems[0].material.valid());
}

TEST_CASE("SceneRenderer marks the hidden entity shadow-only (cockpit ownship)") {
    MockLogger logger;
    std::vector<std::unique_ptr<IContentPack>> packs;
    AssetManager assets{std::move(packs), logger};
    assets.initialize(nullptr);

    MockRenderer renderer;
    SimRenderBridge bridge;
    SceneRenderer sr{bridge, noTypes(), assets, renderer};

    EntityRenderEntry self = makeEntry(0);
    self.entityIdx = 0;
    self.entityGen = 1;
    EntityRenderEntry other = makeEntry(0, {100.0, 0.0, 0.0});
    other.entityIdx = 1;
    other.entityGen = 1;

    auto publishBoth = [&]() {
        RenderSnapshot snap = makeSnap();
        snap.entries.push_back(self);
        snap.entries.push_back(other);
        bridge.publish(std::move(snap));
    };
    auto shadowOnlyCount = [&]() {
        int n = 0;
        for (const auto& it : renderer.lastScene.renderItems)
            if (it.flags & kRenderFlagShadowOnly)
                ++n;
        return n;
    };

    // Hide the player's own entity (idx 0, gen 1): it is still submitted (so it casts a shadow)
    // but flagged shadow-only; the other entity renders normally.
    publishBoth();
    sr.setHiddenEntity(0, 1);
    sr.renderFrame(0.0f, CameraView{}, EnvironmentState{});
    REQUIRE(renderer.lastScene.renderItems.size() == 2);
    CHECK(shadowOnlyCount() == 1);

    // gen == 0 disables the filter: both entities render normally.
    publishBoth();
    sr.setHiddenEntity(0, 0);
    sr.renderFrame(0.0f, CameraView{}, EnvironmentState{});
    REQUIRE(renderer.lastScene.renderItems.size() == 2);
    CHECK(shadowOnlyCount() == 0);

    // A non-matching generation does not hide the entity (stale-id guard).
    publishBoth();
    sr.setHiddenEntity(0, 99);
    sr.renderFrame(0.0f, CameraView{}, EnvironmentState{});
    CHECK(shadowOnlyCount() == 0);
}

TEST_CASE("SceneRenderer assigns distinct palette materials by entityIdx") {
    MockLogger logger;
    std::vector<std::unique_ptr<IContentPack>> packs;
    AssetManager assets{std::move(packs), logger};
    assets.initialize(nullptr);

    MockRenderer renderer;
    SimRenderBridge bridge;
    SceneRenderer sr{bridge, noTypes(), assets, renderer};

    EntityRenderEntry e0 = makeEntry(0, {0.0, 0.0, 0.0});
    e0.entityIdx = 0;
    EntityRenderEntry e1 = makeEntry(0, {1.0, 0.0, 0.0});
    e1.entityIdx = 1;

    RenderSnapshot snap = makeSnap();
    snap.entries.push_back(e0);
    snap.entries.push_back(e1);
    bridge.publish(std::move(snap));

    sr.renderFrame(0.0f, CameraView{}, EnvironmentState{});

    REQUIRE(renderer.setSceneCount == 1);
    REQUIRE(renderer.lastScene.renderItems.size() == 2);
    const auto& items = renderer.lastScene.renderItems;
    CHECK(items[0].material.valid());
    CHECK(items[1].material.valid());
    // entityIdx 0 and 1 map to different palette slots.
    CHECK(items[0].material.id != items[1].material.id);
}

// ---------------------------------------------------------------------------
// SceneRenderer — planet-scale precision and floor coverage
// ---------------------------------------------------------------------------

TEST_CASE("SceneRenderer camera-relative rendering is float32-safe at 2000 km from origin", "[scene_renderer]") {
    // Entity and camera are both at 2,000 km from world origin. The camera-relative
    // offset must be exact (near zero), not corrupted by float32 precision loss.
    MockLogger logger;
    std::vector<std::unique_ptr<IContentPack>> packs;
    AssetManager assets{std::move(packs), logger};
    assets.initialize(nullptr);

    MockRenderer renderer;
    SimRenderBridge bridge;
    SceneRenderer sr{bridge, noTypes(), assets, renderer};

    constexpr double kLarge = 2'000'000.0; // 2,000 km

    RenderSnapshot snap = makeSnap();
    snap.entries.push_back(makeEntry(0, {kLarge, 100.0, kLarge}));
    bridge.publish(std::move(snap));

    CameraView cam;
    cam.worldOrigin = {kLarge, 0.0, kLarge};

    sr.renderFrame(0.0f, cam, EnvironmentState{});

    REQUIRE(renderer.setSceneCount == 1);
    REQUIRE(renderer.lastScene.renderItems.size() == 1);
    // transform[3] is the translation column (glm column-major).
    // Camera-relative offset: entity at (kLarge, 100, kLarge) minus camera at (kLarge, 0, kLarge) = (0, 100, 0).
    const glm::vec4& t = renderer.lastScene.renderItems[0].transform[3];
    CHECK(t.x == Catch::Approx(0.0f).margin(1e-3f));
    CHECK(t.y == Catch::Approx(100.0f).margin(1e-3f));
    CHECK(t.z == Catch::Approx(0.0f).margin(1e-3f));
}

TEST_CASE("SceneRenderer builtin floor camera-relative offset uses dvec3 worldOrigin", "[scene_renderer]") {
    MockLogger logger;
    std::vector<std::unique_ptr<IContentPack>> packs;
    AssetManager assets{std::move(packs), logger};
    assets.initialize(nullptr);

    MockRenderer renderer;
    SimRenderBridge bridge;
    SceneRenderer sr{bridge, noTypes(), assets, renderer};
    sr.setBuiltinFloor(true);

    // Publish an empty snapshot so renderFrame doesn't early-out before appending the floor.
    RenderSnapshot snap;
    snap.tickIndex = 1;
    bridge.publish(std::move(snap));

    CameraView cam;
    cam.worldOrigin = {50.0, 0.0, 75.0};

    sr.renderFrame(0.0f, cam, EnvironmentState{});

    REQUIRE(renderer.setSceneCount == 1);
    // Floor is the last item; transform translates by -worldOrigin cast to vec3.
    const auto& items = renderer.lastScene.renderItems;
    REQUIRE(!items.empty());
    const glm::vec4& ft = items.back().transform[3];
    CHECK(ft.x == Catch::Approx(-50.0f).margin(1e-4f));
    CHECK(ft.z == Catch::Approx(-75.0f).margin(1e-4f));
}

// ---------------------------------------------------------------------------
// Builtin tetrahedron winding / normals
// ---------------------------------------------------------------------------

// The builtin entity tetrahedron must have outward-facing normals (CCW winding when
// viewed from outside). With inward normals the opaque pipeline (frontFace=CW after the
// Vulkan Y-flip, cull BACK) renders the mesh inside-out: visible from every camera angle
// and, critically, NOT culled in cockpit view where the camera sits at the centroid.
// Regression guard for the inverted-winding bug.
TEST_CASE("Builtin tetrahedron has outward-facing normals") {
    const std::span<const uint8_t> glb = builtinTetrahedronGlb();
    REQUIRE(glb.size() > 20u);

    auto readU32 = [&](std::size_t off) {
        uint32_t v = 0;
        std::memcpy(&v, glb.data() + off, 4);
        return v;
    };
    // GLB: 12-byte header, then JSON chunk (4-byte len + 4-byte type "JSON" + data),
    // then BIN chunk (4-byte len + 4-byte type "BIN\0" + data).
    const uint32_t jsonLen = readU32(12);
    const std::size_t binStart = 12u + 8u + jsonLen + 8u;
    REQUIRE(glb.size() >= binStart + 12u * 12u * 2u);

    // Non-interleaved layout: 12 POSITION vec3 (144 B) then 12 NORMAL vec3 (144 B).
    auto readVec3 = [&](std::size_t off) {
        float x, y, z;
        std::memcpy(&x, glb.data() + off + 0, 4);
        std::memcpy(&y, glb.data() + off + 4, 4);
        std::memcpy(&z, glb.data() + off + 8, 4);
        return glm::vec3{x, y, z};
    };

    constexpr int kVerts = 12;
    const std::size_t normBase = binStart + static_cast<std::size_t>(kVerts) * 12u;

    // The mesh origin is the ground-contact point, not the centroid, so compute the centroid
    // (mean of the 12 vertices) and check each normal points away from it (outward).
    glm::vec3 centroid{0.f};
    for (int i = 0; i < kVerts; ++i)
        centroid += readVec3(binStart + static_cast<std::size_t>(i) * 12u);
    centroid /= static_cast<float>(kVerts);

    for (int i = 0; i < kVerts; ++i) {
        const glm::vec3 pos = readVec3(binStart + static_cast<std::size_t>(i) * 12u);
        const glm::vec3 nrm = readVec3(normBase + static_cast<std::size_t>(i) * 12u);
        // An outward normal points away from the centroid: dot(normal, pos - centroid) > 0.
        CHECK(glm::dot(nrm, pos - centroid) > 0.0f);
    }

    // Standard glTF winding (CCW-from-outside): each face's winding cross-product must AGREE with
    // its outward stored normal (dot > 0). The engine front-faces this (frontFace=CCW). A negative
    // dot means the mesh is wound inside-out (back faces shown, front faces culled).
    for (int f = 0; f < kVerts / 3; ++f) {
        const glm::vec3 v0 = readVec3(binStart + static_cast<std::size_t>(f * 3 + 0) * 12u);
        const glm::vec3 v1 = readVec3(binStart + static_cast<std::size_t>(f * 3 + 1) * 12u);
        const glm::vec3 v2 = readVec3(binStart + static_cast<std::size_t>(f * 3 + 2) * 12u);
        const glm::vec3 storedNormal = readVec3(normBase + static_cast<std::size_t>(f * 3) * 12u);
        const glm::vec3 windingCross = glm::cross(v1 - v0, v2 - v0);
        CHECK(glm::dot(windingCross, storedNormal) > 0.0f);
    }
}

// The builtin floor's triangles are wound CCW-from-above (standard glTF) so the winding
// cross-product agrees with the +Y stored normal; the engine front-faces this (frontFace=CCW),
// rendering the top surface. Inverted winding would render the plane only from below. Regression
// guard for the inside-out winding class of bug.
TEST_CASE("Builtin floor plane is wound front-face up") {
    const std::span<const uint8_t> glb = builtinFloorPlaneGlb();
    REQUIRE(glb.size() > 20u);

    auto readU32 = [&](std::size_t off) {
        uint32_t v = 0;
        std::memcpy(&v, glb.data() + off, 4);
        return v;
    };
    auto readVec3 = [&](std::size_t off) {
        float x, y, z;
        std::memcpy(&x, glb.data() + off + 0, 4);
        std::memcpy(&y, glb.data() + off + 4, 4);
        std::memcpy(&z, glb.data() + off + 8, 4);
        return glm::vec3{x, y, z};
    };

    // BIN layout (build_floor_plane): 4 POSITION vec3 (48 B), 4 NORMAL vec3 (48 B),
    // then 6 uint16 indices.
    const uint32_t jsonLen = readU32(12);
    const std::size_t binStart = 12u + 8u + jsonLen + 8u;
    constexpr int kVerts = 4;
    const std::size_t idxBase = binStart + static_cast<std::size_t>(kVerts) * 12u * 2u;
    REQUIRE(glb.size() >= idxBase + 6u * sizeof(uint16_t));

    auto idx = [&](int i) {
        uint16_t v = 0;
        std::memcpy(&v, glb.data() + idxBase + static_cast<std::size_t>(i) * sizeof(uint16_t), sizeof(uint16_t));
        return static_cast<std::size_t>(v);
    };
    const glm::vec3 a = readVec3(binStart + idx(0) * 12u);
    const glm::vec3 b = readVec3(binStart + idx(1) * 12u);
    const glm::vec3 c = readVec3(binStart + idx(2) * 12u);
    // Winding cross-product of the first triangle points UP (+Y), agreeing with the stored
    // normal (standard CCW-from-above) so the engine front-faces the top surface.
    const glm::vec3 windingCross = glm::cross(b - a, c - a);
    CHECK(windingCross.y > 0.0f);
    // The stored NORMAL (first vertex) also points up (+Y) for lighting.
    const std::size_t normBase = binStart + static_cast<std::size_t>(kVerts) * 12u;
    const glm::vec3 storedNormal = readVec3(normBase);
    CHECK(storedNormal.y > 0.0f);
}
