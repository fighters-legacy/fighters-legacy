// SPDX-License-Identifier: GPL-3.0-or-later
#include "mission_validator.h"

#include "mission/MissionParser.h" // parseMission — camera shots need the parsed Mission model (#910)

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

using namespace fl;

static const char* kValidMission = R"yaml(
name: "Storm Warning"
map: ukraine
layer: ukraine_clear
time: { hour: 14, minute: 0 }
wind: { heading: 270, speed: 12 }
sides: [nato, russia]
objects:
  - type: F22
    id: player1
    side: nato
    pos: [12400, 0, 8800]
    heading: 90
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

static std::string without(const char* src, const char* line) {
    std::string s(src);
    auto pos = s.find(line);
    if (pos != std::string::npos) {
        auto end = s.find('\n', pos);
        if (end != std::string::npos)
            s.erase(pos, end - pos + 1);
    }
    return s;
}

static std::string replace_first(const char* src, const char* from, const char* to) {
    std::string s(src);
    auto pos = s.find(from);
    if (pos != std::string::npos)
        s.replace(pos, std::string(from).size(), to);
    return s;
}

TEST_CASE("valid mission passes", "[mission-validator]") {
    auto r = validateMission(kValidMission);
    CHECK(r.ok);
    CHECK(r.errors.empty());
}

TEST_CASE("malformed YAML fails with parse error", "[mission-validator]") {
    auto r = validateMission("{{{{ not valid yaml");
    CHECK_FALSE(r.ok);
    REQUIRE(!r.errors.empty());
    CHECK(r.errors[0].find("parse error") != std::string::npos);
}

TEST_CASE("missing name fails", "[mission-validator]") {
    auto r = validateMission(without(kValidMission, "name: \"Storm Warning\""));
    CHECK_FALSE(r.ok);
    bool found = false;
    for (const auto& e : r.errors)
        if (e.find("name") != std::string::npos) {
            found = true;
            break;
        }
    CHECK(found);
}

TEST_CASE("missing map fails", "[mission-validator]") {
    auto r = validateMission(without(kValidMission, "map: ukraine"));
    CHECK_FALSE(r.ok);
}

TEST_CASE("missing layer fails", "[mission-validator]") {
    auto r = validateMission(without(kValidMission, "layer: ukraine_clear"));
    CHECK_FALSE(r.ok);
}

TEST_CASE("missing time block fails", "[mission-validator]") {
    auto r = validateMission(without(kValidMission, "time: { hour: 14, minute: 0 }"));
    CHECK_FALSE(r.ok);
}

TEST_CASE("time.hour out of range [0,23] fails", "[mission-validator]") {
    auto r = validateMission(replace_first(kValidMission, "hour: 14", "hour: 25"));
    CHECK_FALSE(r.ok);
    bool found = false;
    for (const auto& e : r.errors)
        if (e.find("hour") != std::string::npos) {
            found = true;
            break;
        }
    CHECK(found);
}

TEST_CASE("time.minute out of range [0,59] fails", "[mission-validator]") {
    auto r = validateMission(replace_first(kValidMission, "minute: 0", "minute: 60"));
    CHECK_FALSE(r.ok);
}

TEST_CASE("missing wind block fails", "[mission-validator]") {
    auto r = validateMission(without(kValidMission, "wind: { heading: 270, speed: 12 }"));
    CHECK_FALSE(r.ok);
}

TEST_CASE("wind.heading out of range [0,359] fails", "[mission-validator]") {
    auto r = validateMission(replace_first(kValidMission, "heading: 270", "heading: 360"));
    CHECK_FALSE(r.ok);
}

TEST_CASE("wind.speed negative fails", "[mission-validator]") {
    auto r = validateMission(replace_first(kValidMission, "speed: 12", "speed: -1"));
    CHECK_FALSE(r.ok);
}

TEST_CASE("empty sides list fails", "[mission-validator]") {
    auto r = validateMission(replace_first(kValidMission, "sides: [nato, russia]", "sides: []"));
    CHECK_FALSE(r.ok);
}

TEST_CASE("empty objects list fails", "[mission-validator]") {
    // Replace objects with an empty sequence
    auto r = validateMission("name: x\nmap: y\nlayer: z\n"
                             "time: { hour: 0, minute: 0 }\nwind: { heading: 0, speed: 0 }\n"
                             "sides: [a]\nobjects: []\ntriggers: []\n");
    CHECK_FALSE(r.ok);
}

