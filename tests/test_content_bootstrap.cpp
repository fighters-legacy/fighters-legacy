// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for registerPackEntityDefs (#683): loading content-pack entity definitions into the
// server's EntityTypeRegistry. Uses a MockContentPack fixture (idiom from test_content_system.cpp).

#include "mock_log.h"
#include <content/ContentBootstrap.h>

#include <ILogger.h>
#include <content/AssetManager.h>
#include <content/ContentIndex.h>
#include <content/IContentPack.h>
#include <entity/EntityTypeRegistry.h>
#include <mock_content.h>
#include <sensor/BuiltinSensors.h>
#include <sensor/SensorDef.h>
#include <weapon/BuiltinWeapon.h>
#include <weapon/WeaponDef.h>
#include <weapon/WeaponRegistry.h>

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace fl;

namespace {

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

    NullLogger log;
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

    NullLogger log;
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

    NullLogger log;
    AssetManager assets(packsFrom(std::move(pack)), log);
    assets.initialize(nullptr);

    EntityTypeRegistry registry;
    const uint32_t n = registerPackEntityDefs(assets, registry, log);

    REQUIRE(n == 1);
    REQUIRE(registry.typeCount() == 1);
    REQUIRE(registry.findById("fl-base:dupe") != nullptr);
}

TEST_CASE("registerPackEntityDefs returns zero with no packs") {
    NullLogger log;
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

    NullLogger log;
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

    NullLogger log;
    AssetManager assets(weaponPacksFrom(std::move(pack)), log);
    assets.initialize(nullptr);

    WeaponRegistry registry;
    const uint32_t n = registerPackWeaponDefs(assets, registry, log);

    REQUIRE(n == 1);
    CHECK(registry.findById("fl-base:aim9p") != nullptr);
}

TEST_CASE("registerPackWeaponDefs returns zero with no packs") {
    NullLogger log;
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

    RecordingLogger log;
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
    CHECK_FALSE(log.hasMessage(LogLevel::Error));
}

TEST_CASE("makeSensorDefResolver logs an unknown sensor id at Error, not Warn") {
    SensorDefPack pack;
    pack.ns = "fl-base";
    pack.sensors["apq159"] = sensorToml("fl-base:apq159", "AN/APQ-159");

    RecordingLogger log;
    AssetManager assets(packsFrom(std::move(pack)), log);
    assets.initialize(nullptr);

    ContentIndex index;
    index.build(assets, kDefTypes, log);

    auto resolver = makeSensorDefResolver(assets, index, log);
    CHECK(resolver("fl-base:typo") == nullptr);
    CHECK(log.hasMessage(LogLevel::Error, "unknown sensor def id 'fl-base:typo'"));
}

