// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "RenderTypes.h"
#include "mock_hal.h"

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
