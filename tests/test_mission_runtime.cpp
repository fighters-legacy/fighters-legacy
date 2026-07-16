// SPDX-License-Identifier: GPL-3.0-or-later
//
// engine-mission runtime tests (#632): the shared schema parser (parseMission), parser/linter parity
// with validate-mission, and the faction-aware sim-setup applier (applyMission).

#include "mission/BuiltinMissions.h"
#include "mission/Mission.h"
#include "mission/MissionParser.h"
#include "mission/MissionRuntime.h"
#include "mission/MissionSetup.h"

#include "ILogger.h"
#include "entity/EntityDef.h"
#include "entity/EntityManager.h"
#include "entity/EntityState.h"
#include "entity/EntityTypeRegistry.h"
#include "weather/WeatherController.h"
#include "world/FactionRegistry.h"

#include "mission_validator.h" // validate-mission's façade — for the parity check

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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

// ---------------------------------------------------------------------------
// Scripted-bot schema (#855): ai / route / loadout
// ---------------------------------------------------------------------------

TEST_CASE("parseMission parses ai, route, and loadout into the model", "[mission-parser]") {
    const char* yaml = R"yaml(
name: x
map: y
layer: z
time: { hour: 0, minute: 0 }
wind: { heading: 0, speed: 0 }
sides: [red]
objects:
  - type: SA10
    id: bandit
    side: red
    pos: [0, 0, 0]
    heading: 0
    ai: "pursuit 3"
    loadout: [aim9, aim9, "~"]
    route:
      - [100, 200, 300]
      - [400, 500, 600]
triggers: []
)yaml";
    auto r = parseMission(yaml);
    REQUIRE(r.ok);
    REQUIRE(r.mission.objects.size() == 1);
    const MissionObject& o = r.mission.objects[0];
    CHECK(o.ai == "pursuit 3");
    REQUIRE(o.loadout.size() == 3);
    CHECK(o.loadout[0] == "aim9");
    CHECK(o.loadout[2] == "~");
    REQUIRE(o.route.size() == 2);
    CHECK(o.route[0][0] == 100.0);
    CHECK(o.route[1][2] == 600.0);
}

TEST_CASE("parseMission parses an optional airborne speed and rejects a negative one (#883)", "[mission-parser]") {
    const char* base = R"yaml(
name: x
map: y
layer: z
time: { hour: 0, minute: 0 }
wind: { heading: 0, speed: 0 }
sides: [red]
objects:
  - { type: SA10, id: bandit, side: red, pos: [0, 3000, 0], heading: 0, speed: %SPEED% }
triggers: []
)yaml";

    {
        std::string yaml = base;
        yaml.replace(yaml.find("%SPEED%"), 7, "180");
        auto r = parseMission(yaml);
        REQUIRE(r.ok);
        REQUIRE(r.mission.objects.size() == 1);
        REQUIRE(r.mission.objects[0].speed.has_value());
        CHECK(*r.mission.objects[0].speed == Catch::Approx(180.f));
    }
    {
        std::string yaml = base;
        yaml.replace(yaml.find("%SPEED%"), 7, "-5");
        auto r = parseMission(yaml);
        CHECK_FALSE(r.ok);
        const bool flagged = std::any_of(r.errors.begin(), r.errors.end(), [](const std::string& e) {
            return e.find("speed must be >= 0") != std::string::npos;
        });
        CHECK(flagged);
    }
    // Absent speed leaves the optional empty (the engine picks a cruise default at spawn).
    {
        std::string yaml = base;
        yaml.replace(yaml.find(", speed: %SPEED%"), 16, "");
        auto r = parseMission(yaml);
        REQUIRE(r.ok);
        CHECK_FALSE(r.mission.objects[0].speed.has_value());
    }
}

TEST_CASE("parseMission parses start: ground|air and rejects other values (#885)", "[mission-parser]") {
    const char* base = R"yaml(
name: x
map: y
layer: z
time: { hour: 0, minute: 0 }
wind: { heading: 0, speed: 0 }
sides: [red]
objects:
  - { type: SA10, id: a, side: red, pos: [0, 0, 0], heading: 0, start: %S% }
triggers: []
)yaml";
    auto withStart = [&](const char* v) {
        std::string y = base;
        y.replace(y.find("%S%"), 3, v);
        return parseMission(y);
    };

    auto g = withStart("ground");
    REQUIRE(g.ok);
    CHECK(g.mission.objects[0].groundStart);

    auto a = withStart("air");
    REQUIRE(a.ok);
    CHECK_FALSE(a.mission.objects[0].groundStart);

    auto bad = withStart("sideways");
    CHECK_FALSE(bad.ok);

    // Absent start defaults to air.
    std::string plain = base;
    plain.replace(plain.find(", start: %S%"), 12, "");
    auto p = parseMission(plain);
    REQUIRE(p.ok);
    CHECK_FALSE(p.mission.objects[0].groundStart);
}

