// SPDX-License-Identifier: GPL-3.0-or-later
//
// AssetManager pack resolution, validation and caching (#1145). test_content_system.cpp covers the
// happy path through one pack; this file is the multi-pack behaviour that makes modding work — which
// pack wins, what happens when one of them hands back rubbish, and what the cache does about it.
//
// The load path is a fall-through across packs with a validator in the middle, so every failure mode
// here has the same shape: the asset must come from the NEXT pack rather than the game breaking.

#include <catch2/catch_test_macros.hpp>

#include "content/AssetManager.h"
#include "mock_content.h"
#include "mock_hal.h"

#include <memory>
#include <string>
#include <vector>

using namespace fl;

namespace {

struct CountingLogger final : ILogger {
    std::vector<std::string> errors;
    void log(LogLevel lvl, const char*, int, const char* msg) override {
        if (lvl == LogLevel::Error && msg)
            errors.emplace_back(msg);
    }
    void setMinLevel(LogLevel) override {}
    void flush() override {}
    [[nodiscard]] bool mentions(std::string_view needle) const {
        for (const auto& e : errors)
            if (e.find(needle) != std::string::npos)
                return true;
        return false;
    }
};

// A pack that can be told exactly what to do: its identity, whether it needs configuration, and
// what bytes (if any) it hands back for a texture.
struct ScriptedPack final : public NullContentPack {
    std::string packId{"scripted"};
    std::string packVersion{"1.0"};
    Status initStatus{Status::Ready};
    bool configureOk{true};
    int initCalls{0};
    int configureCalls{0};
    int loadCalls{0};
    std::optional<std::vector<uint8_t>> textureBytes; // nullopt = "I don't have it"
    std::vector<std::string> assets;

