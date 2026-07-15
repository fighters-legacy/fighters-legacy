// SPDX-License-Identifier: GPL-3.0-or-later
//
// engine-mission runtime tests (#632): the shared schema parser (parseMission), parser/linter parity
// with validate-mission, and the faction-aware sim-setup applier (applyMission).

#include "mission/Mission.h"
#include "mission/MissionParser.h"
#include "mission/MissionSetup.h"

#include "ILogger.h"
#include "entity/EntityDef.h"
#include "entity/EntityManager.h"
#include "entity/EntityState.h"
#include "entity/EntityTypeRegistry.h"
#include "weather/WeatherController.h"
#include "world/FactionRegistry.h"

#include "mission_validator.h" // validate-mission's façade — for the parity check

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace fl;

namespace {

const char* kValidMission = R"yaml(
name: "Storm Warning"
map: ukraine
layer: ukraine_clear
time: { hour: 14, minute: 30 }
wind: { heading: 270, speed: 12 }
weather:
  preset: rain
time_scale: 20.0
sides: [nato, russia]
objects:
  - type: F22
    id: player1
    side: nato
    pos: [12400, 0, 8800]
    heading: 90
    alt: 500
    player: true
  - type: SA10
    id: sam1
    side: russia
    pos: [15000, 0, 9000]
    heading: 0
triggers:
  - on: destroy(sam1)
    do: mission_success
  - on: timer(600)
    do: mission_failure
)yaml";

struct NullLogger final : ILogger {
    void log(LogLevel, const char*, int, const char*) override {}
    void setMinLevel(LogLevel) override {}
    void flush() override {}
};

EntityDef makeDef(const char* id) {
    EntityDef d{};
    d.id = id;
    d.maxHp = 100.f;
    return d;
}

} // namespace

// ---------------------------------------------------------------------------
// parseMission — model population
// ---------------------------------------------------------------------------

TEST_CASE("parseMission populates the runtime model", "[mission-parser]") {
    auto r = parseMission(kValidMission);
    REQUIRE(r.ok);
    const Mission& m = r.mission;

    CHECK(m.name == "Storm Warning");
    CHECK(m.map == "ukraine");
    CHECK(m.layer == "ukraine_clear");
    CHECK(m.time.hour == 14);
    CHECK(m.time.minute == 30);
    CHECK(m.wind.headingDeg == 270.f);
    CHECK(m.wind.speedMs == 12.f);
    REQUIRE(m.weatherPreset.has_value());
    CHECK(*m.weatherPreset == WeatherPreset::Rain);
    REQUIRE(m.timeScale.has_value());
    CHECK(*m.timeScale == 20.f);

    REQUIRE(m.sides.size() == 2);
    CHECK(m.sides[0].id == "nato");
    CHECK(m.sides[1].id == "russia");

    REQUIRE(m.objects.size() == 2);
    CHECK(m.objects[0].id == "player1");
    CHECK(m.objects[0].playerSlot); // player: true
    REQUIRE(m.objects[0].alt.has_value());
    CHECK(*m.objects[0].alt == 500.f);
    CHECK(m.objects[1].id == "sam1");
    CHECK_FALSE(m.objects[1].playerSlot);
    CHECK(m.objects[1].pos[0] == 15000.0);

    REQUIRE(m.triggers.size() == 2);
    CHECK(m.triggers[0].on == "destroy(sam1)");
    CHECK(m.triggers[0].doAction == "mission_success");
}

TEST_CASE("parseMission accepts the coalition (map) side form with allies", "[mission-parser]") {
    const char* yaml = R"yaml(
name: x
map: y
layer: z
time: { hour: 0, minute: 0 }
wind: { heading: 0, speed: 0 }
sides:
  - id: nato
    allies: [ukraine]
  - id: ukraine
  - id: russia
objects:
  - { type: F16, id: a, side: nato, pos: [0, 0, 0], heading: 0 }
triggers: []
)yaml";
    auto r = parseMission(yaml);
    REQUIRE(r.ok);
    REQUIRE(r.mission.sides.size() == 3);
    CHECK(r.mission.sides[0].id == "nato");
    REQUIRE(r.mission.sides[0].allies.size() == 1);
    CHECK(r.mission.sides[0].allies[0] == "ukraine");
}

TEST_CASE("parseMission rejects an ally that names an unknown side", "[mission-parser]") {
    const char* yaml = R"yaml(
name: x
map: y
layer: z
time: { hour: 0, minute: 0 }
wind: { heading: 0, speed: 0 }
sides:
  - id: nato
    allies: [atlantis]
objects:
  - { type: F16, id: a, side: nato, pos: [0, 0, 0], heading: 0 }
triggers: []
)yaml";
    auto r = parseMission(yaml);
    CHECK_FALSE(r.ok);
    bool found = false;
    for (const auto& e : r.errors)
        if (e.find("atlantis") != std::string::npos)
            found = true;
    CHECK(found);
}

