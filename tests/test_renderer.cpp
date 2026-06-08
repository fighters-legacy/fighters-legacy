// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "RenderTypes.h"
#include "mock_hal.h"
#include <array>

// ---------------------------------------------------------------------------
// RenderTypes — handle validity
// ---------------------------------------------------------------------------

TEST_CASE("resource handles default to invalid") {
    CHECK_FALSE(MeshHandle{}.valid());
    CHECK_FALSE(TextureHandle{}.valid());
    CHECK_FALSE(MaterialHandle{}.valid());
}

TEST_CASE("resource handles with non-zero id are valid") {
    CHECK(MeshHandle{1}.valid());
    CHECK(TextureHandle{42}.valid());
    CHECK(MaterialHandle{std::numeric_limits<uint32_t>::max()}.valid());
}

// ---------------------------------------------------------------------------
// RenderTypes — scene types
// ---------------------------------------------------------------------------

TEST_CASE("FrameScene defaults to empty spans") {
    FrameScene scene{};
    CHECK(scene.renderItems.empty());
    CHECK(scene.particleEmitters.empty());
}

TEST_CASE("RenderItem defaults") {
    RenderItem item{};
    CHECK_FALSE(item.mesh.valid());
    CHECK_FALSE(item.material.valid());
    CHECK(item.lod == 0);
    CHECK(item.flags == 0);
    CHECK(item.animPoses.empty());
}

TEST_CASE("kRenderFlag constants are distinct single-bit values") {
    CHECK(kRenderFlagDamaged != kRenderFlagShadowOnly);
    CHECK((kRenderFlagDamaged & kRenderFlagShadowOnly) == 0);
}

TEST_CASE("FrameScene holds span of RenderItems") {
    std::array<RenderItem, 3> items{};
    items[0].mesh = MeshHandle{1};
    items[1].mesh = MeshHandle{2};
    items[2].mesh = MeshHandle{3};

    FrameScene scene{};
    scene.renderItems = items;
    REQUIRE(scene.renderItems.size() == 3);
    CHECK(scene.renderItems[0].mesh.id == 1);
    CHECK(scene.renderItems[2].mesh.id == 3);
}

// ---------------------------------------------------------------------------
// MockRenderer — lifecycle
// ---------------------------------------------------------------------------

TEST_CASE("MockRenderer lifecycle counters start at zero") {
    MockRenderer r;
    CHECK(r.initCount == 0);
    CHECK(r.beginFrameCount == 0);
    CHECK(r.endFrameCount == 0);
    CHECK(r.shutdownCount == 0);
}

TEST_CASE("MockRenderer init increments counter and returns initResult") {
    MockRenderer r;
    CHECK(r.init(nullptr) == true);
    CHECK(r.initCount == 1);

    r.initResult = false;
    CHECK(r.init(nullptr) == false);
    CHECK(r.initCount == 2);
}

TEST_CASE("MockRenderer beginFrame and endFrame") {
    MockRenderer r;
    r.beginFrame();
    r.beginFrame();
    r.endFrame();
    CHECK(r.beginFrameCount == 2);
    CHECK(r.endFrameCount == 1);
}

TEST_CASE("MockRenderer shutdown") {
    MockRenderer r;
    r.shutdown();
    CHECK(r.shutdownCount == 1);
}

TEST_CASE("MockRenderer onResize records last dimensions") {
    MockRenderer r;
    r.onResize(1920, 1080);
    CHECK(r.resizeCount == 1);
    CHECK(r.lastResizeW == 1920);
    CHECK(r.lastResizeH == 1080);

    r.onResize(2560, 1440);
    CHECK(r.resizeCount == 2);
    CHECK(r.lastResizeW == 2560);
}

TEST_CASE("MockRenderer getLastError returns nullptr when no error") {
    MockRenderer r;
    CHECK(r.getLastError() == nullptr);
}

TEST_CASE("MockRenderer getLastError returns message when set") {
    MockRenderer r;
    r.lastErrorBuf = "test error";
    REQUIRE(r.getLastError() != nullptr);
    CHECK(std::string(r.getLastError()) == "test error");
}

TEST_CASE("MockRenderer gpuInfo returns non-null string") {
    MockRenderer r;
    const char* info = r.gpuInfo();
    REQUIRE(info != nullptr);
    CHECK(std::string(info).size() > 0);
}

// ---------------------------------------------------------------------------
// MockRenderer via IRenderer pointer (interface compliance)
// ---------------------------------------------------------------------------

TEST_CASE("MockRenderer is usable through IRenderer pointer") {
    MockRenderer mock;
    IRenderer* r = &mock;

    r->init(nullptr);
    r->onResize(800, 600);
    r->beginFrame();
    r->endFrame();
    r->shutdown();

    CHECK(mock.initCount == 1);
    CHECK(mock.resizeCount == 1);
    CHECK(mock.beginFrameCount == 1);
    CHECK(mock.endFrameCount == 1);
    CHECK(mock.shutdownCount == 1);
}

// ---------------------------------------------------------------------------
// MockRenderer — resource creation
// ---------------------------------------------------------------------------

TEST_CASE("MockRenderer createMesh returns valid distinct handles") {
    MockRenderer r;
    auto h1 = r.createMesh(MeshUploadDesc{});
    auto h2 = r.createMesh(MeshUploadDesc{});
    CHECK(h1.valid());
    CHECK(h2.valid());
    CHECK(h1.id != h2.id);
    CHECK(r.createMeshCount == 2);
}