    const char* id() const override {
        return packId.c_str();
    }
    const char* version() const override {
        return packVersion.c_str();
    }
    Status init() override {
        ++initCalls;
        return initStatus;
    }
    bool configure(IWindow*) override {
        ++configureCalls;
        return configureOk;
    }
    std::optional<TextureData> loadTexture(const char* name) override {
        ++loadCalls;
        lastRequested = name ? name : "";
        if (!textureBytes)
            return std::nullopt;
        TextureData d;
        d.bytes = *textureBytes;
        return d;
    }
    std::vector<std::string> listAssets(AssetType) const override {
        return assets;
    }
    std::string lastRequested;
};

// A KTX2 header the asset validator accepts, padded to a plausible size.
std::vector<uint8_t> validKtx2(std::size_t size = 256) {
    std::vector<uint8_t> b{0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
    b.resize(size, 0);
    return b;
}

std::unique_ptr<AssetManager> makeManager(std::vector<std::unique_ptr<IContentPack>> packs, ILogger& log) {
    return std::make_unique<AssetManager>(std::move(packs), log);
}

} // namespace

// ---------------------------------------------------------------------------
// initialize(): pack status handling
// ---------------------------------------------------------------------------

TEST_CASE("AssetManager: a pack needing configuration is configured when a window exists (#1145)",
          "[content][assets]") {
    CountingLogger log;
    MockWindow window;
    auto p = std::make_unique<ScriptedPack>();
    p->initStatus = IContentPack::Status::NeedsConfiguration;
    ScriptedPack* raw = p.get();

    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.push_back(std::move(p));
    auto am = makeManager(std::move(packs), log);
    am->initialize(&window);

    CHECK(raw->initCalls == 1);
    CHECK(raw->configureCalls == 1);
    CHECK(am->packManifest().size() == 1); // configured, so still active
}

TEST_CASE("AssetManager: a pack needing configuration is dropped headless (#1145)", "[content][assets]") {
    // configure() takes a window; without one there is nothing to configure against, so the pack
    // cannot be made usable and carrying it forward would mean asking a half-initialised pack for
    // assets. A headless server hits this path for every plugin pack.
    CountingLogger log;
    auto p = std::make_unique<ScriptedPack>();
    p->initStatus = IContentPack::Status::NeedsConfiguration;
    ScriptedPack* raw = p.get();

    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.push_back(std::move(p));
    auto am = makeManager(std::move(packs), log);
    am->initialize(nullptr);

    CHECK(raw->initCalls == 1);
    CHECK(raw->configureCalls == 0);
    CHECK(am->packManifest().empty());
}

TEST_CASE("AssetManager: a pack whose configuration fails is dropped (#1145)", "[content][assets]") {
    CountingLogger log;
    MockWindow window;
    auto bad = std::make_unique<ScriptedPack>();
    bad->packId = "bad";
    bad->initStatus = IContentPack::Status::NeedsConfiguration;
    bad->configureOk = false;
    auto good = std::make_unique<ScriptedPack>();
    good->packId = "good";

    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.push_back(std::move(bad));
    packs.push_back(std::move(good));
    auto am = makeManager(std::move(packs), log);
    am->initialize(&window);

    const auto manifest = am->packManifest();
    REQUIRE(manifest.size() == 1);
    CHECK(manifest[0].id == "good"); // the broken one is gone, the good one survives
}

TEST_CASE("AssetManager: packManifest tolerates a pack with null id and version (#1145)", "[content][assets]") {
    // A compiled plugin can legitimately return nullptr here; the manifest must not dereference it.
    struct NullIdPack final : public NullContentPack {
        const char* id() const override {
            return nullptr;
        }
        const char* version() const override {
            return nullptr;
        }
    };
    CountingLogger log;
    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.push_back(std::make_unique<NullIdPack>());
    auto am = makeManager(std::move(packs), log);

    const auto manifest = am->packManifest();
    REQUIRE(manifest.size() == 1);
    CHECK(manifest[0].id.empty());
    CHECK(manifest[0].version.empty());
}

// ---------------------------------------------------------------------------
// The load fall-through
// ---------------------------------------------------------------------------

TEST_CASE("AssetManager: a pack without the asset falls through to the next (#1145)", "[content][assets]") {
    CountingLogger log;
    auto empty = std::make_unique<ScriptedPack>();
    empty->packId = "empty";
    auto holder = std::make_unique<ScriptedPack>();
    holder->packId = "holder";
    holder->textureBytes = validKtx2();
    ScriptedPack* emptyRaw = empty.get();
    ScriptedPack* holderRaw = holder.get();

    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.push_back(std::move(empty));
    packs.push_back(std::move(holder));
    auto am = makeManager(std::move(packs), log);
    am->initialize(nullptr);

    const auto tex = am->loadTexture("hud_font");
    CHECK(tex != nullptr);
    CHECK(emptyRaw->loadCalls == 1);  // asked
    CHECK(holderRaw->loadCalls == 1); // and it answered
}

TEST_CASE("AssetManager: an asset failing validation is discarded and the next pack tried (#1145)",
          "[content][assets]") {
    CountingLogger log;
    auto rubbish = std::make_unique<ScriptedPack>();
    rubbish->packId = "rubbish";
    rubbish->textureBytes = std::vector<uint8_t>{'n', 'o', 't', ' ', 'k', 't', 'x', '2', 0, 0, 0, 0, 0, 0, 0, 0};
    auto good = std::make_unique<ScriptedPack>();
    good->packId = "good";
    good->textureBytes = validKtx2();

    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.push_back(std::move(rubbish));
    packs.push_back(std::move(good));
    auto am = makeManager(std::move(packs), log);
    am->initialize(nullptr);

    const auto tex = am->loadTexture("hud_font");
    CHECK(tex != nullptr);                   // the good pack supplied it
    CHECK(log.mentions("discarding asset")); // and the bad one was reported, not silently ignored
}

TEST_CASE("AssetManager: an asset no pack has returns null without throwing (#1145)", "[content][assets]") {
    CountingLogger log;
    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.push_back(std::make_unique<ScriptedPack>());
    auto am = makeManager(std::move(packs), log);
    am->initialize(nullptr);

    CHECK(am->loadTexture("nothing_here") == nullptr);
}

TEST_CASE("AssetManager: with no packs at all every load is a clean miss (#1145)", "[content][assets]") {
    CountingLogger log;
    auto am = makeManager({}, log);
    am->initialize(nullptr);
    CHECK_FALSE(am->hasPacks());
    CHECK(am->loadTexture("anything") == nullptr);
    CHECK(am->packManifest().empty());
}

TEST_CASE("AssetManager: a loaded asset is cached and the packs are not asked twice (#1145)", "[content][assets]") {
    CountingLogger log;
    auto p = std::make_unique<ScriptedPack>();
    p->textureBytes = validKtx2();
    ScriptedPack* raw = p.get();

    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.push_back(std::move(p));
    auto am = makeManager(std::move(packs), log);
    am->initialize(nullptr);

    const auto first = am->loadTexture("hud_font");
    const auto second = am->loadTexture("hud_font");
    REQUIRE(first != nullptr);
    CHECK(first == second);     // the same shared_ptr, not a second decode
    CHECK(raw->loadCalls == 1); // asked exactly once
}

TEST_CASE("AssetManager: the pack sees the name without its type prefix (#1145)", "[content][assets]") {
    CountingLogger log;
    auto p = std::make_unique<ScriptedPack>();
    p->textureBytes = validKtx2();
    ScriptedPack* raw = p.get();

    std::vector<std::unique_ptr<IContentPack>> packs;
    packs.push_back(std::move(p));
    auto am = makeManager(std::move(packs), log);
    am->initialize(nullptr);
    am->loadTexture("hud_font");

    CHECK(raw->lastRequested == "hud_font"); // the cache key's "texture:" prefix is manager-side only
}
