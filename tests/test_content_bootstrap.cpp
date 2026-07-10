// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for registerPackEntityDefs (#683): loading content-pack entity definitions into the
// server's EntityTypeRegistry. Uses a MockContentPack fixture (idiom from test_content_system.cpp).

#include "ContentBootstrap.h"

#include <ILogger.h>
#include <content/AssetManager.h>
#include <content/IContentPack.h>
#include <entity/EntityTypeRegistry.h>
#include <mock_content.h>

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace fl;

namespace {

struct NullLog : public ILogger {
    void log(LogLevel, const char*, int, const char*) override {}
    void setMinLevel(LogLevel) override {}
    void flush() override {}
};

// Content pack serving a fixed set of entity-def TOML blobs keyed by asset name.
struct EntityDefPack : public NullContentPack {
    std::map<std::string, std::string> defs; // assetName -> TOML source

    std::optional<EntityDefData> loadEntityDef(const char* n) override {
        auto it = defs.find(n);
        if (it == defs.end())
            return std::nullopt;
        EntityDefData d;
        d.name = n;
        d.bytes.assign(it->second.begin(), it->second.end());
        return d;
    }
    std::vector<std::string> listAssets(AssetType t) const override {
        std::vector<std::string> out;
        if (t == AssetType::EntityDef)
            for (const auto& [k, v] : defs)
                out.push_back(k);
        return out;
    }
};

std::string defToml(const std::string& id, const std::string& name) {
    return "[entity]\nid = \"" + id + "\"\nname = \"" + name + "\"\ncategory = \"air_vehicle\"\nmax_hp = 120.0\n";
}

std::vector<std::unique_ptr<IContentPack>> packsFrom(EntityDefPack pack) {
    std::vector<std::unique_ptr<IContentPack>> v;
    v.push_back(std::make_unique<EntityDefPack>(std::move(pack)));
    return v;
}

} // namespace

TEST_CASE("registerPackEntityDefs registers valid pack entity defs") {
    EntityDefPack pack;
    pack.defs["f15c"] = defToml("fl-base:f15c", "F-15C Eagle");
    pack.defs["mig29"] = defToml("fl-base:mig29", "MiG-29 Fulcrum");

    NullLog log;
    AssetManager assets(packsFrom(std::move(pack)), log);
    assets.initialize(nullptr);

    EntityTypeRegistry registry;
    const uint32_t n = registerPackEntityDefs(assets, registry, log);

    REQUIRE(n == 2);
    REQUIRE(registry.typeCount() == 2);
    REQUIRE(registry.findById("fl-base:f15c") != nullptr);
    REQUIRE(registry.findById("fl-base:mig29") != nullptr);
    // Registered types are spawnable by id (the `spawn`/`types` path resolves through the registry).
    REQUIRE(registry.indexById("fl-base:f15c") != std::numeric_limits<uint32_t>::max());
}

TEST_CASE("registerPackEntityDefs skips a malformed def and continues") {
    EntityDefPack pack;
    pack.defs["good"] = defToml("fl-base:good", "Good");
    pack.defs["bad"] = "this is not valid toml = = =\n[entity\n";  // parse error
    pack.defs["missing"] = "[entity]\nid = \"fl-base:missing\"\n"; // missing required fields

    NullLog log;
    AssetManager assets(packsFrom(std::move(pack)), log);
    assets.initialize(nullptr);

    EntityTypeRegistry registry;
    const uint32_t n = registerPackEntityDefs(assets, registry, log);

    REQUIRE(n == 1);
    REQUIRE(registry.findById("fl-base:good") != nullptr);
    REQUIRE(registry.findById("fl-base:missing") == nullptr);
}

TEST_CASE("registerPackEntityDefs skips a duplicate id (first-wins)") {
    // Two distinct asset files declare the same internal entity id -- the registry rejects the
    // second by id, so only one type is registered.
    EntityDefPack pack;
    pack.defs["variant_a"] = defToml("fl-base:dupe", "Variant A");
    pack.defs["variant_b"] = defToml("fl-base:dupe", "Variant B");

    NullLog log;
    AssetManager assets(packsFrom(std::move(pack)), log);
    assets.initialize(nullptr);

    EntityTypeRegistry registry;
    const uint32_t n = registerPackEntityDefs(assets, registry, log);

    REQUIRE(n == 1);
    REQUIRE(registry.typeCount() == 1);
    REQUIRE(registry.findById("fl-base:dupe") != nullptr);
}

TEST_CASE("registerPackEntityDefs returns zero with no packs") {
    NullLog log;
    std::vector<std::unique_ptr<IContentPack>> none;
    AssetManager assets(std::move(none), log);
    assets.initialize(nullptr);

    EntityTypeRegistry registry;
    REQUIRE(registerPackEntityDefs(assets, registry, log) == 0);
    REQUIRE(registry.typeCount() == 0);
}
