// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "ILogger.h"

#include "entity/DamageDef.h"
#include "entity/EntityDefParser.h"
#include "entity/EntityEvent.h"
#include "entity/EntityId.h"
#include "entity/EntityManager.h"
#include "entity/EntityPool.h"
#include "entity/EntityState.h"
#include "entity/EntityTypeRegistry.h"
#include "entity/ObjectCategory.h"

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using Catch::Matchers::WithinAbs;

// ---------------------------------------------------------------------------
// Mock logger
// ---------------------------------------------------------------------------

struct MockLogger : public ILogger {
    struct Entry {
        LogLevel level;
        std::string message;
    };
    std::vector<Entry> entries;

    void log(LogLevel level, const char* /*file*/, int /*line*/, const char* message) override {
        entries.push_back({level, message});
    }
    void setMinLevel(LogLevel) override {}
    void flush() override {}

    bool hasMessage(LogLevel level, const std::string& substr) const {
        for (const auto& e : entries)
            if (e.level == level && e.message.find(substr) != std::string::npos)
                return true;
        return false;
    }
};

// ---------------------------------------------------------------------------
// Minimal TOML fixtures
// ---------------------------------------------------------------------------

static const char* kMinimalEntityToml = R"(
[entity]
id       = "test:fighter"
name     = "Test Fighter"
category = "air_vehicle"
max_hp   = 100.0
mesh     = "aircraft/test"
)";

static const char* kFullEntityToml = R"(
[entity]
id       = "test:tank"
name     = "Test Tank"
category = "ground_vehicle"
max_hp   = 200.0
mesh     = "ground/tank"

[damage.light]
hp_fraction    = 0.75
visual_effect  = "smoke_light"
thrust_factor  = 0.9
control_factor = 1.0

[damage.heavy]
hp_fraction      = 0.40
visual_effect    = "smoke_heavy"
thrust_factor    = 0.60
control_factor   = 0.80
avionics_failure = false

[damage.critical]
hp_fraction      = 0.15
visual_effect    = "fire"
thrust_factor    = 0.25
control_factor   = 0.50
avionics_failure = true

[classic]
damage_mesh = "ground/tank_damaged"
)";

static fl::EntityDef makeAirVehicleDef(const char* id = "test:f15") {
    fl::EntityDef def;
    def.id = id;
    def.name = "F-15";
    def.category = fl::ObjectCategory::AirVehicle;
    def.maxHp = 100.f;
    def.mesh = "aircraft/f15";
    return def;
}

static fl::EntityDef makeDefWithDamage(const char* id = "test:damaged") {
    fl::EntityDef def = makeAirVehicleDef(id);
    fl::DamageDef dmg;
    dmg.light.hpFraction = 0.75f;
    dmg.heavy.hpFraction = 0.40f;
    dmg.critical.hpFraction = 0.15f;
    def.damage = dmg;
    return def;
}

// ---------------------------------------------------------------------------
// EntityId
// ---------------------------------------------------------------------------

TEST_CASE("EntityId: null is not valid", "[entity_id]") {
    CHECK_FALSE(fl::EntityId::null().valid());
    CHECK_FALSE(fl::EntityId{}.valid());
}

TEST_CASE("EntityId: non-zero generation is valid", "[entity_id]") {
    fl::EntityId id{0, 1};
    CHECK(id.valid());
}

TEST_CASE("EntityId: equality and inequality", "[entity_id]") {
    fl::EntityId a{1, 2};
    fl::EntityId b{1, 2};
    fl::EntityId c{1, 3};
    CHECK(a == b);
    CHECK(a != c);
}

// ---------------------------------------------------------------------------
// ObjectCategory helpers
// ---------------------------------------------------------------------------