// ---------------------------------------------------------------------------
// Parser / linter parity — the anti-drift contract (#632)
// ---------------------------------------------------------------------------

TEST_CASE("validate-mission delegates to parseMission (identical diagnostics)", "[mission-parser][parity]") {
    for (const char* doc : {kValidMission, "name: x\n", "{{{ not yaml", "sides: [a]\nobjects: []\ntriggers: []\n"}) {
        auto p = parseMission(doc);
        auto v = validateMission(doc);
        CHECK(p.ok == v.ok);
        CHECK(p.errors == v.errors);
        CHECK(p.warnings == v.warnings);
    }
}

// ---------------------------------------------------------------------------
// applyMission — faction-aware sim setup
// ---------------------------------------------------------------------------

TEST_CASE("applyMission builds the coalition registry with reserved neutral index 0", "[mission-setup]") {
    const char* yaml = R"yaml(
name: x
map: y
layer: z
time: { hour: 0, minute: 0 }
wind: { heading: 0, speed: 0 }
sides:
  - id: nato
    allies: [ukraine]
  - id: ukraine
  - id: russia
objects:
  - { type: test:fighter, id: a, side: nato, pos: [0, 0, 0], heading: 0 }
triggers: []
)yaml";
    auto parsed = parseMission(yaml);
    REQUIRE(parsed.ok);

    NullLogger log;
    EntityTypeRegistry reg;
    reg.registerType(makeDef("test:fighter"));
    EntityManager em(log, reg);
    FactionRegistry factions;

    auto result = applyMission(parsed.mission, em, factions);

    // Index 0 is reserved neutral; real sides occupy 1..N (never index 0, which would be unhostile).
    CHECK(factions.count() == 4u); // neutral + 3 sides
    const uint16_t nato = factions.indexOf("nato");
    const uint16_t ukraine = factions.indexOf("ukraine");
    const uint16_t russia = factions.indexOf("russia");
    CHECK(nato >= 1u);
    CHECK(factions.indexOf("") == 0u); // the neutral sentinel

    // Distinct non-allied sides are hostile; declared allies are friendly.
    CHECK(factions.areHostile(nato, russia));
    CHECK(factions.areHostile(ukraine, russia));
    CHECK_FALSE(factions.areHostile(nato, ukraine)); // allied
}

TEST_CASE("applyMission spawns world objects with faction, separates player slots", "[mission-setup]") {
    auto parsed = parseMission(kValidMission);
    REQUIRE(parsed.ok);

    NullLogger log;
    EntityTypeRegistry reg;
    reg.registerType(makeDef("SA10")); // sam1's type is registered; F22 (the player slot) is not needed
    EntityManager em(log, reg);
    FactionRegistry factions;
    WeatherController weather;

    auto result = applyMission(parsed.mission, em, factions, &weather);

    // player1 is `player: true` -> a slot, not a spawned entity; sam1 is spawned.
    REQUIRE(result.playerSlots.size() == 1);
    CHECK(result.playerSlots[0].type == "F22");
    CHECK(result.playerSlots[0].factionIndex == factions.indexOf("nato"));
    CHECK(result.playerSlots[0].pos[1] == 500.0); // alt override applied

    REQUIRE(result.spawned.size() == 1);
    const EntityState* sam = em.get(result.spawned[0]);
    REQUIRE(sam != nullptr);
    CHECK(sam->factionIndex == factions.indexOf("russia"));
    CHECK(sam->transform.pos[0] == 15000.0);

    // Weather / time / wind applied to the controller.
    CHECK(weather.preset() == WeatherPreset::Rain);
    CHECK(weather.timeOfDay() == 14.5f); // 14:30
}

TEST_CASE("applyMission warns (does not crash) on an unregistered object type", "[mission-setup]") {
    const char* yaml = R"yaml(
name: x
map: y
layer: z
time: { hour: 0, minute: 0 }
wind: { heading: 0, speed: 0 }
sides: [nato]
objects:
  - { type: does_not_exist, id: ghost, side: nato, pos: [0, 0, 0], heading: 0 }
triggers: []
)yaml";
    auto parsed = parseMission(yaml);
    REQUIRE(parsed.ok);

    NullLogger log;
    EntityTypeRegistry reg; // nothing registered
    EntityManager em(log, reg);
    FactionRegistry factions;

    auto result = applyMission(parsed.mission, em, factions);
    CHECK(result.spawned.empty());
    REQUIRE(result.warnings.size() == 1);
    CHECK(result.warnings[0].find("ghost") != std::string::npos);
}
