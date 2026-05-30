// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "render/ParticleSystem.h"
#include "render/RenderSnapshot.h"
#include "render/SceneRenderer.h"
#include "render/SimRenderBridge.h"

#include "content/AssetManager.h"
#include "content/AssetTypes.h"
#include "content/IContentPack.h"

#include "mock_hal.h"

#include <optional>
#include <string>
#include <vector>

using namespace fl;

// ---------------------------------------------------------------------------
// Minimal mock content pack (no assets needed for particle tests)
// ---------------------------------------------------------------------------

struct EmptyContentPack : public IContentPack {
    const char* name() const override {
        return "Empty";
    }
    const char* version() const override {
        return "0.0.1";
    }
    const char* id() const override {
        return "test:empty";
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
    bool hasAsset(const char*, AssetType) const override {
        return false;
    }
    std::optional<MeshData> loadMesh(const char*) override {
        return std::nullopt;
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
// ParticleSystem unit tests
// ---------------------------------------------------------------------------

TEST_CASE("ParticleSystem: emit unknown preset ignored") {
    ParticleSystem ps;
    ps.emit("no_such_preset", {0.0f, 0.0f, 0.0f});
    REQUIRE(ps.emitters().empty());
}

TEST_CASE("ParticleSystem: emit nullptr preset ignored") {
    ParticleSystem ps;
    ps.emit(nullptr, {0.0f, 0.0f, 0.0f});
    REQUIRE(ps.emitters().empty());
}

TEST_CASE("ParticleSystem: register then emit produces one emitter") {
    ParticleSystem ps;
    ParticlePreset preset{};
    preset.spawnRate = 100.0f;
    ps.registerPreset("explosion", preset);

    ps.emit("explosion", {1.0f, 2.0f, 3.0f});

    REQUIRE(ps.emitters().size() == 1);
    CHECK(ps.emitters()[0].position.x == Catch::Approx(1.0f));
    CHECK(ps.emitters()[0].position.y == Catch::Approx(2.0f));
    CHECK(ps.emitters()[0].position.z == Catch::Approx(3.0f));
}

TEST_CASE("ParticleSystem: preset parameters copied to emitter state") {
    ParticlePreset preset{};
    preset.spawnRate = 200.0f;
    preset.particleLifetime = 3.5f;
    preset.initialSpeed = 12.0f;
    preset.colorStart = {0.9f, 0.4f, 0.1f};
    preset.colorEnd = {0.2f, 0.2f, 0.2f};
    preset.sizeStart = 0.3f;
    preset.sizeEnd = 2.5f;
    preset.additive = true;

    ParticleSystem ps;
    ps.registerPreset("fire", preset);
    ps.emit("fire", {}, 0.75f);

    REQUIRE(ps.emitters().size() == 1);
    const auto& e = ps.emitters()[0];
    CHECK(e.effectName == std::string_view("fire"));
    CHECK(e.intensity == Catch::Approx(0.75f));
    CHECK(e.spawnRate == Catch::Approx(200.0f));
    CHECK(e.particleLifetime == Catch::Approx(3.5f));
    CHECK(e.initialSpeed == Catch::Approx(12.0f));
    CHECK(e.colorStart.r == Catch::Approx(0.9f));
    CHECK(e.colorEnd.g == Catch::Approx(0.2f));
    CHECK(e.sizeStart == Catch::Approx(0.3f));
    CHECK(e.sizeEnd == Catch::Approx(2.5f));
    CHECK(e.additive == true);
}

TEST_CASE("ParticleSystem: smoke preset has additive=false") {
    ParticlePreset smokePreset{};
    smokePreset.additive = false;

    ParticleSystem ps;
    ps.registerPreset("smoke", smokePreset);
    ps.emit("smoke", {});

    REQUIRE(ps.emitters().size() == 1);
    CHECK(ps.emitters()[0].additive == false);
}

TEST_CASE("ParticleSystem: multiple emit calls produce multiple emitters") {
    ParticlePreset preset{};
    ParticleSystem ps;
    ps.registerPreset("fx", preset);

    ps.emit("fx", {0.0f, 0.0f, 0.0f});
    ps.emit("fx", {1.0f, 0.0f, 0.0f});
    ps.emit("fx", {2.0f, 0.0f, 0.0f});

    REQUIRE(ps.emitters().size() == 3);
    CHECK(ps.emitters()[2].position.x == Catch::Approx(2.0f));
}

TEST_CASE("ParticleSystem: reset clears emitters") {
    ParticlePreset preset{};
    ParticleSystem ps;
    ps.registerPreset("fx", preset);
    ps.emit("fx", {});
    REQUIRE(!ps.emitters().empty());

    ps.reset();
    CHECK(ps.emitters().empty());
}

TEST_CASE("ParticleSystem: reset then emit gives fresh list") {
    ParticlePreset preset{};
    ParticleSystem ps;
    ps.registerPreset("fx", preset);
    ps.emit("fx", {0.0f, 0.0f, 0.0f});
    ps.reset();
    ps.emit("fx", {5.0f, 5.0f, 5.0f});

    REQUIRE(ps.emitters().size() == 1);
    CHECK(ps.emitters()[0].position.x == Catch::Approx(5.0f));
}

TEST_CASE("ParticleSystem: overwriting preset replaces parameters") {
    ParticleSystem ps;

    ParticlePreset v1{};
    v1.spawnRate = 10.0f;
    ps.registerPreset("fx", v1);

    ParticlePreset v2{};
    v2.spawnRate = 99.0f;
    ps.registerPreset("fx", v2); // overwrite

    ps.emit("fx", {});
    REQUIRE(ps.emitters().size() == 1);
    CHECK(ps.emitters()[0].spawnRate == Catch::Approx(99.0f));
}

// ---------------------------------------------------------------------------
// SceneRenderer + ParticleSystem integration
// ---------------------------------------------------------------------------

static fl::RenderSnapshot makeSnap(uint64_t tick) {
    fl::RenderSnapshot s;
    s.tickIndex = tick;
    return s;
}

static fl::EntityRenderEntry makeEntry(uint32_t typeIndex, glm::vec3 pos, uint8_t damage = 0) {
    fl::EntityRenderEntry e;
    e.typeIndex = typeIndex;
    e.position = pos;
    e.damageLevel = damage;
    e.orientation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    return e;
}

TEST_CASE("SceneRenderer: no particle system -- particleEmitters empty in scene") {
    MockRenderer renderer;
    SimRenderBridge bridge;
    auto pack = std::make_unique<EmptyContentPack>();
    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.push_back(std::move(pack));
    MockLogger logger;
    AssetManager assets(std::move(packs), logger);

    SceneRenderer sr{bridge, [](uint32_t, std::string&, std::string&) { return false; }, assets, renderer};

    // Publish a snapshot with one damaged entity.
    auto snap = makeSnap(1);
    snap.entries.push_back(makeEntry(0, {0.0f, 0.0f, 0.0f}, 2)); // damageLevel=2
    bridge.publish(std::move(snap));

    sr.renderFrame(0.0f, CameraView{}, EnvironmentState{});

    REQUIRE(renderer.setSceneCount == 1);
    CHECK(renderer.lastScene.particleEmitters.empty());
}

TEST_CASE("SceneRenderer: with particle system -- damaged entity emits effect") {
    MockRenderer renderer;
    SimRenderBridge bridge;
    auto pack = std::make_unique<EmptyContentPack>();
    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.push_back(std::move(pack));
    MockLogger logger;
    AssetManager assets(std::move(packs), logger);

    SceneRenderer sr{bridge, [](uint32_t, std::string&, std::string&) { return false; }, assets, renderer};

    ParticleSystem ps;
    ParticlePreset preset{};
    preset.spawnRate = 50.0f;
    ps.registerPreset("explosion", preset);

    // EffectResolver: type 0, damageLevel 2 (Heavy) → "explosion".
    sr.setParticleSystem(&ps, [](uint32_t typeIndex, uint8_t damageLevel) -> std::string {
        if (typeIndex == 0 && damageLevel == 2)
            return "explosion";
        return {};
    });

    auto snap = makeSnap(1);
    snap.entries.push_back(makeEntry(0, {10.0f, 0.0f, 0.0f}, 2));
    bridge.publish(std::move(snap));

    sr.renderFrame(0.0f, CameraView{}, EnvironmentState{});

    // ParticleSystem::m_emitters is alive; span in lastScene points into it.
    REQUIRE(renderer.setSceneCount == 1);
    REQUIRE(renderer.lastScene.particleEmitters.size() == 1);
    CHECK(renderer.lastScene.particleEmitters[0].spawnRate == Catch::Approx(50.0f));
}

TEST_CASE("SceneRenderer: intact entity does not emit particle effect") {
    MockRenderer renderer;
    SimRenderBridge bridge;
    auto pack = std::make_unique<EmptyContentPack>();
    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.push_back(std::move(pack));
    MockLogger logger;
    AssetManager assets(std::move(packs), logger);

    SceneRenderer sr{bridge, [](uint32_t, std::string&, std::string&) { return false; }, assets, renderer};

    ParticleSystem ps;
    ParticlePreset preset{};
    ps.registerPreset("explosion", preset);
    sr.setParticleSystem(&ps, [](uint32_t, uint8_t) -> std::string { return "explosion"; });

    auto snap = makeSnap(1);
    snap.entries.push_back(makeEntry(0, {}, 0)); // damageLevel=0 = Intact
    bridge.publish(std::move(snap));

    sr.renderFrame(0.0f, CameraView{}, EnvironmentState{});

    REQUIRE(renderer.setSceneCount == 1);
    CHECK(renderer.lastScene.particleEmitters.empty());
}
