// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for registerPackEntityDefs (#683): loading content-pack entity definitions into the
// server's EntityTypeRegistry. Uses a MockContentPack fixture (idiom from test_content_system.cpp).

#include "ContentBootstrap.h"

#include <ILogger.h>
#include <content/AssetManager.h>
#include <content/ContentIndex.h>
#include <content/IContentPack.h>
#include <entity/EntityTypeRegistry.h>
#include <mock_content.h>
#include <sensor/SensorDef.h>

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

struct RecordingLog : public ILogger {
    struct Entry {
        LogLevel level;
        std::string msg;
    };
    std::vector<Entry> entries;

    void log(LogLevel lvl, const char*, int, const char* msg) override {
        entries.push_back({lvl, msg ? msg : ""});
    }
    void setMinLevel(LogLevel) override {}
    void flush() override {}

    int count(LogLevel lvl, std::string_view needle) const {
        int n = 0;
        for (const auto& e : entries)
            if (e.level == lvl && e.msg.find(needle) != std::string::npos)
                ++n;
        return n;
    }
    bool has(LogLevel lvl, std::string_view needle = "") const {
        return count(lvl, needle) > 0;
    }
};

constexpr AssetType kDefTypes[] = {AssetType::EntityDef, AssetType::SensorDef};

// Content pack serving sensor-def TOML blobs keyed by asset name (the FILE stem -- deliberately not
// the def id, which is the whole distinction under test).
struct SensorDefPack : public NullContentPack {
    std::string ns{"fl-base"};
    std::map<std::string, std::string> sensors; // assetName -> TOML source

    const char* namespaceId() const override {
        return ns.c_str();
    }
    bool hasAsset(const char* n, AssetType t) const override {
        return t == AssetType::SensorDef && sensors.count(n) != 0;
    }
    std::optional<SensorDefData> loadSensorDef(const char* n) override {
        auto it = sensors.find(n);
        if (it == sensors.end())
            return std::nullopt;
        SensorDefData d;
        d.name = n;
        d.bytes.assign(it->second.begin(), it->second.end());
        return d;
    }
    std::vector<std::string> listAssets(AssetType t) const override {
        std::vector<std::string> out;
        if (t == AssetType::SensorDef)
            for (const auto& [k, v] : sensors)
                out.push_back(k);
        return out;
    }
};

std::string sensorToml(const std::string& id, const std::string& name) {
    return "[sensor]\nid = \"" + id + "\"\nname = \"" + name +
           "\"\ntype = \"radar\"\nemitter = true\n\n[search]\naz_half_angle_deg = 60.0\n"
           "el_half_angle_deg = 60.0\nmax_range_nm = 20.0\npod = 0.9\n";
}

std::vector<std::unique_ptr<IContentPack>> packsFrom(SensorDefPack pack) {
    std::vector<std::unique_ptr<IContentPack>> v;
    v.push_back(std::make_unique<SensorDefPack>(std::move(pack)));
    return v;
}

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

// ---------------------------------------------------------------------------
// makeSensorDefResolver (#810) -- THE regression for the epic's headline defect.
//
// Before ContentIndex, an entity's `sensors = ["fl-base:apq159"]` was handed straight to
// AssetManager, which built "sensors/fl-base:apq159.toml" -- a file that cannot exist. The miss was
// a Warn, and the aircraft silently fell back to the builtin eyeball. Every aircraft. Every pack.
// ---------------------------------------------------------------------------

TEST_CASE("makeSensorDefResolver resolves a namespaced sensor id to a real SensorDef") {
    SensorDefPack pack;
    pack.ns = "fl-base";
    pack.sensors["apq159"] = sensorToml("fl-base:apq159", "AN/APQ-159");

    RecordingLog log;
    AssetManager assets(packsFrom(std::move(pack)), log);
    assets.initialize(nullptr);

    ContentIndex index;
    index.build(assets, kDefTypes, log);

    auto resolver = makeSensorDefResolver(assets, index, log);
    std::shared_ptr<const sensor::SensorDef> def = resolver("fl-base:apq159");

    // The load-bearing assertion: a REAL sensor def, not a null that degrades to the eyeball.
    REQUIRE(def != nullptr);
    CHECK(def->id == "fl-base:apq159");
    CHECK(def->type == sensor::SensorType::Radar);
    CHECK_FALSE(log.has(LogLevel::Error));
}

TEST_CASE("makeSensorDefResolver logs an unknown sensor id at Error, not Warn") {
    SensorDefPack pack;
    pack.ns = "fl-base";
    pack.sensors["apq159"] = sensorToml("fl-base:apq159", "AN/APQ-159");

    RecordingLog log;
    AssetManager assets(packsFrom(std::move(pack)), log);
    assets.initialize(nullptr);

    ContentIndex index;
    index.build(assets, kDefTypes, log);

    auto resolver = makeSensorDefResolver(assets, index, log);
    CHECK(resolver("fl-base:typo") == nullptr);
    CHECK(log.has(LogLevel::Error, "unknown sensor def id 'fl-base:typo'"));
}

TEST_CASE("makeSensorDefResolver caches both hits and misses") {
    SensorDefPack pack;
    pack.ns = "fl-base";
    pack.sensors["apq159"] = sensorToml("fl-base:apq159", "AN/APQ-159");

    RecordingLog log;
    AssetManager assets(packsFrom(std::move(pack)), log);
    assets.initialize(nullptr);

    ContentIndex index;
    index.build(assets, kDefTypes, log);
    auto resolver = makeSensorDefResolver(assets, index, log);

    CHECK(resolver("fl-base:apq159") == resolver("fl-base:apq159")); // same shared_ptr, parsed once

    log.entries.clear();
    resolver("fl-base:typo");
    resolver("fl-base:typo");
    // A bad id is reported once, not once per spawn.
    CHECK(log.count(LogLevel::Error, "unknown sensor def id") == 1);
}