TEST_CASE("applyMission places a ground-start object on the terrain and parks the slot (#885)", "[mission-setup]") {
    const char* yaml = R"yaml(
name: x
map: y
layer: z
time: { hour: 0, minute: 0 }
wind: { heading: 0, speed: 0 }
sides: [blue]
objects:
  - { type: test:fighter, id: parked, side: blue, pos: [100, 4000, 200], heading: 0, start: ground }
  - { type: test:fighter, id: p1, side: blue, pos: [0, 4000, 0], heading: 0, start: ground, player: true }
triggers: []
)yaml";
    auto parsed = parseMission(yaml);
    REQUIRE(parsed.ok);

    NullLogger log;
    EntityTypeRegistry reg;
    reg.registerType(makeDef("test:fighter"));
    EntityManager em(log, reg);
    FactionRegistry factions;

    // Terrain sits at 550 m everywhere for this test.
    auto result =
        applyMission(parsed.mission, em, factions, nullptr, kEarthRadiusM, {}, [](double, double) { return 550.0; });

    // The AI object is spawned sitting on the ground (550), not at its authored 4000 m alt.
    REQUIRE(result.spawned.size() == 1);
    const EntityState* s = em.get(result.spawned[0]);
    REQUIRE(s != nullptr);
    CHECK(s->transform.pos[1] == Catch::Approx(550.0));

    // The player slot is likewise on the ground AND parked (0 airspeed).
    REQUIRE(result.playerSlots.size() == 1);
    CHECK(result.playerSlots[0].pos[1] == Catch::Approx(550.0));
    REQUIRE(result.playerSlots[0].speed.has_value());
    CHECK(*result.playerSlots[0].speed == Catch::Approx(0.f));
}

TEST_CASE("parseMission rejects a malformed route waypoint", "[mission-parser]") {
    const char* yaml = R"yaml(
name: x
map: y
layer: z
time: { hour: 0, minute: 0 }
wind: { heading: 0, speed: 0 }
sides: [red]
objects:
  - { type: SA10, id: b, side: red, pos: [0, 0, 0], heading: 0, route: [[1, 2]] }
triggers: []
)yaml";
    auto r = parseMission(yaml);
    CHECK_FALSE(r.ok);
    bool found = false;
    for (const auto& e : r.errors)
        if (e.find("route") != std::string::npos)
            found = true;
    CHECK(found);
}

TEST_CASE("parseMission warns when a player slot also carries ai/route/loadout", "[mission-parser]") {
    const char* yaml = R"yaml(
name: x
map: y
layer: z
time: { hour: 0, minute: 0 }
wind: { heading: 0, speed: 0 }
sides: [blue]
objects:
  - { type: F16, id: p, side: blue, pos: [0, 0, 0], heading: 0, player: true, ai: "loiter" }
triggers: []
)yaml";
    auto r = parseMission(yaml);
    CHECK(r.ok); // a warning, not an error
    bool found = false;
    for (const auto& w : r.warnings)
        if (w.find("player slot") != std::string::npos)
            found = true;
    CHECK(found);
}

TEST_CASE("applyMission calls the onSpawned hook once per spawned world object", "[mission-setup]") {
    const char* yaml = R"yaml(
name: x
map: y
layer: z
time: { hour: 0, minute: 0 }
wind: { heading: 0, speed: 0 }
sides: [red, blue]
objects:
  - { type: test:fighter, id: a, side: red, pos: [0, 0, 0], heading: 0, ai: "loiter" }
  - { type: test:fighter, id: slot, side: blue, pos: [10, 0, 0], heading: 0, player: true }
triggers: []
)yaml";
    auto parsed = parseMission(yaml);
    REQUIRE(parsed.ok);

    NullLogger log;
    EntityTypeRegistry reg;
    reg.registerType(makeDef("test:fighter"));
    EntityManager em(log, reg);
    FactionRegistry factions;

    std::vector<std::string> hookAi;
    auto hook = [&](EntityId, const MissionObject& obj) { hookAi.push_back(obj.ai); };
    auto result = applyMission(parsed.mission, em, factions, nullptr, fl::kEarthRadiusM, hook);

    // Only the non-player object is spawned -> the hook fires once, with its ai spec. The player slot
    // is not spawned (it becomes a joinable slot), so the hook never sees it.
    REQUIRE(result.spawned.size() == 1);
    REQUIRE(result.playerSlots.size() == 1);
    REQUIRE(hookAi.size() == 1);
    CHECK(hookAi[0] == "loiter");
}

// ---------------------------------------------------------------------------
// MissionRuntime — objective / trigger evaluator (#633)
// ---------------------------------------------------------------------------

