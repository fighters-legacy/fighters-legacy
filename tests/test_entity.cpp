// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "ILogger.h"

#include "entity/DamageApplication.h"
#include "entity/DamageDef.h"
#include "entity/EntityDefParser.h"
#include "entity/EntityEvent.h"
#include "entity/EntityId.h"
#include "entity/EntityManager.h"
#include "entity/EntityPool.h"
#include "entity/EntityState.h"
#include "entity/EntityTypeRegistry.h"
#include "entity/ObjectCategory.h"
#include "render/SimRenderBridge.h"
#include "spatial/SpatialIndex.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using Catch::Matchers::WithinAbs;
using namespace fl;

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
id           = "test:tank"
name         = "Test Tank"
category     = "ground_vehicle"
max_hp       = 200.0
mesh         = "ground/tank"
flight_model = "models/tank_drive"

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
// EntityState
// ---------------------------------------------------------------------------

TEST_CASE("EntityState: factionIndex defaults to neutral", "[entity_state]") {
    EntityState s{};
    CHECK(s.factionIndex == 0u);
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
    CHECK(std::string(fl::objectCategoryName(fl::ObjectCategory::Structure)) == "structure");
}

TEST_CASE("ObjectCategory/ProjectileKind ordinal gates accept the range and nothing past it", "[object_category]") {
    CHECK(fl::isObjectCategoryOrdinal(static_cast<uint8_t>(fl::ObjectCategory::AirVehicle)));
    CHECK(fl::isObjectCategoryOrdinal(static_cast<uint8_t>(fl::ObjectCategory::Structure)));
    CHECK_FALSE(fl::isObjectCategoryOrdinal(static_cast<uint8_t>(fl::ObjectCategory::Structure) + 1));
    CHECK_FALSE(fl::isObjectCategoryOrdinal(0xFF));
    CHECK(fl::isProjectileKindOrdinal(static_cast<uint8_t>(fl::ProjectileKind::None)));
    CHECK(fl::isProjectileKindOrdinal(static_cast<uint8_t>(fl::ProjectileKind::Rocket)));
    CHECK_FALSE(fl::isProjectileKindOrdinal(static_cast<uint8_t>(fl::ProjectileKind::Rocket) + 1));
    CHECK_FALSE(fl::isProjectileKindOrdinal(0xFF));
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

TEST_CASE("EntityPool: getByIndex returns the live slot or nullptr", "[entity_pool]") {
    fl::EntityPool pool;
    fl::EntityId a = pool.alloc();
    REQUIRE(a.valid());
    pool.get(a)->factionIndex = 7;

    // Live slot: returns the state at that index.
    const fl::EntityState* s = pool.getByIndex(a.index);
    REQUIRE(s != nullptr);
    CHECK(s->factionIndex == 7);

    // Out-of-range index: nullptr.
    CHECK(pool.getByIndex(a.index + 1000u) == nullptr);

    // Freed slot: nullptr (not alive), even though the index is in range.
    pool.free(a);
    CHECK(pool.getByIndex(a.index) == nullptr);
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

// --- dense O(liveCount) iteration: swap-remove correctness (issue #573) ------------------------
// free() does an O(1) swap-remove on the live-index list, so forEach visits exactly the live set
// regardless of which slot is freed (middle / last / only / free-list head) and its order is
// free-history-dependent. These lock the invariant every forEach consumer relies on.

namespace {
// Collect the set of live entity indices via forEach (order-independent).
std::set<uint32_t> liveIndexSet(const fl::EntityPool& pool) {
    std::set<uint32_t> s;
    pool.forEach([&](const fl::EntityState& e) { s.insert(e.id.index); });
    return s;
}
} // namespace

TEST_CASE("EntityPool: forEach visits exact live set when freeing a MIDDLE slot", "[entity_pool]") {
    fl::EntityPool pool;
    auto a = pool.alloc();
    auto b = pool.alloc();
    auto c = pool.alloc();
    pool.free(b); // middle of the live list
    CHECK(pool.liveCount() == 2);
    CHECK(liveIndexSet(pool) == std::set<uint32_t>{a.index, c.index});
}

TEST_CASE("EntityPool: forEach visits exact live set when freeing the LAST slot", "[entity_pool]") {
    fl::EntityPool pool;
    auto a = pool.alloc();
    auto b = pool.alloc();
    auto c = pool.alloc();
    pool.free(c); // tail of the live list — swap-remove is a self-move
    CHECK(pool.liveCount() == 2);
    CHECK(liveIndexSet(pool) == std::set<uint32_t>{a.index, b.index});
}

TEST_CASE("EntityPool: forEach empty after freeing the ONLY slot", "[entity_pool]") {
    fl::EntityPool pool;
    auto a = pool.alloc();
    pool.free(a);
    CHECK(pool.liveCount() == 0);
    CHECK(liveIndexSet(pool).empty());
}

TEST_CASE("EntityPool: free-then-realloc reuses the slot and stays iterable", "[entity_pool]") {
    fl::EntityPool pool;
    auto a = pool.alloc();
    auto b = pool.alloc();
    pool.free(a);          // a.index becomes the free-list head
    auto c = pool.alloc(); // must reuse a.index with a bumped generation
    CHECK(c.index == a.index);
    CHECK(c.generation != a.generation);
    CHECK(pool.liveCount() == 2);
    CHECK(liveIndexSet(pool) == std::set<uint32_t>{b.index, c.index});
    CHECK_FALSE(pool.valid(a)); // stale handle stays invalid
}

TEST_CASE("EntityPool: heavy interleaved churn keeps forEach == live set", "[entity_pool]") {
    fl::EntityPool pool(8);
    std::vector<fl::EntityId> live;
    std::mt19937 rng(1234);
    for (int step = 0; step < 4000; ++step) {
        // Bias toward growth early, churn later.
        const bool doAlloc = live.empty() || (rng() % 100) < 55;
        if (doAlloc) {
            live.push_back(pool.alloc());
        } else {
            auto pick = rng() % live.size();
            pool.free(live[pick]);
            live[pick] = live.back();
            live.pop_back();
        }
    }
    // Expected live index set from the bookkeeping vector.
    std::set<uint32_t> expected;
    for (auto id : live) {
        REQUIRE(pool.valid(id));
        expected.insert(id.index);
    }
    CHECK(pool.liveCount() == static_cast<uint32_t>(live.size()));
    CHECK(liveIndexSet(pool) == expected);
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
    CHECK(def.cockpitMesh.empty());      // optional field defaults empty -> no cockpit geometry
    CHECK(def.flightModelAsset.empty()); // optional field defaults empty -> builtin model
}

TEST_CASE("EntityDefParser: entity.cockpit parses into cockpitMesh", "[parser]") {
    // The cockpit belongs to the ENTITY, not to the flight model (#813) -- a flight model is
    // aerodynamics and does not know what it looks like.
    const std::string toml = "[entity]\n"
                             "id = \"test:fighter\"\n"
                             "name = \"Test Fighter\"\n"
                             "category = \"air_vehicle\"\n"
                             "max_hp = 100.0\n"
                             "mesh = \"f5e\"\n"
                             "cockpit = \"f5e_cockpit\"\n";
    fl::EntityDef def = fl::parseEntityDef(toml);
    CHECK(def.mesh == "f5e");
    CHECK(def.cockpitMesh == "f5e_cockpit");
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
    CHECK(def.flightModelAsset == "models/tank_drive");
}

TEST_CASE("EntityDefParser: no subsystems section leaves the 3-level model intact (#675)", "[parser]") {
    // Regression guard: an entity that does not declare [damage.subsystems] behaves exactly as
    // before — the optional stays empty.
    fl::EntityDef def = fl::parseEntityDef(kFullEntityToml);
    REQUIRE(def.damage.has_value());
    CHECK_FALSE(def.damage->subsystems.has_value());
}

TEST_CASE("EntityDefParser: [damage.subsystems] parses the fixed vocabulary (#675)", "[parser]") {
    const char* toml = R"(
[entity]
id = "test:twin"
name = "Twin"
category = "air_vehicle"
max_hp = 100.0

[damage.light]
hp_fraction = 0.75

[damage.heavy]
hp_fraction = 0.4

[damage.critical]
hp_fraction = 0.15

[damage.subsystems.engine_left]
hp = 40
weight = 2.0

[damage.subsystems.engine_right]
hp = 40
weight = 2.0

[damage.subsystems.avionics]
hp = 15
weight = 1.0
)";
    fl::EntityDef def = fl::parseEntityDef(toml);
    REQUIRE(def.damage.has_value());
    REQUIRE(def.damage->subsystems.has_value());
    const fl::SubsystemSet& s = *def.damage->subsystems;
    CHECK(s[fl::Subsystem::EngineLeft].hp == 40.f);
    CHECK(s[fl::Subsystem::EngineLeft].weight == 2.f);
    CHECK(s[fl::Subsystem::EngineRight].hp == 40.f);
    CHECK(s[fl::Subsystem::Avionics].hp == 15.f);
    // Omitted subsystems stay absent (hp 0 = not modelled).
    CHECK(s[fl::Subsystem::Controls].hp == 0.f);
    CHECK(s[fl::Subsystem::Fuel].hp == 0.f);
    CHECK(s[fl::Subsystem::Engine].hp == 0.f); // twin content does not use the centreline pool
    CHECK(s.any());
}

TEST_CASE("EntityDefParser: [damage.subsystems.engine] parses the centreline pool (#901)", "[parser]") {
    const char* toml = R"(
[entity]
id = "test:single"
name = "Single"
category = "air_vehicle"
max_hp = 100.0

[damage.light]
hp_fraction = 0.75

[damage.heavy]
hp_fraction = 0.4

[damage.critical]
hp_fraction = 0.15

[damage.subsystems.engine]
hp = 60
weight = 2.5
)";
    fl::EntityDef def = fl::parseEntityDef(toml);
    REQUIRE(def.damage.has_value());
    REQUIRE(def.damage->subsystems.has_value());
    const fl::SubsystemSet& s = *def.damage->subsystems;
    CHECK(s[fl::Subsystem::Engine].hp == 60.f);
    CHECK(s[fl::Subsystem::Engine].weight == 2.5f);
    // A single-engine airframe uses the centreline pool, not the twin L/R pools.
    CHECK(s[fl::Subsystem::EngineLeft].hp == 0.f);
    CHECK(s[fl::Subsystem::EngineRight].hp == 0.f);
}