TEST_CASE("object missing id fails", "[mission-validator]") {
    std::string s = replace_first(kValidMission, "    id: player1\n", "");
    auto r = validateMission(s);
    CHECK_FALSE(r.ok);
    bool found = false;
    for (const auto& e : r.errors)
        if (e.find("id") != std::string::npos) {
            found = true;
            break;
        }
    CHECK(found);
}

TEST_CASE("object pos with 2 components fails", "[mission-validator]") {
    auto r = validateMission(replace_first(kValidMission, "pos: [12400, 0, 8800]", "pos: [12400, 0]"));
    CHECK_FALSE(r.ok);
    bool found = false;
    for (const auto& e : r.errors)
        if (e.find("pos") != std::string::npos) {
            found = true;
            break;
        }
    CHECK(found);
}

TEST_CASE("duplicate object id fails", "[mission-validator]") {
    auto r = validateMission(replace_first(kValidMission, "id: sam1", "id: player1"));
    CHECK_FALSE(r.ok);
    bool found = false;
    for (const auto& e : r.errors)
        if (e.find("duplicated") != std::string::npos) {
            found = true;
            break;
        }
    CHECK(found);
}

TEST_CASE("object side not in sides list fails", "[mission-validator]") {
    auto r = validateMission(replace_first(kValidMission, "side: russia", "side: china"));
    CHECK_FALSE(r.ok);
    bool found = false;
    for (const auto& e : r.errors)
        if (e.find("sides") != std::string::npos) {
            found = true;
            break;
        }
    CHECK(found);
}

TEST_CASE("trigger with destroy referencing unknown id fails", "[mission-validator]") {
    auto r = validateMission(replace_first(kValidMission, "on: destroy(sam1)", "on: destroy(nonexistent)"));
    CHECK_FALSE(r.ok);
    bool found = false;
    for (const auto& e : r.errors)
        if (e.find("nonexistent") != std::string::npos) {
            found = true;
            break;
        }
    CHECK(found);
}

TEST_CASE("trigger with destroy referencing valid id passes", "[mission-validator]") {
    auto r = validateMission(kValidMission);
    CHECK(r.ok);
}

TEST_CASE("trigger missing do field fails", "[mission-validator]") {
    auto r = validateMission(replace_first(kValidMission, "    do: mission_success\n", ""));
    CHECK_FALSE(r.ok);
    bool found = false;
    for (const auto& e : r.errors)
        if (e.find("do") != std::string::npos) {
            found = true;
            break;
        }
    CHECK(found);
}

TEST_CASE("trigger missing on field fails", "[mission-validator]") {
    auto r = validateMission(replace_first(kValidMission, "  - on: destroy(sam1)\n", "  - do: mission_success\n"));
    CHECK_FALSE(r.ok);
}

TEST_CASE("all errors reported in single pass", "[mission-validator]") {
    // Minimal YAML with multiple required fields missing
    auto r = validateMission("name: x\n");
    CHECK_FALSE(r.ok);
    CHECK(r.errors.size() >= 4);
}

TEST_CASE("non-standard trigger on passes without error", "[mission-validator]") {
    // mission_start is not destroy(id) so does not trigger id cross-check
    auto r = validateMission(replace_first(kValidMission, "on: destroy(sam1)", "on: mission_start"));
    CHECK(r.ok);
    CHECK(r.errors.empty());
}

// ---------------------------------------------------------------------------
// Weather and time_scale field validation (issue #39)
// ---------------------------------------------------------------------------

TEST_CASE("weather.preset clear passes", "[mission-validator][weather]") {
    auto r = validateMission(replace_first(kValidMission, "wind: { heading: 270, speed: 12 }",
                                           "wind: { heading: 270, speed: 12 }\nweather:\n  preset: clear"));
    CHECK(r.ok);
}

TEST_CASE("weather.preset partly_cloudy passes", "[mission-validator][weather]") {
    auto r = validateMission(replace_first(kValidMission, "wind: { heading: 270, speed: 12 }",
                                           "wind: { heading: 270, speed: 12 }\nweather:\n  preset: partly_cloudy"));
    CHECK(r.ok);
}

TEST_CASE("weather.preset storm passes", "[mission-validator][weather]") {
    auto r = validateMission(replace_first(kValidMission, "wind: { heading: 270, speed: 12 }",
                                           "wind: { heading: 270, speed: 12 }\nweather:\n  preset: storm"));
    CHECK(r.ok);
}