TEST_CASE("objectCategoryName returns stable ASCII names", "[object_category]") {
    CHECK(std::string(fl::objectCategoryName(fl::ObjectCategory::AirVehicle)) == "air_vehicle");
    CHECK(std::string(fl::objectCategoryName(fl::ObjectCategory::GroundVehicle)) == "ground_vehicle");
    CHECK(std::string(fl::objectCategoryName(fl::ObjectCategory::NavalVehicle)) == "naval_vehicle");
    CHECK(std::string(fl::objectCategoryName(fl::ObjectCategory::Projectile)) == "projectile");
    CHECK(std::string(fl::objectCategoryName(fl::ObjectCategory::Effect)) == "effect");
    CHECK(std::string(fl::objectCategoryName(fl::ObjectCategory::Player)) == "player");
}

// ---------------------------------------------------------------------------
// DamageDef / evaluateDamageLevel
// ---------------------------------------------------------------------------

TEST_CASE("evaluateDamageLevel: Intact above light threshold", "[damage]") {
    fl::DamageDef def;
    def.light.hpFraction = 0.75f;
    def.heavy.hpFraction = 0.40f;
    def.critical.hpFraction = 0.15f;
    CHECK(fl::evaluateDamageLevel(def, 1.0f) == fl::DamageLevel::Intact);
    CHECK(fl::evaluateDamageLevel(def, 0.76f) == fl::DamageLevel::Intact);
}

TEST_CASE("evaluateDamageLevel: Light between light and heavy thresholds", "[damage]") {
    fl::DamageDef def;
    def.light.hpFraction = 0.75f;
    def.heavy.hpFraction = 0.40f;
    def.critical.hpFraction = 0.15f;
    CHECK(fl::evaluateDamageLevel(def, 0.75f) == fl::DamageLevel::Light);
    CHECK(fl::evaluateDamageLevel(def, 0.50f) == fl::DamageLevel::Light);
    CHECK(fl::evaluateDamageLevel(def, 0.41f) == fl::DamageLevel::Light);
}

TEST_CASE("evaluateDamageLevel: Heavy between heavy and critical thresholds", "[damage]") {
    fl::DamageDef def;
    def.light.hpFraction = 0.75f;
    def.heavy.hpFraction = 0.40f;
    def.critical.hpFraction = 0.15f;
    CHECK(fl::evaluateDamageLevel(def, 0.40f) == fl::DamageLevel::Heavy);
    CHECK(fl::evaluateDamageLevel(def, 0.25f) == fl::DamageLevel::Heavy);
    CHECK(fl::evaluateDamageLevel(def, 0.16f) == fl::DamageLevel::Heavy);
}

TEST_CASE("evaluateDamageLevel: Critical below critical threshold", "[damage]") {
    fl::DamageDef def;
    def.light.hpFraction = 0.75f;
    def.heavy.hpFraction = 0.40f;
    def.critical.hpFraction = 0.15f;
    CHECK(fl::evaluateDamageLevel(def, 0.15f) == fl::DamageLevel::Critical);
    CHECK(fl::evaluateDamageLevel(def, 0.05f) == fl::DamageLevel::Critical);
}

TEST_CASE("evaluateDamageLevel: Destroyed at exactly 0", "[damage]") {
    fl::DamageDef def;
    def.light.hpFraction = 0.75f;
    def.heavy.hpFraction = 0.40f;
    def.critical.hpFraction = 0.15f;
    CHECK(fl::evaluateDamageLevel(def, 0.0f) == fl::DamageLevel::Destroyed);
    CHECK(fl::evaluateDamageLevel(def, -1.0f) == fl::DamageLevel::Destroyed);
}

// ---------------------------------------------------------------------------
// EntityPool
// ---------------------------------------------------------------------------

TEST_CASE("EntityPool: alloc returns valid distinct IDs", "[entity_pool]") {
    fl::EntityPool pool;
    fl::EntityId a = pool.alloc();
    fl::EntityId b = pool.alloc();
    CHECK(a.valid());
    CHECK(b.valid());
    CHECK(a != b);
    CHECK(pool.liveCount() == 2);
}