namespace {

// A mission with the given triggers and no objects/sides (the evaluator only reads triggers +
// the object->entity map passed separately).
Mission missionWith(std::vector<MissionTrigger> triggers) {
    Mission m;
    m.name = "T";
    m.triggers = std::move(triggers);
    return m;
}

} // namespace

TEST_CASE("MissionRuntime: mission_start fires immediately and completes the objective", "[mission-runtime]") {
    NullLogger log;
    EntityTypeRegistry reg;
    EntityManager em(log, reg);
    Mission m = missionWith({{"mission_start", "mission_success"}});

    bool ended = false;
    MissionRuntime rt(m, {}, em);
    rt.setOnEnd([&](const MissionOutcome&) { ended = true; });
    rt.step(0);

    CHECK(rt.done());
    CHECK(rt.outcome().state == MissionState::Complete);
    CHECK(rt.outcome().triggersFired == 1u);
    CHECK(ended);
}

TEST_CASE("MissionRuntime: timer(n) fires after n seconds of elapsed sim time", "[mission-runtime]") {
    NullLogger log;
    EntityTypeRegistry reg;
    EntityManager em(log, reg);
    Mission m = missionWith({{"timer(2)", "mission_failure"}});

    MissionRuntime rt(m, {}, em);
    rt.setEvalIntervalTicks(1); // evaluate every tick so the boundary is exact
    rt.setSimDt(1.0 / 60.0);

    for (uint64_t t = 0; t < 120; ++t) {
        rt.step(t);
        CHECK(rt.outcome().state == MissionState::Active); // < 2.0 s
    }
    rt.step(120); // elapsed = 120/60 = 2.0 s
    CHECK(rt.outcome().state == MissionState::Failed);
}

TEST_CASE("MissionRuntime: destroy(<id>) fires when the object's entity dies", "[mission-runtime]") {
    NullLogger log;
    EntityTypeRegistry reg;
    reg.registerType(makeDef("test:fighter"));
    EntityManager em(log, reg);

    EntityTransform t{};
    const EntityId bandit = em.spawn("test:fighter", t);
    REQUIRE(bandit.valid());

    Mission m = missionWith({{"destroy(bandit)", "mission_success"}});
    MissionRuntime rt(m, {{"bandit", bandit}}, em);
    rt.setEvalIntervalTicks(1);

    rt.step(0);
    CHECK(rt.outcome().state == MissionState::Active); // still alive

    em.kill(bandit);
    em.onTick(1.0 / 60.0, 1); // reap the killed entity
    rt.step(1);
    CHECK(rt.outcome().state == MissionState::Complete);
}

TEST_CASE("MissionRuntime: destroy(<player-slot>) tracks the bound pilot, not t=0 (#884)", "[mission-runtime]") {
    NullLogger log;
    EntityTypeRegistry reg;
    reg.registerType(makeDef("test:fighter"));
    EntityManager em(log, reg);

    // A player slot is seeded with an INVALID entity (unoccupied). Before the fix, destroy(player1)
    // read "never spawned -> destroyed" and failed the mission at 0.0 s, before anyone connected.
    Mission m = missionWith({{"destroy(player1)", "mission_failure"}});
    MissionRuntime rt(m, {{"player1", EntityId{}}}, em);
    rt.setEvalIntervalTicks(1);

    rt.step(0);
    CHECK(rt.outcome().state == MissionState::Active); // unoccupied slot != destroyed

    // A pilot claims the slot: bind its aircraft; destroy() stays false while it lives.
    EntityTransform t{};
    const EntityId pilot = em.spawn("test:fighter", t);
    REQUIRE(pilot.valid());
    rt.registerObjectEntity("player1", pilot);
    rt.step(60);
    CHECK(rt.outcome().state == MissionState::Active);

    // The pilot disconnects: the slot is unbound and reads as unoccupied again (not destroyed).
    rt.registerObjectEntity("player1", EntityId{});
    rt.step(120);
    CHECK(rt.outcome().state == MissionState::Active);

    // A pilot re-occupies the slot and is then destroyed -> the failure fires.
    rt.registerObjectEntity("player1", pilot);
    em.kill(pilot);
    em.onTick(1.0 / 60.0, 181);
    rt.step(180);
    CHECK(rt.outcome().state == MissionState::Failed);
}

TEST_CASE("MissionRuntime: triggers fire in declaration order; non-terminal actions dispatch", "[mission-runtime]") {
    NullLogger log;
    EntityTypeRegistry reg;
    EntityManager em(log, reg);
    Mission m = missionWith({{"mission_start", "spawn(Su27,red,0,0,0)"}, {"mission_start", "mission_success"}});

    std::vector<std::string> dispatched;
    MissionRuntime rt(m, {}, em, [&](std::string_view a) { dispatched.emplace_back(a); });
    rt.step(0);

    REQUIRE(dispatched.size() == 1); // the spawn action routed through the dispatcher
    CHECK(dispatched[0] == "spawn(Su27,red,0,0,0)");
    CHECK(rt.outcome().state == MissionState::Complete); // the second trigger ended the mission
    CHECK(rt.outcome().triggersFired == 2u);
}

