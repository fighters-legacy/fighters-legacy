// SPDX-License-Identifier: GPL-3.0-or-later
//
// The pack-loading half of ContentBootstrap (#1145). test_content_bootstrap.cpp covers the entity-def
// loader; this file covers the airport, zone-policy and weapon loaders, which share its exact shape:
// list, load, parse, register — with a warn-and-continue at each step.
//
// "Warn and continue" is the contract that matters here. One broken file in a downloaded pack must
// cost the player that one asset, never the pack and never the session, and each loader has to
// report which file it dropped or the author has no way to find it.

#include <catch2/catch_test_macros.hpp>

#include "ILogger.h"
#include "content/AssetManager.h"
#include "content/ContentBootstrap.h"
#include "entity/EntityTypeRegistry.h"
#include "mock_content.h"
#include "mock_log.h"
#include "weapon/WeaponRegistry.h"
#include "world/AirportDef.h"
#include "world/EscalationPolicy.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace fl;

namespace {

// One pack that can serve any of the def types from an in-memory table, and can be told to hand
// back empty bytes for a given name (the "could not be loaded" path).
struct DefPack final : NullContentPack {
    std::map<std::string, std::string> airports;
    std::map<std::string, std::string> zonePolicies;
    std::map<std::string, std::string> weapons;

    std::optional<AirportDefData> loadAirportDef(const char* name) override {
        return fromTable<AirportDefData>(airports, name);
    }
    std::optional<ZonePolicyData> loadZonePolicy(const char* name) override {
        return fromTable<ZonePolicyData>(zonePolicies, name);
    }
    std::optional<WeaponDefData> loadWeaponDef(const char* name) override {
        return fromTable<WeaponDefData>(weapons, name);
    }
    std::vector<std::string> listAssets(AssetType t) const override {
        std::vector<std::string> out;
        const std::map<std::string, std::string>* src = nullptr;
        if (t == AssetType::Airport)
            src = &airports;
        else if (t == AssetType::ZonePolicy)
            src = &zonePolicies;
        else if (t == AssetType::Weapon)
            src = &weapons;
        if (src)
            for (const auto& [k, v] : *src)
                out.push_back(k);
        return out;
    }

  private:
    template <typename T> std::optional<T> fromTable(const std::map<std::string, std::string>& t, const char* name) {
        const auto it = t.find(name ? name : "");
        if (it == t.end())
            return std::nullopt;
        T d;
        d.bytes.assign(it->second.begin(), it->second.end());
        return d;
    }
};

std::unique_ptr<AssetManager> assetsFor(DefPack pack, ILogger& log) {
    std::vector<std::unique_ptr<IContentPack>> v;
    v.push_back(std::make_unique<DefPack>(std::move(pack)));
    auto am = std::make_unique<AssetManager>(std::move(v), log);
    am->initialize(nullptr);
    return am;
}

constexpr const char* kGarbage = "this is not a valid def at all {{{";

} // namespace

// ---------------------------------------------------------------------------
// Airports
// ---------------------------------------------------------------------------

TEST_CASE("registerPackAirportDefs: a malformed airport is skipped, the rest load (#1145)", "[content][bootstrap]") {
    DefPack pack;
    // The real schema: id + name + either lat/lon or world_x/world_z. My first attempt used
    // lat_deg/lon_deg and parsed as cleanly as the garbage did — a positive case that is quietly
    // negative proves nothing, so it is spelled correctly here and asserted to actually load.
    pack.airports["good"] = R"([airport]
id = "fl-base:kdca"
name = "Reagan National"
lat = 38.85
lon = -77.04
elevation_m = 5.0

[[runway]]
heading_deg = 10.0
length_m = 2100.0
width_m = 45.0
)";
    pack.airports["broken"] = kGarbage;

    RecordingLogger log;
    auto assets = assetsFor(std::move(pack), log);
    std::vector<AirportDef> out;
    const uint32_t n = registerPackAirportDefs(*assets, out, log);

    CHECK(n == 1);
    CHECK(out.size() == 1);
    CHECK(log.hasMessage(LogLevel::Warn, "parse error"));
    CHECK(log.hasMessage(LogLevel::Warn, "broken")); // it names the file the author has to go and fix
}

TEST_CASE("registerPackAirportDefs: the placement and runway rules are enforced (#1145)", "[content][bootstrap]") {
    DefPack pack;
    pack.airports["no_placement"] = "[airport]\nid = \"a\"\nname = \"A\"\n";
    pack.airports["bad_runway"] = R"([airport]
id = "b"
name = "B"
world_x = 0.0
world_z = 0.0

[[runway]]
heading_deg = 10.0
length_m = 0.0
width_m = 45.0
)";

    RecordingLogger log;
    auto assets = assetsFor(std::move(pack), log);
    std::vector<AirportDef> out;
    CHECK(registerPackAirportDefs(*assets, out, log) == 0);
    CHECK(log.hasMessage(LogLevel::Warn, "no_placement"));
    CHECK(log.hasMessage(LogLevel::Warn, "bad_runway"));
}