TEST_CASE("EntityPool: free invalidates the ID", "[entity_pool]") {
    fl::EntityPool pool;
    fl::EntityId id = pool.alloc();
    REQUIRE(id.valid());
    pool.free(id);
    CHECK_FALSE(pool.valid(id));
    CHECK(pool.liveCount() == 0);
}

TEST_CASE("EntityPool: slot reuse increments generation", "[entity_pool]") {
    fl::EntityPool pool;
    fl::EntityId first = pool.alloc();
    pool.free(first);
    fl::EntityId second = pool.alloc();
    // Same index is reused
    CHECK(second.index == first.index);
    // But generation differs, making old handle stale
    CHECK(second.generation != first.generation);
    CHECK(pool.valid(second));
    CHECK_FALSE(pool.valid(first));
}

TEST_CASE("EntityPool: soft cap enforced, alloc returns null when full", "[entity_pool]") {
    fl::EntityPool pool;
    pool.setSoftCap(3);
    auto a = pool.alloc();
    auto b = pool.alloc();
    auto c = pool.alloc();
    auto d = pool.alloc(); // should fail
    CHECK(a.valid());
    CHECK(b.valid());
    CHECK(c.valid());
    CHECK_FALSE(d.valid());
    CHECK(pool.liveCount() == 3);
}

TEST_CASE("EntityPool: forEach visits only live entities", "[entity_pool]") {
    fl::EntityPool pool;
    auto a = pool.alloc();
    auto b = pool.alloc();
    auto c = pool.alloc();
    REQUIRE(a.valid());
    REQUIRE(c.valid());
    pool.free(b);

    int count = 0;
    pool.forEach([&](const fl::EntityState&) { ++count; });
    CHECK(count == 2);
}

TEST_CASE("EntityPool: liveCount is accurate across alloc and free", "[entity_pool]") {
    fl::EntityPool pool;
    CHECK(pool.liveCount() == 0);
    auto a = pool.alloc();
    auto b = pool.alloc();
    CHECK(pool.liveCount() == 2);
    pool.free(a);
    CHECK(pool.liveCount() == 1);
    pool.free(b);
    CHECK(pool.liveCount() == 0);
}

TEST_CASE("EntityPool: dynamic growth past initial capacity", "[entity_pool]") {
    fl::EntityPool pool(4); // small initial capacity
    std::vector<fl::EntityId> ids;
    for (int i = 0; i < 300; ++i)
        ids.push_back(pool.alloc());

    CHECK(pool.liveCount() == 300);
    CHECK(pool.capacity() >= 300u);

    // All IDs must still be valid after growth
    for (auto id : ids)
        CHECK(pool.valid(id));
}

TEST_CASE("EntityPool: get returns nullptr for invalid ID", "[entity_pool]") {
    fl::EntityPool pool;
    CHECK(pool.get(fl::EntityId::null()) == nullptr);

    auto id = pool.alloc();
    pool.free(id);
    CHECK(pool.get(id) == nullptr);
}

TEST_CASE("EntityPool: free silently ignores already-freed ID", "[entity_pool]") {
    fl::EntityPool pool;
    auto id = pool.alloc();
    pool.free(id);
    REQUIRE_NOTHROW(pool.free(id)); // second free must not crash
    CHECK(pool.liveCount() == 0);
}

// ---------------------------------------------------------------------------
// EntityTypeRegistry
// ---------------------------------------------------------------------------

TEST_CASE("EntityTypeRegistry: registerType returns sequential indices", "[registry]") {
    fl::EntityTypeRegistry reg;
    uint32_t i0 = reg.registerType(makeAirVehicleDef("a:0"));
    uint32_t i1 = reg.registerType(makeAirVehicleDef("a:1"));
    CHECK(i0 == 0);
    CHECK(i1 == 1);
    CHECK(reg.typeCount() == 2);
}