TEST_CASE("weather.preset snow passes", "[mission-validator][weather]") {
    auto r = validateMission(replace_first(kValidMission, "wind: { heading: 270, speed: 12 }",
                                           "wind: { heading: 270, speed: 12 }\nweather:\n  preset: snow"));
    CHECK(r.ok);
}

TEST_CASE("weather.preset blizzard passes", "[mission-validator][weather]") {
    auto r = validateMission(replace_first(kValidMission, "wind: { heading: 270, speed: 12 }",
                                           "wind: { heading: 270, speed: 12 }\nweather:\n  preset: blizzard"));
    CHECK(r.ok);
}

TEST_CASE("weather.preset invalid value fails", "[mission-validator][weather]") {
    auto r = validateMission(replace_first(kValidMission, "wind: { heading: 270, speed: 12 }",
                                           "wind: { heading: 270, speed: 12 }\nweather:\n  preset: hurricane"));
    CHECK_FALSE(r.ok);
    REQUIRE(!r.errors.empty());
    CHECK(r.errors[0].find("hurricane") != std::string::npos);
}

TEST_CASE("missing weather block passes (field is optional)", "[mission-validator][weather]") {
    auto r = validateMission(kValidMission);
    CHECK(r.ok);
}

TEST_CASE("time_scale positive value passes", "[mission-validator][weather]") {
    auto r = validateMission(replace_first(kValidMission, "wind: { heading: 270, speed: 12 }",
                                           "wind: { heading: 270, speed: 12 }\ntime_scale: 20.0"));
    CHECK(r.ok);
}

TEST_CASE("time_scale zero or negative fails", "[mission-validator][weather]") {
    auto r = validateMission(replace_first(kValidMission, "wind: { heading: 270, speed: 12 }",
                                           "wind: { heading: 270, speed: 12 }\ntime_scale: 0.0"));
    CHECK_FALSE(r.ok);
}

// ── crew: block (#976) ──────────────────────────────────────────────────────

static const char* kCrewMission = R"yaml(
name: "Crew Test"
map: ukraine
layer: ukraine_clear
time: { hour: 14, minute: 0 }
wind: { heading: 270, speed: 12 }
sides: [nato, russia]
objects:
  - type: "test:bomber"
    id: b1
    side: russia
    pos: [15000, 2000, 9000]
    heading: 0
    ai: "loiter 15000 2000 9000"
    crew:
      skill: [0.3, 0.8]
      seats:
        - role: tail-gunner
          skill: 0.9
triggers:
  - on: timer(600)
    do: mission_failure
)yaml";

TEST_CASE("crew: skill range parses (schema-only)", "[mission-validator][crew]") {
    auto r = validateMission(kCrewMission);
    CHECK(r.ok);
}

TEST_CASE("crew: skill out of [0,1] fails", "[mission-validator][crew]") {
    auto r = validateMission(replace_first(kCrewMission, "skill: [0.3, 0.8]", "skill: [0.3, 1.4]"));
    CHECK_FALSE(r.ok);
}

TEST_CASE("crew: a seat with neither seat nor role fails", "[mission-validator][crew]") {
    auto r = validateMission(replace_first(kCrewMission, "- role: tail-gunner", "- skill: 0.9\n        - role: extra"));
    CHECK_FALSE(r.ok);
}

TEST_CASE("crew: --pack cross-check accepts a real seat/role and rejects a bad one", "[mission-validator][crew]") {
    namespace fs = std::filesystem;
    const fs::path pack = fs::temp_directory_path() / "fl_crew_pack_test";
    fs::remove_all(pack);
    fs::create_directories(pack / "entities");
    {
        std::ofstream f(pack / "entities" / "bomber.toml");
        f << R"toml(
[entity]
id = "test:bomber"
name = "Bomber"
category = "air_vehicle"
max_hp = 300

[[hardpoints]]
slot = 0
allowed = ["test:rkt"]
default = "test:rkt"
[[hardpoints]]
slot = 1
allowed = ["test:rkt"]
default = "test:rkt"

[[turrets]]
id = "tail"
stations = [1]

[[crew]]
role = "pilot"
capabilities = ["fly", "fire"]
stations = [0]
[[crew]]
role = "tail-gunner"
capabilities = ["fire"]
turret = "tail"
bot = "gunner"
)toml";
    }
    {
        std::ofstream f(pack / "entities" / "fighter.toml");
        f << R"toml(
[entity]
id = "test:fighter"
name = "Fighter"
category = "air_vehicle"
max_hp = 100
)toml";
    }

    // A valid role (tail-gunner) passes the cross-check.
    CHECK(validateMission(kCrewMission, pack.string()).ok);

    // A role the entity does not declare is an ERROR.
    {
        auto r = validateMission(replace_first(kCrewMission, "role: tail-gunner", "role: bombardier"), pack.string());
        CHECK_FALSE(r.ok);
    }

    // A seat index out of range is an ERROR.
    {
        auto r = validateMission(replace_first(kCrewMission, "role: tail-gunner", "seat: 9"), pack.string());
        CHECK_FALSE(r.ok);
    }

    // A crew: block on a single-seat entity (no [[crew]]) is an ERROR.
    {
        auto r = validateMission(replace_first(kCrewMission, "type: \"test:bomber\"", "type: \"test:fighter\""),
                                 pack.string());
        CHECK_FALSE(r.ok);
    }

    fs::remove_all(pack);
}

