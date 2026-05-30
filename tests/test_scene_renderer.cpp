// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "render/CameraController.h"
#include "render/RenderSnapshot.h"
#include "render/SceneRenderer.h"
#include "render/SimRenderBridge.h"

#include "content/AssetManager.h"
#include "content/AssetTypes.h"
#include "content/IContentPack.h"

#include "mock_hal.h"

#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace fl;

// ---------------------------------------------------------------------------
// Minimal mock content pack for asset loading tests
// ---------------------------------------------------------------------------

struct MockContentPack : public IContentPack {
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
    int priority() const override {
        return 0;
    }
    const char* rootDirectory() const override {
        return nullptr;
    }
    Status init() override {
        return Status::Ready;
    }
    bool configure(IWindow*) override {
        return true;
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
    std::optional<TextureData> loadTexture(const char*) override {
        return std::nullopt;
    }
    std::optional<AudioBuffer> loadAudio(const char*) override {
        return std::nullopt;
    }
    std::optional<FlightModel> loadFlightModel(const char*) override {
        return std::nullopt;
    }
    std::optional<MissionData> loadMission(const char*) override {
        return std::nullopt;
    }
    std::optional<TerrainData> loadTerrain(const char*) override {
        return std::nullopt;
    }
    std::optional<AIScript> loadAIScript(const char*) override {
        return std::nullopt;
    }
    std::optional<EntityDefData> loadEntityDef(const char*) override {
        return std::nullopt;
    }
    std::vector<std::string> listAssets(AssetType) const override {
        return {};
    }
    std::optional<std::string> loadConfig(const char*) const override {
        return std::nullopt;
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

static EntityRenderEntry makeEntry(uint32_t typeIndex = 0, glm::vec3 pos = {}) {
    EntityRenderEntry e;
    e.typeIndex = typeIndex;
    e.position = pos;
    e.orientation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    return e;
}

// ---------------------------------------------------------------------------
// CameraController — Free mode
// ---------------------------------------------------------------------------

TEST_CASE("CameraController defaults to Free mode") {
    CameraController cam;
    CHECK(cam.mode() == CameraMode::Free);
}

TEST_CASE("CameraController Free mode worldOrigin is nonzero at default distance") {
    CameraController cam;
    CameraView cv = cam.view(16.0f / 9.0f);
    // Default distance=50m, pitch=20 deg: camera should not be at origin.
    float len = std::sqrt(cv.worldOrigin.x * cv.worldOrigin.x + cv.worldOrigin.y * cv.worldOrigin.y +
                          cv.worldOrigin.z * cv.worldOrigin.z);
    CHECK(len > 1.0f);
}

TEST_CASE("CameraController Free mode projection encodes near plane in proj[3][2]") {
    CameraController cam;
    float near = 0.5f;
    CameraView cv = cam.view(1.0f, 1.0472f, near);
    // Infinite reverse-Z: proj[3][2] == near.
    CHECK(cv.proj[3][2] == Catch::Approx(near));
}

TEST_CASE("CameraController Free mode projection flips Y for Vulkan") {
    CameraController cam;
    CameraView cv = cam.view(1.0f);
    // proj[1][1] is negative (Vulkan Y-flip).
    CHECK(cv.proj[1][1] < 0.0f);
}

TEST_CASE("CameraController Free mode proj[2][3] is -1 for RH perspective") {
    CameraController cam;
    CameraView cv = cam.view(1.0f);
    CHECK(cv.proj[2][3] == Catch::Approx(-1.0f));
}

TEST_CASE("CameraController setFreeOrbit repositions camera") {
    CameraController cam;
    cam.setFreeOrbit({0, 0, 0}, 0.0f, 0.0f, 10.0f);
    CameraView cv = cam.view(1.0f);
    // yaw=0, pitch=0, dist=10 => cam at (0, 0, 10).
    CHECK(cv.worldOrigin.x == Catch::Approx(0.0f).margin(1e-4f));
    CHECK(cv.worldOrigin.y == Catch::Approx(0.0f).margin(1e-4f));
    CHECK(cv.worldOrigin.z == Catch::Approx(10.0f).margin(1e-3f));
}

TEST_CASE("CameraController setMode changes mode") {
    CameraController cam;
    cam.setMode(CameraMode::Chase);
    CHECK(cam.mode() == CameraMode::Chase);
    cam.setMode(CameraMode::Free);
    CHECK(cam.mode() == CameraMode::Free);
}

TEST_CASE("CameraController Chase mode worldOrigin differs from target position") {
    CameraController cam;
    cam.setMode(CameraMode::Chase);
    glm::vec3 targetPos{100.0f, 50.0f, 200.0f};
    cam.setTarget(targetPos, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    CameraView cv = cam.view(16.0f / 9.0f);
    // Camera should not be at the target position.
    float dx = cv.worldOrigin.x - targetPos.x;
    float dy = cv.worldOrigin.y - targetPos.y;
    float dz = cv.worldOrigin.z - targetPos.z;
    float dist2 = dx * dx + dy * dy + dz * dz;
    CHECK(dist2 > 1.0f);
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
    pack->meshes["f15c"] = {1, 2, 3, 4};

    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.push_back(std::move(pack));
    AssetManager assets{std::move(packs), logger};
    assets.initialize(nullptr);

    MockRenderer renderer;
    SimRenderBridge bridge;
    SceneRenderer sr{bridge, oneType(), assets, renderer};

    RenderSnapshot snap = makeSnap();
    snap.entries.push_back(makeEntry(0, {10.0f, 0.0f, 0.0f}));
    bridge.publish(std::move(snap));

    sr.renderFrame(0.0f, CameraView{}, EnvironmentState{});

    REQUIRE(renderer.setSceneCount == 1);
    REQUIRE(renderer.lastScene.renderItems.size() == 1);
    CHECK(renderer.lastScene.renderItems[0].mesh.valid());
    // ensureBuiltins() uploads 2 meshes (entity + floor) + 7 materials (6 palette + 1 floor);
    // the content mesh adds 1 more createMesh and 1 more createMaterial.
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
    pack->meshes["f15c"] = {1, 2, 3};
    pack->meshes["f15c_damaged"] = {4, 5, 6};

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
    pack->meshes["f15c"] = {1, 2, 3};

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
    pack->meshes["f15c"] = {1, 2, 3};

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

    // ensureBuiltins on frame 1: 2 meshes + 7 materials. Content "f15c": +1 each. Frame 2: cached.
    CHECK(renderer.createMeshCount == 3);
    CHECK(renderer.createMaterialCount == 8);
    CHECK(renderer.setSceneCount == 2);
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
    cam.worldOrigin = {10.0f, 20.0f, 30.0f};

    EnvironmentState env;
    env.sunDirection = {0.0f, -0.8f, 0.6f};
    env.timeOfDay = 7.5f;

    sr.renderFrame(0.0f, cam, env);

    CHECK(renderer.lastScene.camera.worldOrigin == glm::vec3(10.0f, 20.0f, 30.0f));
    CHECK(renderer.lastScene.environment.sunDirection == glm::vec3(0.0f, -0.8f, 0.6f));
    CHECK(renderer.lastScene.environment.timeOfDay == Catch::Approx(7.5f));
}

// ---------------------------------------------------------------------------
// SceneRenderer — velocity extrapolation
// ---------------------------------------------------------------------------

TEST_CASE("SceneRenderer applies velocity extrapolation to transform position") {
    MockLogger logger;
    auto pack = std::make_unique<MockContentPack>();
    pack->meshes["f15c"] = {1, 2, 3};

    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.push_back(std::move(pack));
    AssetManager assets{std::move(packs), logger};
    assets.initialize(nullptr);

    MockRenderer renderer;
    SimRenderBridge bridge;
    SceneRenderer sr{bridge, oneType(), assets, renderer};

    EntityRenderEntry e = makeEntry(0, {0.0f, 0.0f, 0.0f});
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
    pack->meshes["f15c"] = {1, 2, 3};

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
    snap.entries.push_back(makeEntry(0, {11000.0f, 0.0f, 0.0f}));
    bridge.publish(std::move(snap));

    sr.renderFrame(0.0f, CameraView{}, EnvironmentState{});

    CHECK(renderer.setSceneCount == 1);
    CHECK(renderer.lastScene.renderItems.empty()); // culled
}

TEST_CASE("SceneRenderer keeps entity within draw distance") {
    MockLogger logger;
    auto pack = std::make_unique<MockContentPack>();
    pack->meshes["f15c"] = {1, 2, 3};

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
    snap.entries.push_back(makeEntry(0, {9000.0f, 0.0f, 0.0f}));
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
    rs.antiAliasing = false;
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

TEST_CASE("SceneRenderer assigns distinct palette materials by entityIdx") {
    MockLogger logger;
    std::vector<std::unique_ptr<IContentPack>> packs;
    AssetManager assets{std::move(packs), logger};
    assets.initialize(nullptr);

    MockRenderer renderer;
    SimRenderBridge bridge;
    SceneRenderer sr{bridge, noTypes(), assets, renderer};

    EntityRenderEntry e0 = makeEntry(0, {0.0f, 0.0f, 0.0f});
    e0.entityIdx = 0;
    EntityRenderEntry e1 = makeEntry(0, {1.0f, 0.0f, 0.0f});
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
