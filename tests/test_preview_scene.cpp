// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "render/BuiltinGeometry.h"
#include "render/PreviewScene.h"

#include "content/AssetManager.h"
#include "content/AssetTypes.h"
#include "content/IContentPack.h"

#include "mock_content.h"
#include "mock_hal.h"

#include <cmath>
#include <glm/glm.hpp>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

using namespace fl;

namespace {

// Serves meshes/textures from in-memory maps; everything else is null-object (mock_content.h).
struct PreviewMockPack : NullContentPack {
    std::unordered_map<std::string, std::vector<uint8_t>> meshes;
    std::unordered_map<std::string, std::vector<uint8_t>> textures;

    const char* id() const override {
        return "test:preview";
    }
    bool hasAsset(const char* n, AssetType t) const override {
        if (t == AssetType::Mesh)
            return meshes.count(n) != 0;
        if (t == AssetType::Texture)
            return textures.count(n) != 0;
        return false;
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
    std::optional<TextureData> loadTexture(const char* n) override {
        auto it = textures.find(n);
        if (it == textures.end())
            return std::nullopt;
        TextureData d;
        d.name = n;
        d.bytes = it->second;
        return d;
    }
};

std::vector<uint8_t> glbBytes() {
    auto span = builtinShapeGlb(BuiltinShape::AirVehicle);
    return std::vector<uint8_t>(span.begin(), span.end());
}

} // namespace

// ---------------------------------------------------------------------------
// Pure framing math
// ---------------------------------------------------------------------------
TEST_CASE("frameBounds sizes distance to fit the bounding sphere", "[preview]") {
    // A 4 m cube centred at (0,0,0): radius = sqrt(3)*2 ~= 3.464 m.
    const glm::vec3 mn(-2.0f), mx(2.0f);
    const float fovY = 1.0472f; // 60 deg
    PreviewOrbit o = frameBounds(mn, mx, fovY, /*margin=*/1.0f);
    const float radius = 0.5f * glm::length(mx - mn);
    const float expected = radius / std::sin(fovY * 0.5f);
    CHECK(o.focus.x == Catch::Approx(0.0f));
    CHECK(o.focus.y == Catch::Approx(0.0f));
    CHECK(o.focus.z == Catch::Approx(0.0f));
    CHECK(o.distance == Catch::Approx(expected).epsilon(0.01));
}

TEST_CASE("frameBounds focuses on an off-centre box", "[preview]") {
    PreviewOrbit o = frameBounds(glm::vec3(8.0f, 0.0f, 0.0f), glm::vec3(12.0f, 2.0f, 2.0f));
    CHECK(o.focus.x == Catch::Approx(10.0f));
    CHECK(o.focus.y == Catch::Approx(1.0f));
    CHECK(o.focus.z == Catch::Approx(1.0f));
    CHECK(o.distance > 0.0f);
}

TEST_CASE("frameBounds falls back for a degenerate box", "[preview]") {
    PreviewOrbit o = frameBounds(glm::vec3(1.0f), glm::vec3(-1.0f)); // min > max
    CHECK(o.distance == Catch::Approx(5.0f));
}

TEST_CASE("previewCameraView places the eye at distance and looks at the focus", "[preview]") {
    PreviewOrbit o;
    o.focus = glm::vec3(0.0f);
    o.yawDeg = 0.0f;
    o.pitchDeg = 0.0f;
    o.distance = 10.0f;
    CameraView cv = previewCameraView(o, 16.0f / 9.0f);
    // yaw 0 / pitch 0 -> eye on -X at distance 10.
    CHECK(cv.worldOrigin.x == Catch::Approx(-10.0).epsilon(0.001));
    CHECK(cv.worldOrigin.y == Catch::Approx(0.0).margin(0.001));
    CHECK(cv.worldOrigin.z == Catch::Approx(0.0).margin(0.001));
}

// ---------------------------------------------------------------------------
// Loading + rendering
// ---------------------------------------------------------------------------
TEST_CASE("PreviewScene loads a content mesh and frames it", "[preview]") {
    auto pack = std::make_unique<PreviewMockPack>();
    pack->meshes["f5e"] = glbBytes();
    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.push_back(std::move(pack));
    MockLogger logger;
    AssetManager assets{std::move(packs), logger};
    assets.initialize(nullptr);

    MockRenderer renderer;
    renderer.meshBoundsAvailable = true;
    renderer.meshBoundsMin = glm::vec3(-3.0f, -1.0f, -4.0f);
    renderer.meshBoundsMax = glm::vec3(3.0f, 1.0f, 4.0f);

    PreviewScene preview(renderer, &assets, &logger);
    PreviewScene::ModelDesc desc;
    desc.meshAssetName = "f5e";
    CHECK(preview.load(desc));
    CHECK(preview.loadedFromContent());
    CHECK(renderer.createMeshCount == 1);
    CHECK(preview.boundsMin().z == Catch::Approx(-4.0f));
    CHECK(preview.boundsMax().z == Catch::Approx(4.0f));

    auto items = preview.buildItems(glm::dvec3(0.0));
    REQUIRE(items.size() == 1);
    CHECK(items[0].mesh.valid());
}

TEST_CASE("PreviewScene falls back to the builtin placeholder when the mesh is missing", "[preview]") {
    auto pack = std::make_unique<PreviewMockPack>(); // empty — no meshes
    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.push_back(std::move(pack));
    MockLogger logger;
    AssetManager assets{std::move(packs), logger};
    assets.initialize(nullptr);

    MockRenderer renderer;
    renderer.meshBoundsAvailable = true;
    renderer.meshBoundsMin = glm::vec3(-1.0f);
    renderer.meshBoundsMax = glm::vec3(1.0f);

    PreviewScene preview(renderer, &assets, &logger);
    PreviewScene::ModelDesc desc;
    desc.meshAssetName = "does-not-exist";
    CHECK_FALSE(preview.load(desc)); // returns false = builtin fallback
    CHECK_FALSE(preview.loadedFromContent());
    // Still a drawable item (the placeholder) + a material.
    auto items = preview.buildItems(glm::dvec3(0.0));
    REQUIRE(items.size() == 1);
    CHECK(items[0].mesh.valid());
    CHECK(items[0].material.valid());
}

TEST_CASE("PreviewScene swaps to the damage mesh when set", "[preview]") {
    auto pack = std::make_unique<PreviewMockPack>();
    pack->meshes["f5e"] = glbBytes();
    pack->meshes["f5e_dmg"] = glbBytes();
    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.push_back(std::move(pack));
    MockLogger logger;
    AssetManager assets{std::move(packs), logger};
    assets.initialize(nullptr);

    MockRenderer renderer;
    renderer.meshBoundsAvailable = true;
    renderer.meshBoundsMax = glm::vec3(1.0f);

    PreviewScene preview(renderer, &assets, &logger);
    PreviewScene::ModelDesc desc;
    desc.meshAssetName = "f5e";
    desc.damageMeshAssetName = "f5e_dmg";
    REQUIRE(preview.load(desc));
    CHECK(preview.hasDamageMesh());
    CHECK(renderer.createMeshCount == 2); // primary + damage

    const MeshHandle intact = preview.buildItems(glm::dvec3(0.0))[0].mesh;
    preview.setDamaged(true);
    auto dmgItems = preview.buildItems(glm::dvec3(0.0));
    REQUIRE(dmgItems.size() == 1);
    CHECK(dmgItems[0].mesh.id != intact.id);         // a different (damage) mesh
    CHECK((dmgItems[0].flags & kRenderFlagDamaged)); // flagged damaged
}

TEST_CASE("PreviewScene face-color view sets the debug flag", "[preview]") {
    auto pack = std::make_unique<PreviewMockPack>();
    pack->meshes["f5e"] = glbBytes();
    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.push_back(std::move(pack));
    MockLogger logger;
    AssetManager assets{std::move(packs), logger};
    assets.initialize(nullptr);

    MockRenderer renderer;
    renderer.meshBoundsAvailable = true;
    renderer.meshBoundsMax = glm::vec3(1.0f);

    PreviewScene preview(renderer, &assets, &logger);
    PreviewScene::ModelDesc desc;
    desc.meshAssetName = "f5e";
    REQUIRE(preview.load(desc));

    preview.setDebugView(PreviewDebugView::FaceColor);
    auto items = preview.buildItems(glm::dvec3(0.0));
    REQUIRE(items.size() == 1);
    CHECK((items[0].flags & kRenderFlagDebugFaceColor));
}