// ── cameras: shot schema (#910) ─────────────────────────────────────────────────────────────────

// A mission with one shot of every type, all cross-refs valid. Base for the happy-path + mutations.
static const char* kCameraMission = R"yaml(
name: "Camera Test"
map: world
layer: world_clear
time: { hour: 12, minute: 0 }
wind: { heading: 0, speed: 0 }
sides: [nato, russia]
objects:
  - type: F22
    id: player1
    side: nato
    pos: [0, 300, 0]
    heading: 90
  - type: Su27
    id: bandit1
    side: russia
    pos: [1000, 300, 0]
    heading: 270
triggers:
  - on: destroy(bandit1)
    do: mission_success
cameras:
  shots:
    - { type: static, start: 0, duration: 8, pos: [1200, 250, -300], look_at: player1, fov: 60 }
    - { type: orbit, start: 8, duration: 12, target: bandit1, radius: 400, height: 60, period: 30 }
    - { type: chase, start: 20, duration: 15, target: player1, offset: [-60, 15, 0], stiffness: 4.0, look_at: bandit1 }
    - { type: move, start: 35, duration: 7, look_at: player1, ease: smooth, keyframes: [ { time: 0, pos: [800, 300, -100] }, { time: 7, pos: [-200, 280, 150] } ] }
)yaml";

TEST_CASE("cameras happy path parses all four shot types", "[mission-validator][cameras]") {
    auto r = parseMission(kCameraMission);
    CHECK(r.ok);
    REQUIRE(r.errors.empty());
    REQUIRE(r.mission.shots.size() == 4);
    CHECK(r.mission.shots[0].type == ShotType::Static);
    CHECK(r.mission.shots[1].type == ShotType::Orbit);
    CHECK(r.mission.shots[2].type == ShotType::Chase);
    CHECK(r.mission.shots[3].type == ShotType::Move);
    // static defaults + values
    CHECK(r.mission.shots[0].fovYDeg == 60.f);
    CHECK(r.mission.shots[0].lookAtId == "player1");
    CHECK(r.mission.shots[0].pos[0] == 1200.0);
    // orbit values
    CHECK(r.mission.shots[1].targetId == "bandit1");
    CHECK(r.mission.shots[1].orbitRadiusM == 400.0);
    CHECK(r.mission.shots[1].orbitPeriodSec == 30.0);
    // chase values
    CHECK(r.mission.shots[2].targetId == "player1");
    CHECK(r.mission.shots[2].chaseStiffness == 4.0);
    CHECK(r.mission.shots[2].lookAtId == "bandit1");
    CHECK(r.mission.shots[2].chaseOffset[0] == -60.0);
    // move keyframes
    REQUIRE(r.mission.shots[3].keyframes.size() == 2);
    CHECK(r.mission.shots[3].ease == ShotEase::Smooth);
    CHECK(r.mission.shots[3].keyframes[1].pos[2] == 150.0);
}

