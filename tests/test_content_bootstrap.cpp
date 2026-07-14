// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for registerPackEntityDefs (#683): loading content-pack entity definitions into the
// server's EntityTypeRegistry. Uses a MockContentPack fixture (idiom from test_content_system.cpp).

#include <content/ContentBootstrap.h>

#include <ILogger.h>
#include <content/AssetManager.h>
#include <content/ContentIndex.h>
#include <content/IContentPack.h>
#include <entity/EntityTypeRegistry.h>
#include <mock_content.h>
#include <sensor/SensorDef.h>
#include <weapon/WeaponRegistry.h>

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
// registerPackWeaponDefs (#812)
// ---------------------------------------------------------------------------

namespace {

// Serves weapon-def TOML blobs keyed by asset name.
struct WeaponDefPack : public NullContentPack {
    std::map<std::string, std::string> weapons;

    bool hasAsset(const char* n, AssetType t) const override {
        return t == AssetType::Weapon && weapons.count(n) != 0;
    }
    std::optional<WeaponDefData> loadWeaponDef(const char* n) override {
        auto it = weapons.find(n);
        if (it == weapons.end())
            return std::nullopt;
        WeaponDefData d;
        d.name = n;
        d.bytes.assign(it->second.begin(), it->second.end());
        return d;
    }
    std::vector<std::string> listAssets(AssetType t) const override {
        std::vector<std::string> out;
        if (t == AssetType::Weapon)
            for (const auto& [k, v] : weapons)
                out.push_back(k);
        return out;
    }
};

std::string weaponToml(const std::string& id, const std::string& name) {
    return "[weapon]\nid = \"" + id + "\"\nname = \"" + name +
           "\"\ntype = \"missile\"\ncategory = \"air-to-air\"\n\n"
           "[performance]\nmax_range_nm = 10.0\nmax_speed_kts = 1500.0\n\n"
           "[warhead]\nblast_radius_ft = 30.0\ndamage = 50.0\n\n"
           "[load]\nweight_lb = 188.0\ndrag_factor = 0.0012\n";
}

std::vector<std::unique_ptr<IContentPack>> weaponPacksFrom(WeaponDefPack pack) {
    std::vector<std::unique_ptr<IContentPack>> v;
    v.push_back(std::make_unique<WeaponDefPack>(std::move(pack)));
    return v;
}

} // namespace

TEST_CASE("registerPackWeaponDefs registers pack weapons by id") {
    WeaponDefPack pack;
    pack.weapons["aim9p"] = weaponToml("fl-base:aim9p", "AIM-9P Sidewinder");
    pack.weapons["aim120c"] = weaponToml("fl-base:aim120c", "AIM-120C AMRAAM");

    NullLog log;
    AssetManager assets(weaponPacksFrom(std::move(pack)), log);
    assets.initialize(nullptr);

    WeaponRegistry registry;
    const uint32_t n = registerPackWeaponDefs(assets, registry, log);

    REQUIRE(n == 2);
    REQUIRE(registry.weaponCount() == 2u);
    // Registered by ID, from a file whose STEM is different -- which is what lets a hardpoint
    // resolve its stores without ever touching the filesystem.
    REQUIRE(registry.findById("fl-base:aim9p") != nullptr);
    CHECK(registry.findById("aim9p") == nullptr);
}

TEST_CASE("registerPackWeaponDefs skips a malformed weapon and keeps going") {
    WeaponDefPack pack;
    pack.weapons["good"] = weaponToml("fl-base:aim9p", "AIM-9P");
    pack.weapons["bad"] = "[weapon]\nid = \"fl-base:broken\"\n"; // missing required tables

    NullLog log;
    AssetManager assets(weaponPacksFrom(std::move(pack)), log);
    assets.initialize(nullptr);

    WeaponRegistry registry;
    const uint32_t n = registerPackWeaponDefs(assets, registry, log);

    REQUIRE(n == 1);
    CHECK(registry.findById("fl-base:aim9p") != nullptr);
}

TEST_CASE("registerPackWeaponDefs returns zero with no packs") {
    NullLog log;
    std::vector<std::unique_ptr<IContentPack>> none;
    AssetManager assets(std::move(none), log);
    assets.initialize(nullptr);

    WeaponRegistry registry;
    REQUIRE(registerPackWeaponDefs(assets, registry, log) == 0);
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