TEST_CASE("EntityTypeRegistry: duplicate id returns max sentinel", "[registry]") {
    fl::EntityTypeRegistry reg;
    reg.registerType(makeAirVehicleDef("dup:x"));
    uint32_t result = reg.registerType(makeAirVehicleDef("dup:x"));
    CHECK(result == std::numeric_limits<uint32_t>::max());
    CHECK(reg.typeCount() == 1);
}

TEST_CASE("EntityTypeRegistry: findById returns correct def or nullptr", "[registry]") {
    fl::EntityTypeRegistry reg;
    reg.registerType(makeAirVehicleDef("reg:f15"));
    CHECK(reg.findById("reg:f15") != nullptr);
    CHECK(reg.findById("reg:f15")->id == "reg:f15");
    CHECK(reg.findById("reg:unknown") == nullptr);
}

TEST_CASE("EntityTypeRegistry: indexById returns max sentinel for unknown id", "[registry]") {
    fl::EntityTypeRegistry reg;
    reg.registerType(makeAirVehicleDef("reg:x"));
    CHECK(reg.indexById("reg:x") == 0);
    CHECK(reg.indexById("reg:missing") == std::numeric_limits<uint32_t>::max());
}

TEST_CASE("EntityTypeRegistry: byIndex returns nullptr out of range", "[registry]") {
    fl::EntityTypeRegistry reg;
    reg.registerType(makeAirVehicleDef("reg:y"));
    CHECK(reg.byIndex(0) != nullptr);
    CHECK(reg.byIndex(1) == nullptr);
    CHECK(reg.byIndex(std::numeric_limits<uint32_t>::max()) == nullptr);
}

TEST_CASE("EntityTypeRegistry: clear resets count and lookups", "[registry]") {
    fl::EntityTypeRegistry reg;
    reg.registerType(makeAirVehicleDef("clr:a"));
    reg.clear();
    CHECK(reg.typeCount() == 0);
    CHECK(reg.findById("clr:a") == nullptr);
}

// ---------------------------------------------------------------------------
// EntityDefParser
// ---------------------------------------------------------------------------

TEST_CASE("EntityDefParser: minimal TOML without damage section", "[parser]") {
    fl::EntityDef def = fl::parseEntityDef(kMinimalEntityToml);
    CHECK(def.id == "test:fighter");
    CHECK(def.name == "Test Fighter");
    CHECK(def.category == fl::ObjectCategory::AirVehicle);
    CHECK_THAT(def.maxHp, WithinAbs(100.f, 1e-4f));
    CHECK(def.mesh == "aircraft/test");
    CHECK_FALSE(def.damage.has_value());
    CHECK(def.classicDamageMesh.empty());
}

TEST_CASE("EntityDefParser: full TOML with damage and classic sections", "[parser]") {
    fl::EntityDef def = fl::parseEntityDef(kFullEntityToml);
    CHECK(def.id == "test:tank");
    CHECK(def.category == fl::ObjectCategory::GroundVehicle);
    REQUIRE(def.damage.has_value());
    CHECK_THAT(def.damage->light.hpFraction, WithinAbs(0.75f, 1e-4f));
    CHECK_THAT(def.damage->heavy.hpFraction, WithinAbs(0.40f, 1e-4f));
    CHECK_THAT(def.damage->critical.hpFraction, WithinAbs(0.15f, 1e-4f));
    CHECK(def.damage->critical.avionicsFailure == true);
    CHECK(def.damage->light.visualEffect == "smoke_light");
    CHECK(def.classicDamageMesh == "ground/tank_damaged");
}

TEST_CASE("EntityDefParser: all category strings are accepted", "[parser]") {
    const char* categories[] = {"air_vehicle", "ground_vehicle", "naval_vehicle", "projectile", "effect", "player"};
    for (const char* cat : categories) {
        std::string toml = std::string("[entity]\nid=\"x:x\"\nname=\"X\"\ncategory=\"") + cat + "\"\nmax_hp=1.0\n";
        REQUIRE_NOTHROW(fl::parseEntityDef(toml));
    }
}