TEST_CASE("cameras defaults applied when omitted", "[mission-validator][cameras]") {
    // Orbit with only the required target; static look_at as a fixed point.
    const char* yaml = R"yaml(
name: "D"
map: world
layer: world_clear
time: { hour: 12, minute: 0 }
wind: { heading: 0, speed: 0 }
sides: [nato]
objects:
  - { type: F22, id: a, side: nato, pos: [0,0,0], heading: 0 }
triggers:
  - { on: mission_start, do: mission_failure }
cameras:
  shots:
    - { type: orbit, start: 0, duration: 5, target: a }
    - { type: static, start: 5, duration: 5, pos: [10,20,30], look_at: [1,2,3] }
)yaml";
    auto r = parseMission(yaml);
    CHECK(r.ok);
    REQUIRE(r.mission.shots.size() == 2);
    CHECK(r.mission.shots[0].orbitRadiusM == 300.0);  // default
    CHECK(r.mission.shots[0].orbitHeightM == 50.0);   // default
    CHECK(r.mission.shots[0].orbitPeriodSec == 30.0); // default
    CHECK(r.mission.shots[0].fovYDeg == 60.f);        // default
    CHECK(r.mission.shots[1].lookAtPointSet);
    CHECK_FALSE(r.mission.shots[1].lookAtId.size());
    CHECK(r.mission.shots[1].lookAtPoint[1] == 2.0);
}

TEST_CASE("cameras absent leaves shots empty and mission valid", "[mission-validator][cameras]") {
    auto r = parseMission(kValidMission);
    CHECK(r.ok);
    CHECK(r.mission.shots.empty());
}

TEST_CASE("cameras unknown shot type is an error", "[mission-validator][cameras]") {
    auto r = parseMission(replace_first(kCameraMission, "type: static", "type: dolly"));
    CHECK_FALSE(r.ok);
}

TEST_CASE("cameras overlapping shots are an error", "[mission-validator][cameras]") {
    // Shift the second shot to start at 4, inside the first shot's [0, 8) span.
    auto r = parseMission(replace_first(kCameraMission, "type: orbit, start: 8", "type: orbit, start: 4"));
    CHECK_FALSE(r.ok);
    bool overlap = false;
    for (const auto& e : r.errors)
        if (e.find("overlap") != std::string::npos)
            overlap = true;
    CHECK(overlap);
}

TEST_CASE("cameras unknown target id is an error", "[mission-validator][cameras]") {
    auto r = parseMission(replace_first(kCameraMission, "target: bandit1, radius", "target: ghost, radius"));
    CHECK_FALSE(r.ok);
}

TEST_CASE("cameras unknown look_at id is an error", "[mission-validator][cameras]") {
    auto r = parseMission(replace_first(kCameraMission, "look_at: player1, fov", "look_at: ghost, fov"));
    CHECK_FALSE(r.ok);
}

TEST_CASE("cameras fov out of range is an error", "[mission-validator][cameras]") {
    auto r = parseMission(replace_first(kCameraMission, "fov: 60", "fov: 200"));
    CHECK_FALSE(r.ok);
}

TEST_CASE("cameras move with fewer than two keyframes is an error", "[mission-validator][cameras]") {
    auto r = parseMission(replace_first(
        kCameraMission, "keyframes: [ { time: 0, pos: [800, 300, -100] }, { time: 7, pos: [-200, 280, 150] } ]",
        "keyframes: [ { time: 0, pos: [800, 300, -100] } ]"));
    CHECK_FALSE(r.ok);
}

TEST_CASE("cameras static without look_at is an error", "[mission-validator][cameras]") {
    auto r = parseMission(replace_first(
        kCameraMission, "type: static, start: 0, duration: 8, pos: [1200, 250, -300], look_at: player1, fov: 60",
        "type: static, start: 0, duration: 8, pos: [1200, 250, -300], fov: 60"));
    CHECK_FALSE(r.ok);
}

TEST_CASE("cameras-only sidecar doc parses shots via the same entry point", "[mission-validator][cameras]") {
    // A --shot-track sidecar: just a cameras: block, no objects. It is not a valid *mission* (missing
    // name/map/objects/…), but the shots parse and its id cross-refs are not checked (no objects).
    const char* sidecar = R"yaml(
cameras:
  shots:
    - { type: orbit, start: 0, duration: 10, target: whoever, radius: 500 }
    - { type: static, start: 10, duration: 5, pos: [0, 100, 0], look_at: [0,0,0] }
)yaml";
    auto r = parseMission(sidecar);
    CHECK_FALSE(r.ok); // missing required mission fields
    REQUIRE(r.mission.shots.size() == 2);
    CHECK(r.mission.shots[0].targetId == "whoever");
    CHECK(r.mission.shots[0].orbitRadiusM == 500.0);
    // The unknown target id did NOT produce an error (sidecar has no objects to check against).
    for (const auto& e : r.errors)
        CHECK(e.find("whoever") == std::string::npos);
}