TEST_CASE("EntityDefParser: all category strings are accepted", "[parser]") {
    const char* categories[] = {"air_vehicle", "ground_vehicle", "naval_vehicle", "projectile",
                                "effect",      "player",         "structure"};
    for (const char* cat : categories) {
        std::string toml = std::string("[entity]\nid=\"x:x\"\nname=\"X\"\ncategory=\"") + cat + "\"\nmax_hp=1.0\n";
        REQUIRE_NOTHROW(fl::parseEntityDef(toml));
    }
}

TEST_CASE("EntityDefParser: projectile_kind parses, defaults, and rejects misuse", "[parser]") {
    // Explicit kind on a projectile def.
    fl::EntityDef bomb = fl::parseEntityDef("[entity]\nid=\"x:b\"\nname=\"B\"\ncategory=\"projectile\"\n"
                                            "projectile_kind=\"bomb\"\nmax_hp=1.0\n");
    CHECK(bomb.projectileKind == fl::ProjectileKind::Bomb);
    fl::EntityDef rocket = fl::parseEntityDef("[entity]\nid=\"x:r\"\nname=\"R\"\ncategory=\"projectile\"\n"
                                              "projectile_kind=\"rocket\"\nmax_hp=1.0\n");
    CHECK(rocket.projectileKind == fl::ProjectileKind::Rocket);

    // A projectile without the key defaults to missile (the common case).
    fl::EntityDef dflt = fl::parseEntityDef("[entity]\nid=\"x:m\"\nname=\"M\"\ncategory=\"projectile\"\nmax_hp=1.0\n");
    CHECK(dflt.projectileKind == fl::ProjectileKind::Missile);

    // A non-projectile stays None.
    fl::EntityDef air = fl::parseEntityDef("[entity]\nid=\"x:a\"\nname=\"A\"\ncategory=\"air_vehicle\"\nmax_hp=1.0\n");
    CHECK(air.projectileKind == fl::ProjectileKind::None);

    // The key on a non-projectile category is an error, not silently-dead data.
    CHECK_THROWS_AS(fl::parseEntityDef("[entity]\nid=\"x:a\"\nname=\"A\"\ncategory=\"air_vehicle\"\n"
                                       "projectile_kind=\"missile\"\nmax_hp=1.0\n"),
                    std::runtime_error);
    // An unknown kind value is an error.
    CHECK_THROWS_AS(fl::parseEntityDef("[entity]\nid=\"x:p\"\nname=\"P\"\ncategory=\"projectile\"\n"
                                       "projectile_kind=\"torpedo\"\nmax_hp=1.0\n"),
                    std::runtime_error);
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

// ── hardpoints (#623) ────────────────────────────────────────────────────────
// Weapon stations are a property of the entity, not of its flight model.

namespace {

std::string entityWithHardpoints(const char* hardpoints) {
    return std::string("[entity]\nid=\"x\"\nname=\"X\"\ncategory=\"air_vehicle\"\nmax_hp=100.0\n") + hardpoints;
}

} // namespace

TEST_CASE("EntityDefParser: hardpoints are optional", "[parser]") {
    const fl::EntityDef def = fl::parseEntityDef(entityWithHardpoints(""));
    CHECK(def.hardpoints.empty());
}

TEST_CASE("EntityDefParser: parses a hardpoint array", "[parser]") {
    const fl::EntityDef def = fl::parseEntityDef(entityWithHardpoints(R"(
[[hardpoints]]
slot    = 0
type    = "missile"
allowed = ["aim120c", "aim9x"]
default = "aim120c"

[[hardpoints]]
slot    = 4
type    = "bomb"
allowed = ["gbu12"]
default = "gbu12"
)"));

    REQUIRE(def.hardpoints.size() == 2);

    CHECK(def.hardpoints[0].slot == 0);
    CHECK(def.hardpoints[0].allowed == std::vector<std::string>{"aim120c", "aim9x"});
    CHECK(def.hardpoints[0].defaultWeapon == "aim120c");

    CHECK(def.hardpoints[1].slot == 4);
    CHECK(def.hardpoints[1].defaultWeapon == "gbu12");
}

TEST_CASE("EntityDefParser: the legacy hardpoint type key is accepted and ignored", "[parser]") {
    // Stations no longer have a kind of their own -- allowed IS the compatibility contract -- but
    // pre-existing content declares `type`, so the key must keep parsing. ANY string is fine,
    // including ones the old enum rejected: the field is dead, not validated.
    for (const char* type : {"missile", "bomb", "rocket", "gun", "fuel", "pod", "railgun"}) {
        const std::string toml = entityWithHardpoints(
            (std::string("[[hardpoints]]\nslot=0\ntype=\"") + type + "\"\nallowed=[\"w\"]\ndefault=\"w\"\n").c_str());
        REQUIRE_NOTHROW(fl::parseEntityDef(toml));
    }
}

TEST_CASE("EntityDefParser: a hardpoint with no type key parses", "[parser]") {
    const std::string toml = entityWithHardpoints("[[hardpoints]]\nslot=0\nallowed=[\"w\"]\ndefault=\"w\"\n");
    REQUIRE_NOTHROW(fl::parseEntityDef(toml));
}

TEST_CASE("EntityDefParser: duplicate hardpoint slot throws runtime_error", "[parser]") {
    const std::string toml = entityWithHardpoints(R"(
[[hardpoints]]
slot=0
type="missile"
allowed=["w"]
default="w"

[[hardpoints]]
slot=0
type="bomb"
allowed=["b"]
default="b"
)");
    CHECK_THROWS_AS(fl::parseEntityDef(toml), std::runtime_error);
}

TEST_CASE("EntityDefParser: negative hardpoint slot throws runtime_error", "[parser]") {
    const std::string toml =
        entityWithHardpoints("[[hardpoints]]\nslot=-1\ntype=\"missile\"\nallowed=[\"w\"]\ndefault=\"w\"\n");
    CHECK_THROWS_AS(fl::parseEntityDef(toml), std::runtime_error);
}

TEST_CASE("EntityDefParser: empty hardpoint allowed list throws runtime_error", "[parser]") {
    const std::string toml =
        entityWithHardpoints("[[hardpoints]]\nslot=0\ntype=\"missile\"\nallowed=[]\ndefault=\"w\"\n");
    CHECK_THROWS_AS(fl::parseEntityDef(toml), std::runtime_error);
}

TEST_CASE("EntityDefParser: hardpoint default outside allowed throws runtime_error", "[parser]") {
    const std::string toml =
        entityWithHardpoints("[[hardpoints]]\nslot=0\ntype=\"missile\"\nallowed=[\"aim9x\"]\ndefault=\"aim120c\"\n");
    CHECK_THROWS_AS(fl::parseEntityDef(toml), std::runtime_error);
}

TEST_CASE("EntityDefParser: missing hardpoint default throws runtime_error", "[parser]") {
    const std::string toml = entityWithHardpoints("[[hardpoints]]\nslot=0\ntype=\"missile\"\nallowed=[\"aim9x\"]\n");
    CHECK_THROWS_AS(fl::parseEntityDef(toml), std::runtime_error);
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

TEST_CASE("EntityDefParser: minimal TOML leaves aiScriptAsset empty", "[parser]") {
    fl::EntityDef def = fl::parseEntityDef(kMinimalEntityToml);
    CHECK(def.aiScriptAsset.empty());
}

TEST_CASE("EntityDefParser: ai_script field is parsed when present", "[parser]") {
    static const char* kTomlWithScript = R"(
[entity]
id        = "test:bot"
name      = "Bot"
category  = "air_vehicle"
max_hp    = 100.0
mesh      = "aircraft/bot"
ai_script = "patrol"
)";
    fl::EntityDef def = fl::parseEntityDef(kTomlWithScript);
    CHECK(def.aiScriptAsset == "patrol");
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

// ---------------------------------------------------------------------------
// EntityManager — render bridge integration
// ---------------------------------------------------------------------------

TEST_CASE("EntityManager: setRenderBridge enables snapshot publish on tick", "[manager]") {
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeAirVehicleDef("mgr:snap_a"));
    fl::EntityManager mgr(logger, registry);

    fl::SimRenderBridge bridge;
    mgr.setRenderBridge(&bridge);

    fl::EntityTransform t{};
    t.pos[0] = 10.f;
    t.pos[1] = 20.f;
    t.pos[2] = 30.f;
    t.vel[0] = 5.f;
    auto id = mgr.spawn("mgr:snap_a", t);
    REQUIRE(id.valid());

    mgr.onTick(1.0 / 60.0, 42);

    REQUIRE(bridge.tryAdvance());
    const auto& snap = bridge.current();
    CHECK(snap.tickIndex == 42);
    REQUIRE(snap.entries.size() == 1);
    CHECK(snap.entries[0].entityIdx == id.index);
    CHECK(snap.entries[0].entityGen == id.generation);
    CHECK(snap.entries[0].position.x == 10.f);
    CHECK(snap.entries[0].position.y == 20.f);
    CHECK(snap.entries[0].position.z == 30.f);
    CHECK(snap.entries[0].velocity.x == 5.f);
}

TEST_CASE("EntityManager: snapshot contains damageLevel and playerOwned", "[manager]") {
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeDefWithDamage("mgr:snap_b"));
    fl::EntityManager mgr(logger, registry);

    fl::SimRenderBridge bridge;
    mgr.setRenderBridge(&bridge);

    fl::EntityTransform t{};
    auto id = mgr.spawn("mgr:snap_b", t);
    fl::EntityState* s = mgr.get(id);
    REQUIRE(s != nullptr);
    s->playerOwned = true;

    // Apply damage to move to Heavy level (cross 40%)
    mgr.applyDamage(id, 61.f); // 100 - 61 = 39 < 40%

    mgr.onTick(1.0 / 60.0, 1);

    REQUIRE(bridge.tryAdvance());
    REQUIRE(bridge.current().entries.size() == 1);
    const auto& e = bridge.current().entries[0];
    CHECK(e.damageLevel == static_cast<uint8_t>(fl::DamageLevel::Heavy));
    CHECK(e.playerOwned == true);
}

TEST_CASE("EntityManager: snapshot is empty when no entities are live", "[manager]") {
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    fl::EntityManager mgr(logger, registry);

    fl::SimRenderBridge bridge;
    mgr.setRenderBridge(&bridge);

    mgr.onTick(1.0 / 60.0, 1);

    REQUIRE(bridge.tryAdvance());
    CHECK(bridge.current().entries.empty());
}

TEST_CASE("EntityManager: dead entities are absent from snapshot", "[manager]") {
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeAirVehicleDef("mgr:snap_c"));
    fl::EntityManager mgr(logger, registry);

    fl::SimRenderBridge bridge;
    mgr.setRenderBridge(&bridge);

    fl::EntityTransform t{};
    auto a = mgr.spawn("mgr:snap_c", t);
    auto b = mgr.spawn("mgr:snap_c", t);
    mgr.kill(a); // reaped in next tick

    mgr.onTick(1.0 / 60.0, 1);

    REQUIRE(bridge.tryAdvance());
    // Only b survives
    REQUIRE(bridge.current().entries.size() == 1);
    CHECK(bridge.current().entries[0].entityIdx == b.index);
}