TEST_CASE("EntityDefParser: invalid category throws runtime_error", "[parser]") {
    const char* toml = "[entity]\nid=\"x\"\nname=\"X\"\ncategory=\"submarine\"\nmax_hp=1.0\n";
    CHECK_THROWS_AS(fl::parseEntityDef(toml), std::runtime_error);
}

TEST_CASE("EntityDefParser: missing entity.id throws runtime_error", "[parser]") {
    const char* toml = "[entity]\nname=\"X\"\ncategory=\"air_vehicle\"\nmax_hp=1.0\n";
    CHECK_THROWS_AS(fl::parseEntityDef(toml), std::runtime_error);
}

TEST_CASE("EntityDefParser: missing entity.name throws runtime_error", "[parser]") {
    const char* toml = "[entity]\nid=\"x\"\ncategory=\"air_vehicle\"\nmax_hp=1.0\n";
    CHECK_THROWS_AS(fl::parseEntityDef(toml), std::runtime_error);
}

TEST_CASE("EntityDefParser: missing entity.category throws runtime_error", "[parser]") {
    const char* toml = "[entity]\nid=\"x\"\nname=\"X\"\nmax_hp=1.0\n";
    CHECK_THROWS_AS(fl::parseEntityDef(toml), std::runtime_error);
}

TEST_CASE("EntityDefParser: missing entity.max_hp throws runtime_error", "[parser]") {
    const char* toml = "[entity]\nid=\"x\"\nname=\"X\"\ncategory=\"air_vehicle\"\n";
    CHECK_THROWS_AS(fl::parseEntityDef(toml), std::runtime_error);
}

TEST_CASE("EntityDefParser: missing [entity] table throws runtime_error", "[parser]") {
    CHECK_THROWS_AS(fl::parseEntityDef("[other]\nfoo=1\n"), std::runtime_error);
}

TEST_CASE("EntityDefParser: hp_fraction zero in damage section throws runtime_error", "[parser]") {
    const char* toml = R"(
[entity]
id="x" name="X" category="air_vehicle" max_hp=100.0
[damage.light]
hp_fraction=0.0
[damage.heavy]
hp_fraction=0.4
[damage.critical]
hp_fraction=0.15
)";
    CHECK_THROWS_AS(fl::parseEntityDef(toml), std::runtime_error);
}

TEST_CASE("EntityDefParser: absent optional [classic] section leaves classicDamageMesh empty", "[parser]") {
    fl::EntityDef def = fl::parseEntityDef(kMinimalEntityToml);
    CHECK(def.classicDamageMesh.empty());
}

TEST_CASE("EntityDefParser: invalid TOML syntax throws runtime_error", "[parser]") {
    CHECK_THROWS_AS(fl::parseEntityDef("not valid toml {{{"), std::runtime_error);
}

// ---------------------------------------------------------------------------
// EntityManager
// ---------------------------------------------------------------------------

TEST_CASE("EntityManager: spawn with registered type returns valid ID", "[manager]") {
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeAirVehicleDef("mgr:a"));
    fl::EntityManager mgr(logger, registry);

    fl::EntityTransform t{};
    auto id = mgr.spawn("mgr:a", t);
    CHECK(id.valid());
    CHECK(mgr.liveCount() == 0); // updated on next tick
}

TEST_CASE("EntityManager: spawn with unknown type returns null and logs Warn", "[manager]") {
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    fl::EntityManager mgr(logger, registry);

    fl::EntityTransform t{};
    auto id = mgr.spawn("unknown:type", t);
    CHECK_FALSE(id.valid());
    CHECK(logger.hasMessage(LogLevel::Warn, "unknown type"));
}