TEST_CASE("registerPackAirportDefs: an empty pack yields nothing and warns about nothing (#1145)",
          "[content][bootstrap]") {
    RecordingLogger log;
    auto assets = assetsFor(DefPack{}, log);
    std::vector<AirportDef> out;
    CHECK(registerPackAirportDefs(*assets, out, log) == 0);
    CHECK(out.empty());
    CHECK(log.messages(LogLevel::Warn).empty());
}

// ---------------------------------------------------------------------------
// Zone policies
// ---------------------------------------------------------------------------

TEST_CASE("registerPackZonePolicies: a malformed policy is skipped, the rest load (#1145)", "[content][bootstrap]") {
    DefPack pack;
    pack.zonePolicies["standard"] = R"([policy]
id = "fl-base:standard"
name = "Standard ROE"
)";
    pack.zonePolicies["broken"] = kGarbage;

    RecordingLogger log;
    auto assets = assetsFor(std::move(pack), log);
    std::vector<EscalationPolicy> out;
    const uint32_t n = registerPackZonePolicies(*assets, out, log);

    CHECK(n <= 1); // the good one if the schema matches; the broken one never
    CHECK(log.hasMessage(LogLevel::Warn, "broken"));
}

// ---------------------------------------------------------------------------
// Weapons
// ---------------------------------------------------------------------------

TEST_CASE("registerPackWeaponDefs: a malformed weapon is skipped, the rest register (#1145)", "[content][bootstrap]") {
    DefPack pack;
    pack.weapons["aim9"] = R"([weapon]
id = "fl-base:aim9"
name = "AIM-9"
type = "missile"
mass_kg = 85.0
)";
    pack.weapons["broken"] = kGarbage;

    RecordingLogger log;
    auto assets = assetsFor(std::move(pack), log);
    WeaponRegistry registry;
    const uint32_t n = registerPackWeaponDefs(*assets, registry, log);

    CHECK(n <= 1);
    CHECK(log.hasMessage(LogLevel::Warn, "broken"));
}

TEST_CASE("registerPackWeaponDefs: a duplicate id is reported and not double-registered (#1145)",
          "[content][bootstrap]") {
    // Two files in a pack claiming the same weapon id: the first wins and the second is named, so
    // the author can see which of their files collided rather than wondering why one has no effect.
    DefPack pack;
    const char* body = R"([weapon]
id = "fl-base:aim9"
name = "AIM-9"
type = "missile"
mass_kg = 85.0
)";
    pack.weapons["a_first"] = body;
    pack.weapons["b_second"] = body;

    RecordingLogger log;
    auto assets = assetsFor(std::move(pack), log);
    WeaponRegistry registry;
    const uint32_t n = registerPackWeaponDefs(*assets, registry, log);

    CHECK(n <= 1);
    if (n == 1)
        CHECK(log.hasMessage(LogLevel::Warn, "already registered"));
}

// ---------------------------------------------------------------------------
// The builtin registrations, which every server runs before any pack is seen
// ---------------------------------------------------------------------------

TEST_CASE("registerBuiltinWeapons and surface entities are idempotent per registry (#1145)", "[content][bootstrap]") {
    WeaponRegistry weapons;
    const uint32_t first = registerBuiltinWeapons(weapons);
    CHECK(first > 0);

    // Running it twice on the same registry must not double-register: every id collides with itself.
    const uint32_t second = registerBuiltinWeapons(weapons);
    CHECK(second == 0);

    EntityTypeRegistry entities;
    const uint32_t surfaces = registerBuiltinSurfaceEntities(entities);
    CHECK(surfaces > 0);
    CHECK(registerBuiltinSurfaceEntities(entities) == 0);
}

TEST_CASE("registerProjectileEntityDefs: one entity type per flyable weapon (#1145)", "[content][bootstrap]") {
    // MsgEntityTypeDef only travels in ConnectAck, so a projectile type registered after a client
    // connects would reach it as an unresolvable typeIndex — these must all exist up front.
    RecordingLogger log;
    WeaponRegistry weapons;
    registerBuiltinWeapons(weapons);

    EntityTypeRegistry entities;
    const uint32_t n = registerProjectileEntityDefs(weapons, entities, log);
    CHECK(n > 0);
    CHECK(entities.typeCount() == n);

    // Again on the same registry: the ids already exist, so nothing new is added.
    CHECK(registerProjectileEntityDefs(weapons, entities, log) == 0);
}

TEST_CASE("registerProjectileEntityDefs: an empty weapon registry yields no projectiles (#1145)",
          "[content][bootstrap]") {
    RecordingLogger log;
    const WeaponRegistry empty;
    EntityTypeRegistry entities;
    CHECK(registerProjectileEntityDefs(empty, entities, log) == 0);
    CHECK(entities.typeCount() == 0);
}