TEST_CASE("makeSensorDefResolver caches both hits and misses") {
    SensorDefPack pack;
    pack.ns = "fl-base";
    pack.sensors["apq159"] = sensorToml("fl-base:apq159", "AN/APQ-159");

    RecordingLogger log;
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

// ---------------------------------------------------------------------------
// Builtin sandbox arming (#440)
// ---------------------------------------------------------------------------

TEST_CASE("registerBuiltinWeapons registers every sandbox store exactly once (#862)") {
    WeaponRegistry weapons;
    // cannon, IR, radar, SARH, bomb, rocket, drop tank, sensor pod
    CHECK(registerBuiltinWeapons(weapons) == 8u);
    CHECK(weapons.findById("builtin:cannon") != nullptr);
    CHECK(weapons.findById("builtin:ir-missile") != nullptr);
    CHECK(weapons.findById("builtin:radar-missile") != nullptr);
    CHECK(weapons.findById("builtin:sarh-missile") != nullptr);
    CHECK(weapons.findById("builtin:bomb") != nullptr);
    CHECK(weapons.findById("builtin:rocket") != nullptr);
    CHECK(weapons.findById("builtin:drop-tank") != nullptr);
    CHECK(weapons.findById("builtin:pod") != nullptr);

    // Idempotent: a second call registers nothing and breaks nothing.
    CHECK(registerBuiltinWeapons(weapons) == 0u);
}

TEST_CASE("the builtin debug entity has a real damage model with the full subsystem table (#864)") {
    const EntityDef def = builtinDebugEntityDef();
    REQUIRE(def.damage.has_value());             // not binary death anymore
    REQUIRE(def.damage->subsystems.has_value()); // + a subsystem table
    CHECK(def.damage->subsystems->any());
    // An aircraft models BOTH engines (asymmetric thrust on a single-engine loss) plus controls,
    // avionics, hydraulics and fuel.
    for (Subsystem s : {Subsystem::EngineLeft, Subsystem::EngineRight, Subsystem::Controls, Subsystem::Avionics,
                        Subsystem::Hydraulics, Subsystem::Fuel})
        CHECK(def.damage->subsystems->parts[static_cast<int>(s)].hp > 0.f);
    // Progressive penalties: heavy/critical degrade thrust + control, critical kills avionics.
    CHECK(def.damage->heavy.thrustFactor < 1.f);
    CHECK(def.damage->critical.controlFactor < def.damage->heavy.controlFactor);
    CHECK(def.damage->critical.avionicsFailure);
}

TEST_CASE("the builtin drop tank is a Fuel store -- inert, mass/drag only (#862)") {
    const WeaponDef& tank = BuiltinWeapon::dropTank();
    CHECK(tank.type == WeaponType::Fuel);
    CHECK_FALSE(tank.seeker.has_value()); // not a weapon: no seeker
    CHECK(tank.warhead.damage == 0.f);    // no warhead
    CHECK(tank.load.massKg > 0.f);        // but it does cost the airframe
    CHECK(tank.load.rounds == 0u);        // never fires
}

TEST_CASE("builtinDebugEntityDef is armed across every store class and resolves against the registry (#862)") {
    WeaponRegistry weapons;
    registerBuiltinWeapons(weapons);

    const EntityDef def = builtinDebugEntityDef();
    CHECK(def.id == "builtin:debug-entity");
    REQUIRE(def.hardpoints.size() == 8u); // gun, IR, radar, SARH, bomb, rocket, drop tank, pod

    for (const Hardpoint& hp : def.hardpoints) {
        REQUIRE_FALSE(hp.defaultWeapon.empty());
        CHECK(weapons.findById(hp.defaultWeapon.c_str()) != nullptr);
        CHECK(hp.allowed.size() == 1u);
    }
    // EVERY WeaponType is mounted somewhere — the whole fire path is provable zero-pack. The
    // station itself has no kind; what matters is the KINDS OF STORE the loadout carries.
    std::set<WeaponType> kinds;
    for (const Hardpoint& hp : def.hardpoints)
        if (const WeaponDef* w = weapons.findById(hp.defaultWeapon.c_str()))
            kinds.insert(w->type);
    CHECK(kinds.count(WeaponType::Gun) == 1u);
    CHECK(kinds.count(WeaponType::Missile) == 1u);
    CHECK(kinds.count(WeaponType::Bomb) == 1u);
    CHECK(kinds.count(WeaponType::Rocket) == 1u);
    CHECK(kinds.count(WeaponType::Fuel) == 1u);
    CHECK(kinds.count(WeaponType::Pod) == 1u);
}

TEST_CASE("builtinBomberDef is a valid crewed aircraft: a pilot + a bot tail-gunner turret (#977)") {
    WeaponRegistry weapons;
    registerBuiltinWeapons(weapons);

    const EntityDef def = builtinBomberDef();
    CHECK(def.id == "builtin:bomber");
    CHECK(def.category == ObjectCategory::AirVehicle);

    // Two seats: exactly one Fly seat (the pilot) + a Fire tail-gunner aiming the turret.
    REQUIRE(def.crew.size() == 2u);
    REQUIRE(def.turrets.size() == 1u);
    int flySeats = 0;
    const SeatDef* gunner = nullptr;
    for (const SeatDef& s : def.crew) {
        if (hasCapability(s.capabilities, CrewCapability::Fly))
            ++flySeats;
        if (!s.turret.empty())
            gunner = &s;
    }
    CHECK(flySeats == 1);
    REQUIRE(gunner != nullptr);
    CHECK(hasCapability(gunner->capabilities, CrewCapability::Fire));
    CHECK(gunner->turret == "tail");
    CHECK(gunner->botSpec == "builtin:gunner"); // resolves to the TurretGunnerController via makeSeatController

    // The turret mounts a real cannon on station 1, and the tail gun resolves against the registry.
    const TurretDef& tail = def.turrets[0];
    REQUIRE(tail.stations == std::vector<int>{1});
    CHECK(tail.slewRateDegS > 0.f);
    CHECK(tail.azMinDeg < tail.azMaxDeg);
    bool tailGunResolves = false;
    for (const Hardpoint& hp : def.hardpoints)
        if (hp.slot == 1)
            tailGunResolves = weapons.findById(hp.defaultWeapon.c_str()) != nullptr;
    CHECK(tailGunResolves);

    // The authored partition is legal (the same invariant the parser enforces).
    std::vector<int> slots;
    for (const Hardpoint& hp : def.hardpoints)
        slots.push_back(hp.slot);
    CHECK(validateCrewPartition(def.crew, def.turrets, slots).empty());
}

TEST_CASE("flying builtin stores get projectile entity types; gun, drop tank, and pod do not (#862)") {
    WeaponRegistry weapons;
    registerBuiltinWeapons(weapons);
    EntityTypeRegistry registry;
    NullLogger log;

    // IR / radar / SARH missiles + bomb + rocket fly; the gun is hitscan and the tank/pod are inert.
    CHECK(registerProjectileEntityDefs(weapons, registry, log) == 5u);
    CHECK(registry.findById("projectile:builtin:ir-missile") != nullptr);
    CHECK(registry.findById("projectile:builtin:radar-missile") != nullptr);
    CHECK(registry.findById("projectile:builtin:sarh-missile") != nullptr);
    CHECK(registry.findById("projectile:builtin:bomb") != nullptr);
    CHECK(registry.findById("projectile:builtin:rocket") != nullptr);
    CHECK(registry.findById("projectile:builtin:cannon") == nullptr);
    CHECK(registry.findById("projectile:builtin:drop-tank") == nullptr);
    CHECK(registry.findById("projectile:builtin:pod") == nullptr);

    // Each projectile def records its weapon class (#886) — the per-class placeholder key.
    CHECK(registry.findById("projectile:builtin:ir-missile")->projectileKind == ProjectileKind::Missile);
    CHECK(registry.findById("projectile:builtin:radar-missile")->projectileKind == ProjectileKind::Missile);
    CHECK(registry.findById("projectile:builtin:sarh-missile")->projectileKind == ProjectileKind::Missile);
    CHECK(registry.findById("projectile:builtin:bomb")->projectileKind == ProjectileKind::Bomb);
    CHECK(registry.findById("projectile:builtin:rocket")->projectileKind == ProjectileKind::Rocket);
}

TEST_CASE("builtin static target is a Structure; emplacements stay ground vehicles (#886)") {
    // The bunker-class target is a fixed structure; the SAM/AAA emplacements remain
    // GroundVehicle (they may gain mobility under #585).
    CHECK(builtinStaticTargetDef().category == ObjectCategory::Structure);
    CHECK(builtinSamSiteDef().category == ObjectCategory::GroundVehicle);
    CHECK(builtinAaaDef().category == ObjectCategory::GroundVehicle);
    CHECK(builtinGroundVehicleDef().category == ObjectCategory::GroundVehicle);
    CHECK(builtinNavalVesselDef().category == ObjectCategory::NavalVehicle);
    // Surface entities are not projectiles.
    CHECK(builtinStaticTargetDef().projectileKind == ProjectileKind::None);
}

// ---------------------------------------------------------------------------
// #1089: the sensor-carrying builtin fighter
// ---------------------------------------------------------------------------

TEST_CASE("builtinSensorFighterDef carries sensors the debug entity does not", "[content_bootstrap][1089]") {
    const EntityDef sensorFighter = builtinSensorFighterDef();
    const EntityDef debug = builtinDebugEntityDef();

    // The whole point: the scale gate needs bots whose contact tables actually populate. The debug
    // entity carries no sensors, which is why the gate measured a hollow world.
    CHECK(debug.sensorIds.empty());
    REQUIRE(sensorFighter.sensorIds.size() == 2u);
    CHECK(sensorFighter.sensorIds[0] == "builtin:eyeball");
    CHECK(sensorFighter.sensorIds[1] == "builtin:ai-radar");

    // A DISTINCT type, so every existing spawn of the debug entity is unaffected — entity-scale
    // deliberately keeps its hollow sweep.
    CHECK(sensorFighter.id == "builtin:sensor-fighter");
    CHECK(sensorFighter.id != debug.id);

    // Otherwise identical to the debug entity: it is the same airframe plus sensors, so a
    // sensor-loaded gate run is not also a different-flight-model run.
    CHECK(sensorFighter.category == debug.category);
    CHECK(sensorFighter.maxHp == debug.maxHp);
    CHECK(sensorFighter.hardpoints.size() == debug.hardpoints.size());
}

TEST_CASE("the builtin intercept radar is a forward-scanning emitter with a track lobe", "[content_bootstrap][1089]") {
    const sensor::SensorDef& d = sensor::BuiltinSensors::interceptRadar();
    CHECK(d.id == "builtin:ai-radar");
    CHECK(d.type == sensor::SensorType::Radar);
    CHECK(d.emitter);               // an airborne radar announces itself to an RWR (#529)
    CHECK_FALSE(d.omnidirectional); // a nose-mounted scan volume, not a ground battery's hemisphere
    // A track lobe matters: without it contacts never reach Locked/firing-quality, and the datalink
    // fusion path would be exercised only at Detected.
    REQUIRE(d.track.has_value());
    CHECK(d.track->maxRangeM > d.search.maxRangeM);
    // Range sits between a missile seeker's and a ground battery's.
    CHECK(d.search.maxRangeM > sensor::BuiltinSensors::radarSeeker().search.maxRangeM);
    CHECK(d.search.maxRangeM > sensor::BuiltinSensors::groundRadar().search.maxRangeM);
}