TEST_CASE("EntityManager: liveCount updated after onTick", "[manager]") {
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeAirVehicleDef("mgr:b"));
    fl::EntityManager mgr(logger, registry);

    fl::EntityTransform t{};
    mgr.spawn("mgr:b", t);
    mgr.spawn("mgr:b", t);
    mgr.onTick(1.0 / 60.0, 0);
    CHECK(mgr.liveCount() == 2);
}

TEST_CASE("EntityManager: kill fires Died event and reaps entity after tick", "[manager]") {
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeAirVehicleDef("mgr:c"));
    fl::EntityManager mgr(logger, registry);

    struct Collector : fl::IEntityEventHandler {
        std::vector<fl::EntityEvent> events;
        void onEntityEvent(const fl::EntityEvent& e) override {
            events.push_back(e);
        }
    } collector;
    mgr.addEventHandler(&collector);

    fl::EntityTransform t{};
    auto id = mgr.spawn("mgr:c", t);
    mgr.kill(id);

    REQUIRE(collector.events.size() == 1);
    CHECK(collector.events[0].type == fl::EntityEventType::Died);
    CHECK(collector.events[0].subject == id);

    // Entity reaped after tick
    mgr.onTick(1.0 / 60.0, 0);
    CHECK(mgr.liveCount() == 0);
    CHECK(mgr.get(id) == nullptr);
}

TEST_CASE("EntityManager: kill with valid instigator fires ScoreAwarded", "[manager]") {
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeAirVehicleDef("mgr:d"));
    fl::EntityManager mgr(logger, registry);

    struct Collector : fl::IEntityEventHandler {
        std::vector<fl::EntityEvent> events;
        void onEntityEvent(const fl::EntityEvent& e) override {
            events.push_back(e);
        }
    } collector;
    mgr.addEventHandler(&collector);

    fl::EntityTransform t{};
    auto victim = mgr.spawn("mgr:d", t);
    auto instigator = mgr.spawn("mgr:d", t);
    mgr.kill(victim, instigator);

    bool hasDied = false;
    bool hasScore = false;
    for (const auto& ev : collector.events) {
        if (ev.type == fl::EntityEventType::Died)
            hasDied = true;
        if (ev.type == fl::EntityEventType::ScoreAwarded)
            hasScore = true;
    }
    CHECK(hasDied);
    CHECK(hasScore);
}

TEST_CASE("EntityManager: kill without instigator does not fire ScoreAwarded", "[manager]") {
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeAirVehicleDef("mgr:e"));
    fl::EntityManager mgr(logger, registry);

    struct Collector : fl::IEntityEventHandler {
        int scoreCount = 0;
        void onEntityEvent(const fl::EntityEvent& e) override {
            if (e.type == fl::EntityEventType::ScoreAwarded)
                ++scoreCount;
        }
    } collector;
    mgr.addEventHandler(&collector);

    fl::EntityTransform t{};
    auto id = mgr.spawn("mgr:e", t);
    mgr.kill(id); // no instigator
    CHECK(collector.scoreCount == 0);
}

TEST_CASE("EntityManager: applyDamage reduces HP and transitions damage levels", "[manager]") {
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDefWithDamage("mgr:f"));
    fl::EntityManager mgr(logger, registry);

    struct Collector : fl::IEntityEventHandler {
        std::vector<fl::EntityEvent> events;
        void onEntityEvent(const fl::EntityEvent& e) override {
            events.push_back(e);
        }
    } collector;
    mgr.addEventHandler(&collector);

    fl::EntityTransform t{};
    auto id = mgr.spawn("mgr:f", t);

    // Intact → Light (cross 75%)
    mgr.applyDamage(id, 26.f); // 100 - 26 = 74 → 74% < 75%
    REQUIRE(!collector.events.empty());
    CHECK(collector.events.back().type == fl::EntityEventType::DamageLevelChanged);
    CHECK(collector.events.back().newDamageLevel == fl::DamageLevel::Light);
    collector.events.clear();

    // Light → Heavy (cross 40%)
    mgr.applyDamage(id, 35.f); // 74 - 35 = 39 → 39% < 40%
    REQUIRE(!collector.events.empty());
    CHECK(collector.events.back().newDamageLevel == fl::DamageLevel::Heavy);
    collector.events.clear();

    // Heavy → Critical (cross 15%)
    mgr.applyDamage(id, 25.f); // 39 - 25 = 14 → 14% < 15%
    REQUIRE(!collector.events.empty());
    CHECK(collector.events.back().newDamageLevel == fl::DamageLevel::Critical);
}