TEST_CASE("EntityManager: setRenderBridge nullptr suppresses publish", "[manager]") {
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeAirVehicleDef("mgr:snap_d"));
    fl::EntityManager mgr(logger, registry);

    fl::SimRenderBridge bridge;
    mgr.setRenderBridge(&bridge);
    mgr.setRenderBridge(nullptr); // detach

    fl::EntityTransform t{};
    mgr.spawn("mgr:snap_d", t);
    mgr.onTick(1.0 / 60.0, 1);

    CHECK_FALSE(bridge.hasSnapshot());
}

// ---------------------------------------------------------------------------
// EntityDefParser — sensing: [signatures], sensors, [ai] (#680)
// ---------------------------------------------------------------------------

namespace {

// Appends TOML after the required [entity] block. Bare keys land INSIDE [entity]; a table header in
// `extra` starts a new top-level section.
std::string entityWith(const char* extra) {
    return std::string("[entity]\nid=\"x\"\nname=\"X\"\ncategory=\"air_vehicle\"\nmax_hp=100.0\n") + extra;
}

} // namespace

TEST_CASE("EntityDefParser: an entity with no sensing sections is the baseline fighter", "[parser]") {
    // The defaults are load-bearing: a sensor def quotes its ranges against signature 1.0, so an
    // entity that says nothing must be exactly as detectable as those numbers assume.
    const fl::EntityDef def = fl::parseEntityDef(kMinimalEntityToml);

    CHECK_THAT(def.signatures.rcs, WithinAbs(1.0f, 1e-6f));
    CHECK_THAT(def.signatures.ir, WithinAbs(1.0f, 1e-6f));
    CHECK_THAT(def.signatures.visual, WithinAbs(1.0f, 1e-6f));
    CHECK_THAT(def.signatures.laser, WithinAbs(1.0f, 1e-6f));

    // Empty is meaningful: an AI entity with no declared sensors gets the builtin eyeball, not
    // omniscience and not blindness.
    CHECK(def.sensorIds.empty());
    CHECK_FALSE(def.aiTuning.has_value());
}