TEST_CASE("MockRenderer createTexture returns valid distinct handles") {
    MockRenderer r;
    auto h1 = r.createTexture(TextureUploadDesc{});
    auto h2 = r.createTexture(TextureUploadDesc{});
    CHECK(h1.valid());
    CHECK(h2.valid());
    CHECK(h1.id != h2.id);
    CHECK(r.createTextureCount == 2);
}

TEST_CASE("MockRenderer createMaterial returns valid distinct handles") {
    MockRenderer r;
    auto h1 = r.createMaterial(MaterialDesc{});
    auto h2 = r.createMaterial(MaterialDesc{});
    CHECK(h1.valid());
    CHECK(h2.valid());
    CHECK(h1.id != h2.id);
    CHECK(r.createMaterialCount == 2);
}

TEST_CASE("MockRenderer destroy methods increment counters") {
    MockRenderer r;
    auto mh = r.createMesh(MeshUploadDesc{});
    auto th = r.createTexture(TextureUploadDesc{});
    auto mth = r.createMaterial(MaterialDesc{});

    r.destroyMesh(mh);
    r.destroyTexture(th);
    r.destroyMaterial(mth);

    CHECK(r.destroyMeshCount == 1);
    CHECK(r.destroyTextureCount == 1);
    CHECK(r.destroyMaterialCount == 1);
}

TEST_CASE("MockRenderer setScene stores last scene and increments counter") {
    MockRenderer r;
    auto mh = r.createMesh(MeshUploadDesc{});

    std::array<RenderItem, 2> items{};
    items[0].mesh = mh;
    items[1].mesh = MeshHandle{99};

    FrameScene scene{};
    scene.renderItems = items;

    r.beginFrame();
    r.setScene(scene);
    r.endFrame();

    CHECK(r.setSceneCount == 1);
    REQUIRE(r.lastScene.renderItems.size() == 2);
    CHECK(r.lastScene.renderItems[0].mesh.id == mh.id);
}

TEST_CASE("MockRenderer resource counters start at zero") {
    MockRenderer r;
    CHECK(r.createMeshCount == 0);
    CHECK(r.createTextureCount == 0);
    CHECK(r.createMaterialCount == 0);
    CHECK(r.destroyMeshCount == 0);
    CHECK(r.destroyTextureCount == 0);
    CHECK(r.destroyMaterialCount == 0);
    CHECK(r.setSceneCount == 0);
}

// ---------------------------------------------------------------------------
// MaterialDesc defaults
// ---------------------------------------------------------------------------

TEST_CASE("MaterialDesc defaults are identity PBR values") {
    MaterialDesc m{};
    CHECK_FALSE(m.baseColorTexture.valid());
    CHECK_FALSE(m.normalTexture.valid());
    CHECK_FALSE(m.ormTexture.valid());
    CHECK(m.baseColorFactor == glm::vec4(1.0f));
    CHECK(m.metallicFactor == 0.0f);
    CHECK(m.roughnessFactor == 1.0f);
    CHECK_FALSE(m.doubleSided);
    CHECK_FALSE(m.alphaBlend);
}

// ---------------------------------------------------------------------------
// Upload descriptor defaults
// ---------------------------------------------------------------------------

TEST_CASE("MeshUploadDesc defaults to empty") {
    MeshUploadDesc d{};
    CHECK(d.name.empty());
    CHECK(d.bytes.empty());
}

TEST_CASE("TextureUploadDesc defaults to srgb=true") {
    TextureUploadDesc d{};
    CHECK(d.srgb == true);
    CHECK(d.bytes.empty());
}

// ---------------------------------------------------------------------------
// EnvironmentState defaults and setScene integration
// ---------------------------------------------------------------------------

TEST_CASE("EnvironmentState defaults") {
    EnvironmentState env{};
    CHECK(env.sunDirection.y == -1.0f);
    CHECK(env.fogDensity == 0.0f);
    CHECK(env.timeOfDay == 12.0f);
    CHECK(env.windX == 0.0f);
    CHECK(env.windZ == 0.0f);
}

TEST_CASE("setScene stores EnvironmentState in lastScene") {
    MockRenderer r;

    FrameScene scene{};
    scene.environment.sunDirection = glm::vec3(0.5f, -0.8f, 0.2f);
    scene.environment.sunColor = glm::vec3(1.0f, 0.9f, 0.7f);
    scene.environment.ambientColor = glm::vec3(0.05f, 0.06f, 0.08f);
    scene.environment.timeOfDay = 8.5f;

    r.beginFrame();
    r.setScene(scene);
    r.endFrame();

    CHECK(r.lastScene.environment.sunDirection == glm::vec3(0.5f, -0.8f, 0.2f));
    CHECK(r.lastScene.environment.sunColor == glm::vec3(1.0f, 0.9f, 0.7f));
    CHECK(r.lastScene.environment.timeOfDay == 8.5f);
}

TEST_CASE("RenderItem kRenderFlagShadowOnly is set independently of kRenderFlagDamaged") {
    RenderItem item{};
    item.flags = kRenderFlagShadowOnly;
    CHECK((item.flags & kRenderFlagShadowOnly) != 0);
    CHECK((item.flags & kRenderFlagDamaged) == 0);
}

TEST_CASE("setScene with multiple RenderItems preserves all flags") {
    MockRenderer r;
    std::array<RenderItem, 2> items{};
    items[0].flags = kRenderFlagDamaged;
    items[1].flags = kRenderFlagShadowOnly;

    FrameScene scene{};
    scene.renderItems = items;

    r.beginFrame();
    r.setScene(scene);
    r.endFrame();

    REQUIRE(r.lastScene.renderItems.size() == 2);
    CHECK(r.lastScene.renderItems[0].flags == kRenderFlagDamaged);
    CHECK(r.lastScene.renderItems[1].flags == kRenderFlagShadowOnly);
}