TEST_CASE("EntityManager: applyDamage to zero HP kills entity", "[manager]") {
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDefWithDamage("mgr:g"));
    fl::EntityManager mgr(logger, registry);

    struct Collector : fl::IEntityEventHandler {
        bool died = false;
        void onEntityEvent(const fl::EntityEvent& e) override {
            if (e.type == fl::EntityEventType::Died)
                died = true;
        }
    } collector;
    mgr.addEventHandler(&collector);

    fl::EntityTransform t{};
    auto id = mgr.spawn("mgr:g", t);
    mgr.applyDamage(id, 200.f); // overkill
    CHECK(collector.died);

    mgr.onTick(1.0 / 60.0, 0);
    CHECK(mgr.liveCount() == 0);
}

TEST_CASE("EntityManager: applyDamage is no-op on dead entity", "[manager]") {
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeAirVehicleDef("mgr:h"));
    fl::EntityManager mgr(logger, registry);

    struct Collector : fl::IEntityEventHandler {
        int diedCount = 0;
        void onEntityEvent(const fl::EntityEvent& e) override {
            if (e.type == fl::EntityEventType::Died)
                ++diedCount;
        }
    } collector;
    mgr.addEventHandler(&collector);

    fl::EntityTransform t{};
    auto id = mgr.spawn("mgr:h", t);
    mgr.kill(id);
    mgr.applyDamage(id, 50.f);       // should be no-op
    CHECK(collector.diedCount == 1); // only one death event
}

TEST_CASE("EntityManager: setSoftCap prevents spawn beyond cap", "[manager]") {
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeAirVehicleDef("mgr:i"));
    fl::EntityManager mgr(logger, registry);
    mgr.setSoftCap(2);

    fl::EntityTransform t{};
    auto a = mgr.spawn("mgr:i", t);
    auto b = mgr.spawn("mgr:i", t);
    auto c = mgr.spawn("mgr:i", t); // should fail
    CHECK(a.valid());
    CHECK(b.valid());
    CHECK_FALSE(c.valid());
}

TEST_CASE("EntityManager: removeEventHandler stops delivery", "[manager]") {
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeAirVehicleDef("mgr:j"));
    fl::EntityManager mgr(logger, registry);

    struct Collector : fl::IEntityEventHandler {
        int count = 0;
        void onEntityEvent(const fl::EntityEvent&) override {
            ++count;
        }
    } collector;
    mgr.addEventHandler(&collector);

    fl::EntityTransform t{};
    auto a = mgr.spawn("mgr:j", t);
    mgr.kill(a);
    CHECK(collector.count >= 1);

    int before = collector.count;
    mgr.removeEventHandler(&collector);
    auto b = mgr.spawn("mgr:j", t);
    mgr.onTick(1.0 / 60.0, 0);
    mgr.kill(b);
    CHECK(collector.count == before); // no new events after removal
}

TEST_CASE("EntityManager: reapDeadEntities does nothing when list is empty", "[manager]") {
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    fl::EntityManager mgr(logger, registry);

    // Tick with no entities — must not crash
    REQUIRE_NOTHROW(mgr.onTick(1.0 / 60.0, 0));
    CHECK(mgr.liveCount() == 0);
}