TEST_CASE("EntityDefParser: parses [signatures], sensors and [ai]", "[parser]") {
    const fl::EntityDef def = fl::parseEntityDef(entityWith(R"(
sensors = ["fl-base:eyeball", "fl-base:apg63"]

[signatures]
rcs    = 0.05
ir     = 0.8
visual = 1.2
laser  = 1.0

[ai]
skill    = 0.9
reaction = 0.2
)"));

    REQUIRE(def.sensorIds.size() == 2);
    CHECK(def.sensorIds[0] == "fl-base:eyeball");
    CHECK(def.sensorIds[1] == "fl-base:apg63");

    CHECK_THAT(def.signatures.rcs, WithinAbs(0.05f, 1e-6f));
    CHECK_THAT(def.signatures.ir, WithinAbs(0.8f, 1e-6f));
    CHECK_THAT(def.signatures.visual, WithinAbs(1.2f, 1e-6f));
    CHECK_THAT(def.signatures.laser, WithinAbs(1.0f, 1e-6f));

    REQUIRE(def.aiTuning.has_value());
    CHECK_THAT(def.aiTuning->skill, WithinAbs(0.9f, 1e-6f));
    CHECK_THAT(def.aiTuning->reaction, WithinAbs(0.2f, 1e-6f));
}

TEST_CASE("EntityDefParser: a partial [signatures] section keeps the baseline for the rest", "[parser]") {
    const fl::EntityDef def = fl::parseEntityDef(entityWith("\n[signatures]\nrcs = 0.01\n"));

    CHECK_THAT(def.signatures.rcs, WithinAbs(0.01f, 1e-6f)); // authored
    CHECK_THAT(def.signatures.ir, WithinAbs(1.0f, 1e-6f));   // a stealth airframe is still hot
    CHECK_THAT(def.signatures.visual, WithinAbs(1.0f, 1e-6f));
    CHECK_THAT(def.signatures.laser, WithinAbs(1.0f, 1e-6f));
}

TEST_CASE("EntityDefParser: a partial [ai] section keeps the default for the other field", "[parser]") {
    const fl::EntityDef def = fl::parseEntityDef(entityWith("\n[ai]\nskill = 1.0\n"));

    REQUIRE(def.aiTuning.has_value());
    CHECK_THAT(def.aiTuning->skill, WithinAbs(1.0f, 1e-6f));
    CHECK_THAT(def.aiTuning->reaction, WithinAbs(0.5f, 1e-6f)); // AiTuning{} default
}

TEST_CASE("EntityDefParser: signature values outside (0, 100] throw runtime_error", "[parser]") {
    // Zero is rejected with the negatives on purpose: a signature of 0 is not "very stealthy", it is
    // a target that sensor type can NEVER detect at any range.
    CHECK_THROWS_AS(fl::parseEntityDef(entityWith("\n[signatures]\nrcs = 0.0\n")), std::runtime_error);
    CHECK_THROWS_AS(fl::parseEntityDef(entityWith("\n[signatures]\nrcs = -1.0\n")), std::runtime_error);
    CHECK_THROWS_AS(fl::parseEntityDef(entityWith("\n[signatures]\nir = 100.1\n")), std::runtime_error);
    CHECK_THROWS_AS(fl::parseEntityDef(entityWith("\n[signatures]\nvisual = -0.5\n")), std::runtime_error);
    CHECK_THROWS_AS(fl::parseEntityDef(entityWith("\n[signatures]\nlaser = 1000.0\n")), std::runtime_error);

    // The boundary itself is legal.
    CHECK_NOTHROW(fl::parseEntityDef(entityWith("\n[signatures]\nrcs = 100.0\n")));
}

TEST_CASE("EntityDefParser: ai skill and reaction outside [0, 1] throw runtime_error", "[parser]") {
    CHECK_THROWS_AS(fl::parseEntityDef(entityWith("\n[ai]\nskill = 1.5\n")), std::runtime_error);
    CHECK_THROWS_AS(fl::parseEntityDef(entityWith("\n[ai]\nskill = -0.1\n")), std::runtime_error);
    CHECK_THROWS_AS(fl::parseEntityDef(entityWith("\n[ai]\nreaction = 2.0\n")), std::runtime_error);

    // Both endpoints are legal: a perfect crew and a hopeless one are both authorable.
    CHECK_NOTHROW(fl::parseEntityDef(entityWith("\n[ai]\nskill = 0.0\nreaction = 1.0\n")));
    CHECK_NOTHROW(fl::parseEntityDef(entityWith("\n[ai]\nskill = 1.0\nreaction = 0.0\n")));
}

TEST_CASE("EntityDefParser: a malformed sensors list throws runtime_error", "[parser]") {
    CHECK_THROWS_AS(fl::parseEntityDef(entityWith("sensors = \"fl-base:apg63\"\n")), std::runtime_error); // not array
    CHECK_THROWS_AS(fl::parseEntityDef(entityWith("sensors = [\"\"]\n")), std::runtime_error);            // empty id
    CHECK_THROWS_AS(fl::parseEntityDef(entityWith("sensors = [1, 2]\n")), std::runtime_error);            // not strings
    CHECK_THROWS_AS(fl::parseEntityDef(entityWith("sensors = [\"a\", \"a\"]\n")), std::runtime_error);    // duplicate

    // An empty list is legal and means the same as omitting it: the entity falls back to the
    // builtin eyeball rather than being blind.
    const fl::EntityDef def = fl::parseEntityDef(entityWith("sensors = []\n"));
    CHECK(def.sensorIds.empty());
}

TEST_CASE("EntityDefParser: an unknown sensor id is NOT a parse error", "[parser]") {
    // Cross-references resolve once the whole pack is read, so an id this parser has never heard of
    // is a resolve-time warning, not a reason to refuse the aircraft.
    const fl::EntityDef def = fl::parseEntityDef(entityWith("sensors = [\"nosuchpack:nosuchsensor\"]\n"));
    REQUIRE(def.sensorIds.size() == 1);
    CHECK(def.sensorIds[0] == "nosuchpack:nosuchsensor");
}

// ---------------------------------------------------------------------------
// Crew seats & turret mounts (#966/#968)
// ---------------------------------------------------------------------------

namespace {
std::string crewEntity(const char* body) {
    return std::string("[entity]\nid=\"x:crewed\"\nname=\"Crewed\"\ncategory=\"air_vehicle\"\nmax_hp=300.0\n") + body;
}
} // namespace

TEST_CASE("EntityDefParser: absent [[crew]] leaves crew empty (implicit single pilot)", "[parser][crew]") {
    // The load-bearing back-compat case: every existing def is a valid 1-seat crewed aircraft with
    // zero content changes.
    const fl::EntityDef def = fl::parseEntityDef(kMinimalEntityToml);
    CHECK(def.crew.empty());
    CHECK(def.turrets.empty());
}

TEST_CASE("EntityDefParser: capability tokens round-trip through the mask", "[parser][crew]") {
    for (fl::CrewCapability cap : fl::allCrewCapabilities()) {
        auto parsed = fl::parseCrewCapability(fl::crewCapabilityName(cap));
        REQUIRE(parsed.has_value());
        CHECK(*parsed == cap);
    }
    CHECK_FALSE(fl::parseCrewCapability("wingman").has_value());
}

TEST_CASE("EntityDefParser: parses a crewed bomber with a tail turret", "[parser][crew]") {
    const fl::EntityDef def = fl::parseEntityDef(crewEntity(R"(
[[hardpoints]]
slot    = 0
allowed = ["gbu12"]
default = "gbu12"

[[hardpoints]]
slot    = 3
allowed = ["m61"]
default = "m61"

[[turrets]]
id              = "tail"
mount_pos       = [0.0, 0.5, -6.0]
az_min_deg      = -60.0
az_max_deg      =  60.0
el_min_deg      = -10.0
el_max_deg      =  80.0
slew_rate_deg_s = 45.0
stations        = [3]

[[crew]]
role         = "pilot"
capabilities = ["fly", "fire", "radar", "countermeasures"]
stations     = [0]
seat_pos     = [0.0, 1.2, 3.0]

[[crew]]
role         = "tail-gunner"
capabilities = ["fire"]
turret       = "tail"
bot          = "gunner"
skill        = 0.6
seat_pos     = [0.0, 0.5, -6.0]
)"));

    REQUIRE(def.crew.size() == 2);
    REQUIRE(def.turrets.size() == 1);

    const fl::SeatDef& pilot = def.crew[0];
    CHECK(pilot.role == "pilot");
    CHECK(fl::hasCapability(pilot.capabilities, fl::CrewCapability::Fly));
    CHECK(fl::hasCapability(pilot.capabilities, fl::CrewCapability::Radar));
    CHECK(fl::hasCapability(pilot.capabilities, fl::CrewCapability::Countermeasures));
    CHECK(fl::hasCapability(pilot.capabilities, fl::CrewCapability::Fire));
    REQUIRE(pilot.stations == std::vector<int>{0});
    CHECK(pilot.defaultOccupancy == fl::SeatOccupancyDefault::Bot);

    const fl::SeatDef& gunner = def.crew[1];
    CHECK(gunner.role == "tail-gunner");
    CHECK(fl::hasCapability(gunner.capabilities, fl::CrewCapability::Fire));
    CHECK(gunner.turret == "tail");
    CHECK(gunner.botSpec == "gunner");
    CHECK_THAT(gunner.defaultSkill, WithinAbs(0.6f, 1e-4f));

    const fl::TurretDef& t = def.turrets[0];
    CHECK(t.id == "tail");
    CHECK_THAT(t.azMinDeg, WithinAbs(-60.f, 1e-4f));
    CHECK_THAT(t.slewRateDegS, WithinAbs(45.f, 1e-4f));
    REQUIRE(t.stations == std::vector<int>{3});
    CHECK_THAT(t.mountPos[2], WithinAbs(-6.f, 1e-4f));

    // Crew-seat damage (#978) is absent here → the seats default to non-damageable (damageHp 0).
    CHECK_THAT(pilot.damageHp, WithinAbs(0.f, 1e-6f));
    CHECK_THAT(gunner.damageHp, WithinAbs(0.f, 1e-6f));
}

TEST_CASE("EntityDefParser: crew-seat damage_hp / hit_weight parse and validate (#978)", "[parser][crew]") {
    const fl::EntityDef def = fl::parseEntityDef(crewEntity(R"(
[[hardpoints]]
slot    = 0
allowed = ["m61"]
default = "m61"
[[crew]]
role         = "pilot"
capabilities = ["fly"]
damage_hp    = 60.0
[[crew]]
role         = "gunner"
capabilities = ["fire"]
stations     = [0]
damage_hp    = 40.0
hit_weight   = 2.5
)"));
    REQUIRE(def.crew.size() == 2);
    CHECK_THAT(def.crew[0].damageHp, WithinAbs(60.f, 1e-4f));
    CHECK_THAT(def.crew[0].hitWeight, WithinAbs(1.f, 1e-4f)); // default
    CHECK_THAT(def.crew[1].damageHp, WithinAbs(40.f, 1e-4f));
    CHECK_THAT(def.crew[1].hitWeight, WithinAbs(2.5f, 1e-4f));

    // A negative damage_hp / non-positive hit_weight is rejected.
    CHECK_THROWS(fl::parseEntityDef(crewEntity(R"(
[[crew]]
role = "pilot"
capabilities = ["fly"]
damage_hp = -5.0
)")));
    CHECK_THROWS(fl::parseEntityDef(crewEntity(R"(
[[crew]]
role = "pilot"
capabilities = ["fly"]
hit_weight = 0.0
)")));
}

TEST_CASE("EntityDefParser: an empty seat is authored with empty = true", "[parser][crew]") {
    const fl::EntityDef def = fl::parseEntityDef(crewEntity(R"(
[[crew]]
role         = "pilot"
capabilities = ["fly"]

[[crew]]
role         = "observer"
capabilities = ["radar"]
empty        = true
)"));
    REQUIRE(def.crew.size() == 2);
    CHECK(def.crew[1].defaultOccupancy == fl::SeatOccupancyDefault::Empty);
    CHECK(def.crew[1].botSpec.empty());
}

TEST_CASE("EntityDefParser: the one-owner-per-channel invariant is enforced", "[parser][crew]") {
    // Zero Fly seats.
    CHECK_THROWS_AS(fl::parseEntityDef(crewEntity("[[crew]]\nrole=\"g\"\ncapabilities=[\"radar\"]\n")),
                    std::runtime_error);
    // Two Fly seats.
    CHECK_THROWS_AS(
        fl::parseEntityDef(
            crewEntity("[[crew]]\nrole=\"a\"\ncapabilities=[\"fly\"]\n[[crew]]\nrole=\"b\"\ncapabilities=[\"fly\"]\n")),
        std::runtime_error);
    // Radar on two seats.
    CHECK_THROWS_AS(fl::parseEntityDef(crewEntity("[[crew]]\nrole=\"a\"\ncapabilities=[\"fly\",\"radar\"]\n"
                                                  "[[crew]]\nrole=\"b\"\ncapabilities=[\"radar\"]\n")),
                    std::runtime_error);
    // A hardpoint fired by two seats.
    CHECK_THROWS_AS(
        fl::parseEntityDef(crewEntity("[[hardpoints]]\nslot=0\nallowed=[\"m61\"]\ndefault=\"m61\"\n"
                                      "[[crew]]\nrole=\"a\"\ncapabilities=[\"fly\",\"fire\"]\nstations=[0]\n"
                                      "[[crew]]\nrole=\"b\"\ncapabilities=[\"fire\"]\nstations=[0]\n")),
        std::runtime_error);
    // A seat binding stations without the Fire capability.
    CHECK_THROWS_AS(fl::parseEntityDef(crewEntity("[[hardpoints]]\nslot=0\nallowed=[\"m61\"]\ndefault=\"m61\"\n"
                                                  "[[crew]]\nrole=\"a\"\ncapabilities=[\"fly\"]\nstations=[0]\n")),
                    std::runtime_error);
    // A seat referencing an unknown turret.
    CHECK_THROWS_AS(
        fl::parseEntityDef(crewEntity("[[crew]]\nrole=\"a\"\ncapabilities=[\"fly\",\"fire\"]\nturret=\"nope\"\n")),
        std::runtime_error);
    // A turret mounting an unknown hardpoint slot.
    CHECK_THROWS_AS(fl::parseEntityDef(crewEntity("[[turrets]]\nid=\"t\"\nstations=[9]\n"
                                                  "[[crew]]\nrole=\"a\"\ncapabilities=[\"fly\"]\n")),
                    std::runtime_error);
    // A Fire seat that fires nothing.
    CHECK_THROWS_AS(fl::parseEntityDef(crewEntity("[[crew]]\nrole=\"a\"\ncapabilities=[\"fly\",\"fire\"]\n")),
                    std::runtime_error);
    // A single valid Fly seat is fine.
    CHECK_NOTHROW(fl::parseEntityDef(crewEntity("[[crew]]\nrole=\"p\"\ncapabilities=[\"fly\"]\n")));
}

TEST_CASE("EntityDefParser: malformed crew/turret tables throw", "[parser][crew]") {
    // capabilities must be a non-empty array.
    CHECK_THROWS_AS(fl::parseEntityDef(crewEntity("[[crew]]\nrole=\"p\"\ncapabilities=[]\n")), std::runtime_error);
    // an unknown capability token.
    CHECK_THROWS_AS(fl::parseEntityDef(crewEntity("[[crew]]\nrole=\"p\"\ncapabilities=[\"navigate\"]\n")),
                    std::runtime_error);
    // seat_pos must have 3 entries.
    CHECK_THROWS_AS(
        fl::parseEntityDef(crewEntity("[[crew]]\nrole=\"p\"\ncapabilities=[\"fly\"]\nseat_pos=[0.0,1.0]\n")),
        std::runtime_error);
    // declaring both bot and empty.
    CHECK_THROWS_AS(
        fl::parseEntityDef(crewEntity("[[crew]]\nrole=\"p\"\ncapabilities=[\"fly\"]\nbot=\"gunner\"\nempty=true\n")),
        std::runtime_error);
    // a turret with slew_rate <= 0.
    CHECK_THROWS_AS(fl::parseEntityDef(crewEntity("[[turrets]]\nid=\"t\"\nslew_rate_deg_s=0.0\n"
                                                  "[[crew]]\nrole=\"p\"\ncapabilities=[\"fly\"]\n")),
                    std::runtime_error);
}

// ---------------------------------------------------------------------------
// applyPointDamage — the combat damage funnel (#626)
// ---------------------------------------------------------------------------

TEST_CASE("applyPointDamage: the friendly-fire gate suppresses same-faction damage", "[damage-rules]") {
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeAirVehicleDef("ff:a"));
    fl::EntityManager mgr(logger, registry);

    fl::EntityTransform t{};
    auto shooter = mgr.spawn("ff:a", t);
    auto victim = mgr.spawn("ff:a", t);
    mgr.get(shooter)->factionIndex = 2;
    mgr.get(victim)->factionIndex = 2;

    fl::DamageRules noFF{};
    noFF.friendlyFire = false;

    SECTION("teammate damage is suppressed and reported as such") {
        CHECK_FALSE(fl::applyPointDamage(mgr, victim, 40.f, shooter, noFF));
        CHECK(mgr.get(victim)->hp == 100.f);
    }

    SECTION("with friendly fire enabled the same shot lands") {
        fl::DamageRules ff{};
        ff.friendlyFire = true;
        CHECK(fl::applyPointDamage(mgr, victim, 40.f, shooter, ff));
        CHECK(mgr.get(victim)->hp == 60.f);
    }

    SECTION("neutral faction 0 is not a team") {
        mgr.get(shooter)->factionIndex = 0;
        mgr.get(victim)->factionIndex = 0;
        CHECK(fl::applyPointDamage(mgr, victim, 40.f, shooter, noFF));
        CHECK(mgr.get(victim)->hp == 60.f);
    }

    SECTION("self-damage always applies (your own blast radius is not friendly fire)") {
        CHECK(fl::applyPointDamage(mgr, victim, 40.f, victim, noFF));
        CHECK(mgr.get(victim)->hp == 60.f);
    }

    SECTION("environmental damage (null instigator) always applies") {
        CHECK(fl::applyPointDamage(mgr, victim, 40.f, fl::EntityId::null(), noFF));
        CHECK(mgr.get(victim)->hp == 60.f);
    }
}

TEST_CASE("applyPointDamage: instigator attribution flows through to the kill", "[damage-rules]") {
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    registry.registerType(makeAirVehicleDef("ff:b"));
    fl::EntityManager mgr(logger, registry);

    struct Collector : fl::IEntityEventHandler {
        std::vector<fl::EntityEvent> events;
        void onEntityEvent(const fl::EntityEvent& e) override {
            events.push_back(e);
        }
    } collector;
    mgr.addEventHandler(&collector);

    fl::EntityTransform t{};
    auto shooter = mgr.spawn("ff:b", t);
    auto victim = mgr.spawn("ff:b", t);
    mgr.get(shooter)->factionIndex = 1;
    mgr.get(victim)->factionIndex = 2;

    CHECK(fl::applyPointDamage(mgr, victim, 200.f, shooter, fl::DamageRules{}));

    REQUIRE(collector.events.size() == 2); // Died + ScoreAwarded
    CHECK(collector.events[0].type == fl::EntityEventType::Died);
    CHECK(collector.events[0].instigator == shooter);
    CHECK(collector.events[1].type == fl::EntityEventType::ScoreAwarded);
    CHECK(collector.events[1].instigator == shooter);
}

// ---------------------------------------------------------------------------
// applyWarhead — area-of-effect detonation (#356)
// ---------------------------------------------------------------------------

namespace {

struct BlastWorld {
    MockLogger logger;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em;
    fl::SpatialIndex si;

    BlastWorld() : em(logger, registry) {
        registry.registerType(makeAirVehicleDef("blast:a"));
    }

    fl::EntityId spawnAt(double x, double y, double z) {
        fl::EntityTransform t{};
        t.pos[0] = x;
        t.pos[1] = y;
        t.pos[2] = z;
        auto id = em.spawn("blast:a", t);
        si.insert(id.index, t.pos);
        return id;
    }
};

} // namespace

TEST_CASE("applyWarhead: linear falloff - full at the centre, zero at the edge, nothing outside", "[warhead]") {
    BlastWorld w;
    const auto centre = w.spawnAt(0, 0, 0);
    const auto mid = w.spawnAt(50, 0, 0);      // half the radius: half the damage
    const auto atEdge = w.spawnAt(100, 0, 0);  // exactly the radius: zero
    const auto outside = w.spawnAt(150, 0, 0); // untouched

    fl::BlastSpec blast{100.f, 80.f, false};
    const double pos[3] = {0, 0, 0};
    const auto r = fl::applyWarhead(w.em, w.si, pos, blast, fl::EntityId::null(), fl::DamageRules{});

    CHECK(w.em.get(centre)->hp == Catch::Approx(20.f)); // 100 − 80
    CHECK(w.em.get(mid)->hp == Catch::Approx(60.f));    // 100 − 40
    CHECK(w.em.get(atEdge)->hp == Catch::Approx(100.f));
    CHECK(w.em.get(outside)->hp == Catch::Approx(100.f));
    CHECK(r.damaged == 2);
}

TEST_CASE("applyWarhead: the friendly-fire gate holds inside a blast", "[warhead]") {
    BlastWorld w;
    const auto shooter = w.spawnAt(500, 0, 0); // outside its own blast
    const auto friendly = w.spawnAt(10, 0, 0);
    const auto hostile = w.spawnAt(20, 0, 0);
    w.em.get(shooter)->factionIndex = 1;
    w.em.get(friendly)->factionIndex = 1;
    w.em.get(hostile)->factionIndex = 2;

    fl::BlastSpec blast{100.f, 50.f, false};
    const double pos[3] = {0, 0, 0};
    fl::applyWarhead(w.em, w.si, pos, blast, shooter, fl::DamageRules{}); // FF off by default

    CHECK(w.em.get(friendly)->hp == 100.f);
    CHECK(w.em.get(hostile)->hp < 100.f);
}

TEST_CASE("applyWarhead: a nuclear blast EMPs bystanders far beyond the shrapnel", "[warhead]") {
    BlastWorld w;
    const auto inBlast = w.spawnAt(50, 0, 0);
    const auto bystander = w.spawnAt(300, 0, 0); // outside 100 m blast, inside the 400 m EMP ring
    const auto farAway = w.spawnAt(500, 0, 0);

    std::vector<uint32_t> emped;
    fl::BlastSpec nuke{100.f, 80.f, true};
    const double pos[3] = {0, 0, 0};
    const auto r = fl::applyWarhead(w.em, w.si, pos, nuke, fl::EntityId::null(), fl::DamageRules{},
                                    [&](fl::EntityId id) { emped.push_back(id.index); });

    CHECK(w.em.get(inBlast)->hp < 100.f);
    CHECK(w.em.get(bystander)->hp == 100.f); // no shrapnel out there...
    CHECK(r.emped == 2);                     // ...but the electronics are gone
    CHECK(std::find(emped.begin(), emped.end(), bystander.index) != emped.end());
    CHECK(std::find(emped.begin(), emped.end(), farAway.index) == emped.end());
}

TEST_CASE("applyWarhead: a degenerate blast is a no-op", "[warhead]") {
    BlastWorld w;
    const auto e = w.spawnAt(0, 0, 0);
    const double pos[3] = {0, 0, 0};
    CHECK(fl::applyWarhead(w.em, w.si, pos, fl::BlastSpec{0.f, 100.f, false}, fl::EntityId::null(), fl::DamageRules{})
              .damaged == 0);
    CHECK(w.em.get(e)->hp == 100.f);
}

TEST_CASE("EntityDefParser: [deck] parses a flight deck and requires accepts_landings (#38)", "[parser]") {
    const std::string base = "[entity]\n"
                             "id = \"test:cv\"\n"
                             "name = \"Carrier\"\n"
                             "category = \"naval_vehicle\"\n"
                             "max_hp = 8000.0\n";
    const std::string deck = "[deck]\n"
                             "length_m = 300.0\n"
                             "width_m = 70.0\n"
                             "height_m = 18.0\n"
                             "cat_end_speed_mps = 72.0\n"
                             "wire_x_m = -100.0\n";

    fl::EntityDef def = fl::parseEntityDef(base + "accepts_landings = true\n" + deck);
    REQUIRE(def.deck.has_value());
    CHECK(def.acceptsLandings);
    CHECK_THAT(def.deck->lengthM, WithinAbs(300.f, 1e-4f));
    CHECK_THAT(def.deck->heightM, WithinAbs(18.f, 1e-4f));
    CHECK_THAT(def.deck->catEndSpeedMps, WithinAbs(72.f, 1e-4f));
    CHECK_THAT(def.deck->wireXM, WithinAbs(-100.f, 1e-4f));
    CHECK_THAT(def.deck->catStrokeM, WithinAbs(100.f, 1e-4f)); // engine default kept

    // A deck nothing may land on is a modelling error, not a warning.
    CHECK_THROWS(fl::parseEntityDef(base + deck));
}