TEST_CASE("MissionRuntime: a trigger fires exactly once (edge), not every tick", "[mission-runtime]") {
    NullLogger log;
    EntityTypeRegistry reg;
    EntityManager em(log, reg);
    Mission m = missionWith({{"mission_start", "spawn(x)"}}); // non-terminal: the mission stays Active

    std::vector<std::string> dispatched;
    MissionRuntime rt(m, {}, em, [&](std::string_view a) { dispatched.emplace_back(a); });
    rt.setEvalIntervalTicks(1);
    for (uint64_t t = 0; t < 10; ++t)
        rt.step(t);

    CHECK(dispatched.size() == 1u); // fired on the first evaluation and never again
    CHECK(rt.outcome().state == MissionState::Active);
}

// --- builtin sandbox mission (#868) --------------------------------------------------------------

TEST_CASE("the builtin sandbox mission parses cleanly and references only builtin content (#868)", "[mission-parser]") {
    const std::string_view yaml = fl::builtinMissionYaml("builtin:sandbox");
    REQUIRE_FALSE(yaml.empty());
    CHECK(fl::builtinMissionYaml("builtin:nope").empty()); // unknown id -> empty

    auto r = parseMission(std::string(yaml));
    REQUIRE(r.ok);
    const Mission& m = r.mission;
    CHECK(m.sides.size() == 2u); // blue vs red

    bool hasPlayerSlot = false, hasSamSite = false, hasLuaBot = false, hasSamAi = false;
    for (const MissionObject& o : m.objects) {
        // Every object flies builtin content — the whole point is zero-pack.
        CHECK(o.type.rfind("builtin:", 0) == 0u);
        if (o.playerSlot)
            hasPlayerSlot = true;
        if (o.type == "builtin:sam-site")
            hasSamSite = true;
        if (o.ai == "lua builtin:fighter")
            hasLuaBot = true;
        if (o.ai == "sam")
            hasSamAi = true;
    }
    CHECK(hasPlayerSlot);           // a joinable slot for Instant Action / a connecting pilot
    CHECK(hasSamSite);              // something that shoots back
    CHECK(hasLuaBot);               // a builtin:fighter scripted bot
    CHECK(hasSamAi);                // the SAM auto-engages
    CHECK(m.triggers.size() >= 2u); // an objective + a backstop
}

TEST_CASE("the builtin shape-gallery mission parses cleanly and covers every placeholder category",
          "[mission-parser]") {
    const std::string_view yaml = fl::builtinMissionYaml("builtin:shape-gallery");
    REQUIRE_FALSE(yaml.empty());

    auto r = parseMission(std::string(yaml));
    REQUIRE(r.ok);
    const Mission& m = r.mission;

    // The gallery is the visual-verification scene for the per-category builtin placeholder meshes
    // (#886): every silhouette source must be present — the surface categories parked on the
    // terrain, the ordnance exhibits (plain projectile-type objects, which spawn with no controller
    // and hold position), an armed player slot, and live shooters for in-flight missiles.
    auto hasObjectOfType = [&](std::string_view type) {
        for (const MissionObject& o : m.objects)
            if (o.type == type)
                return true;
        return false;
    };
    CHECK(hasObjectOfType("builtin:debug-entity"));          // AirVehicle
    CHECK(hasObjectOfType("builtin:ground-vehicle"));        // GroundVehicle
    CHECK(hasObjectOfType("builtin:naval-vessel"));          // NavalVehicle
    CHECK(hasObjectOfType("builtin:static-target"));         // Structure
    CHECK(hasObjectOfType("builtin:sam-site"));              // SARH shooter -> in-flight Missile
    CHECK(hasObjectOfType("builtin:aaa"));                   // gun emplacement
    CHECK(hasObjectOfType("projectile:builtin:ir-missile")); // Missile exhibit
    CHECK(hasObjectOfType("projectile:builtin:bomb"));       // Bomb exhibit
    CHECK(hasObjectOfType("projectile:builtin:rocket"));     // Rocket exhibit

    bool hasPlayerSlot = false;
    for (const MissionObject& o : m.objects)
        hasPlayerSlot |= o.playerSlot;
    CHECK(hasPlayerSlot); // the --fly seat for firing bombs/rockets and making wrecks

    // Exhibit objects must be plain (no ai/route) so they spawn without a controller and float.
    for (const MissionObject& o : m.objects) {
        if (o.type.rfind("projectile:", 0) == 0u) {
            CHECK(o.ai.empty());
            CHECK(o.route.empty());
        }
    }
    CHECK(m.triggers.size() >= 2u);
}
