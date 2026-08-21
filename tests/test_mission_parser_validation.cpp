// SPDX-License-Identifier: GPL-3.0-or-later
//
// MissionParser schema validation (#1145). The parser is ~90 distinct rejections across mission
// header, sides, airspace zones, objects, triggers and the cinematic camera track; the happy path
// and the validator tool are covered elsewhere, and this file is the rejections themselves.
//
// Structured as perturbations of one valid mission, because that is how authoring failures actually
// arrive: a working file with one thing wrong in it. Each case asserts the specific diagnosis, not
// merely `!ok` — a parser that rejects everything with the same message is no better than a crash.

#include <catch2/catch_test_macros.hpp>

#include "mission/MissionParser.h"

#include <algorithm>
#include <string>

using namespace fl;

namespace {

constexpr const char* kValid = R"yaml(
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
)yaml";

bool mentions(const std::vector<std::string>& msgs, std::string_view needle) {
    return std::any_of(msgs.begin(), msgs.end(),
                       [needle](const std::string& m) { return m.find(needle) != std::string::npos; });
}

// Replace a line of the base mission (matched by its leading text) with `to`; empty `to` deletes it.
std::string swapLine(std::string_view from, std::string_view to) {
    std::string s(kValid);
    const auto pos = s.find(from);
    REQUIRE(pos != std::string::npos);
    const auto end = s.find('\n', pos);
    s.replace(pos, end - pos, to);
    return s;
}

std::string append(std::string_view extra) {
    return std::string(kValid) + std::string(extra);
}

// Insert an extra entry INTO the objects block. append() puts text at the end of the document,
// which for an object entry lands it under `triggers:` where the parser never looks at it — the
// first draft of this file "tested" five object rules that way and asserted nothing.
std::string withObject(std::string_view objectYaml) {
    std::string s(kValid);
    const auto pos = s.find("triggers:");
    REQUIRE(pos != std::string::npos);
    s.insert(pos, std::string(objectYaml));
    return s;
}

// Replace the whole `triggers:` block (it is the last block in the base document). A line swap
// there leaves the following `do:` line orphaned under the new entry, which changes the document
// into something other than the case under test.
std::string withTriggers(std::string_view triggersYaml) {
    std::string s(kValid);
    const auto pos = s.find("triggers:");
    REQUIRE(pos != std::string::npos);
    s.erase(pos);
    return s + std::string(triggersYaml);
}

// Every negative case: perturb, expect !ok AND the specific diagnosis.
void rejects(const std::string& doc, std::string_view diagnosis) {
    const auto r = parseMission(doc);
    INFO("expected diagnosis: " << diagnosis);
    CHECK_FALSE(r.ok);
    CHECK(mentions(r.errors, diagnosis));
}

} // namespace

TEST_CASE("parseMission: the base mission is valid (#1145)", "[mission][parser]") {
    const auto r = parseMission(kValid);
    if (!r.errors.empty())
        INFO(r.errors[0]);
    CHECK(r.ok);
    CHECK(r.mission.name == "Storm Warning");
    CHECK(r.mission.time.hour == 14);
    CHECK(r.mission.wind.headingDeg == 270.f);
}

// ---------------------------------------------------------------------------
// Document shape and header
// ---------------------------------------------------------------------------

TEST_CASE("parseMission: malformed and non-mapping documents (#1145)", "[mission][parser]") {
    rejects("name: [unclosed\n :: bad", "YAML parse error");
    rejects("- a\n- b\n", "must be a YAML mapping");
    rejects("just a scalar\n", "must be a YAML mapping");
}

TEST_CASE("parseMission: name, map and layer are required strings (#1145)", "[mission][parser]") {
    rejects(swapLine("name:", ""), "missing required field: name");
    rejects(swapLine("map:", ""), "missing required field: map");
    rejects(swapLine("layer:", ""), "missing required field: layer");
    rejects(swapLine("name:", "name: { not: scalar }"), "name must be a string");
    rejects(swapLine("map:", "map: [a, b]"), "map must be a string");
}

TEST_CASE("parseMission: time is required and bounded (#1145)", "[mission][parser]") {
    rejects(swapLine("time:", ""), "missing required field: time");
    rejects(swapLine("time:", "time: { minute: 0 }"), "missing time.hour");
    rejects(swapLine("time:", "time: { hour: 14 }"), "missing time.minute");
    rejects(swapLine("time:", "time: { hour: 24, minute: 0 }"), "time.hour must be in");
    rejects(swapLine("time:", "time: { hour: -1, minute: 0 }"), "time.hour must be in");
    rejects(swapLine("time:", "time: { hour: 12, minute: 60 }"), "time.minute must be in");
    rejects(swapLine("time:", "time: { hour: 12, minute: -5 }"), "time.minute must be in");
}

TEST_CASE("parseMission: wind is required and bounded (#1145)", "[mission][parser]") {
    rejects(swapLine("wind:", ""), "missing required field: wind");
    rejects(swapLine("wind:", "wind: { speed: 5 }"), "missing wind.heading");
    rejects(swapLine("wind:", "wind: { heading: 90 }"), "missing wind.speed");
    rejects(swapLine("wind:", "wind: { heading: 361, speed: 5 }"), "wind.heading must be in");
    rejects(swapLine("wind:", "wind: { heading: -1, speed: 5 }"), "wind.heading must be in");
    rejects(swapLine("wind:", "wind: { heading: 90, speed: -2 }"), "wind.speed must be >= 0");
}

TEST_CASE("parseMission: the weather preset vocabulary is closed (#1145)", "[mission][parser]") {
    rejects(append("weather: { preset: apocalypse }\n"), "weather.preset must be");

    // Every accepted spelling parses — a vocabulary test that only checks the rejection would not
    // notice a preset going missing from the table.
    for (const char* p : {"clear", "partly_cloudy", "overcast", "rain", "storm", "snow", "blizzard"}) {
        const auto r = parseMission(append(std::string("weather: { preset: ") + p + " }\n"));
        INFO("preset " << p);
        CHECK(r.ok);
    }
}

TEST_CASE("parseMission: time_scale must be positive (#1145)", "[mission][parser]") {
    rejects(append("time_scale: 0\n"), "time_scale must be > 0");
    rejects(append("time_scale: -1.5\n"), "time_scale must be > 0");
    CHECK(parseMission(append("time_scale: 2.0\n")).ok);
}

// ---------------------------------------------------------------------------
// sides and coalitions
// ---------------------------------------------------------------------------

TEST_CASE("parseMission: sides must be a non-empty sequence (#1145)", "[mission][parser]") {
    rejects(swapLine("sides:", ""), "missing required field: sides");
    rejects(swapLine("sides:", "sides: nato"), "sides must be a sequence");
    rejects(swapLine("sides:", "sides: []"), "sides must have at least");
}

TEST_CASE("parseMission: a side mapping needs an id, and allies must resolve (#1145)", "[mission][parser]") {
    rejects(swapLine("sides:", "sides: [{ allies: [x] }, russia]"), "missing required field: id");
    rejects(swapLine("sides:", "sides: [{ id: nato, allies: notalist }, russia]"), "allies must be a sequence");
    rejects(swapLine("sides:", "sides: [{ id: nato, allies: [atlantis] }, russia]"), "references unknown side");
    rejects(swapLine("sides:", "sides: [[nested], russia]"), "must be a string or a mapping");

    const auto ok = parseMission(swapLine("sides:", "sides: [{ id: nato, allies: [russia] }, russia]"));
    CHECK(ok.ok);
}

TEST_CASE("parseMission: an object's side must be declared (#1145)", "[mission][parser]") {
    rejects(swapLine("    side: nato", "    side: atlantis"), "is not in the sides list");
}

// ---------------------------------------------------------------------------
// objects
// ---------------------------------------------------------------------------

TEST_CASE("parseMission: objects is required, a sequence, and non-empty (#1145)", "[mission][parser]") {
    const std::string noObjects = R"yaml(
name: N
map: m
layer: l
time: { hour: 1, minute: 0 }
wind: { heading: 0, speed: 0 }
sides: [nato]
triggers:
  - on: timer(1)
    do: mission_success
)yaml";
    rejects(noObjects, "missing required field: objects");
    rejects(noObjects + "objects: nope\n", "objects must be a sequence");
    rejects(noObjects + "objects: []\n", "objects must have at least");
}

TEST_CASE("parseMission: object entries need type, id, side and a 3-component pos (#1145)", "[mission][parser]") {
    rejects(swapLine("    id: player1", ""), "missing required field: id");
    rejects(swapLine("  - type: F22", "  - id: nope"), "missing required field: type");
    rejects(swapLine("    pos: [12400, 0, 8800]", ""), "missing required field: pos");
    rejects(swapLine("    pos: [12400, 0, 8800]", "    pos: 12400"), "pos must be a sequence");
    rejects(swapLine("    pos: [12400, 0, 8800]", "    pos: [1, 2]"), "pos must have exactly");
    rejects(withObject("  - just_a_scalar\n"), "is not a mapping");
}

TEST_CASE("parseMission: duplicate object ids are rejected (#1145)", "[mission][parser]") {
    rejects(swapLine("    id: sam1", "    id: player1"), "is duplicated");
}

TEST_CASE("parseMission: optional object fields are validated when present (#1145)", "[mission][parser]") {
    rejects(withObject("  - type: F22\n    id: x\n    side: nato\n    pos: [0,0,0]\n    speed: -5\n"),
            "speed must be >= 0");
    rejects(withObject("  - type: F22\n    id: y\n    side: nato\n    pos: [0,0,0]\n    ai: [not, a, string]\n"),
            "ai must be a string");
    rejects(withObject("  - type: F22\n    id: z\n    side: nato\n    pos: [0,0,0]\n    route: notalist\n"),
            "route must be a sequence");
    rejects(withObject("  - type: F22\n    id: w\n    side: nato\n    pos: [0,0,0]\n    loadout: notalist\n"),
            "loadout must be a sequence");
    rejects(withObject("  - type: F22\n    id: v\n    side: nato\n    pos: [0,0,0]\n    crew: notamap\n"),
            "crew must be a mapping");
}

// ---------------------------------------------------------------------------
// airspace zones
// ---------------------------------------------------------------------------

TEST_CASE("parseMission: airspace zones validate shape and ownership (#1145)", "[mission][parser]") {
    auto zone = [](std::string_view body) { return append(std::string("airspace_zones:\n") + std::string(body)); };

    rejects(append("airspace_zones: notalist\n"), "airspace_zones must be a sequence");
    rejects(zone("  - scalar\n"), "must be a mapping");
    rejects(zone("  - { type: circle }\n"), "missing required field: id");
    rejects(zone("  - { id: z1 }\n"), "missing required field: type");
    rejects(zone("  - { id: z1, type: blob, owner: nato }\n"), "type must be circle|polygon");
    rejects(zone("  - { id: z1, type: circle, owner: nato }\n"), "missing required field: center");
    rejects(zone("  - { id: z1, type: circle, center: [0, 0], owner: nato }\n"), "center must have exactly");
    rejects(zone("  - { id: z1, type: circle, center: [0,0,0], owner: nato }\n"), "missing required field: radius");
    rejects(zone("  - { id: z1, type: circle, center: [0,0,0], radius: 0, owner: nato }\n"), "radius must be > 0");
    rejects(zone("  - { id: z1, type: polygon, owner: nato }\n"), "missing required field: vertices");
    rejects(zone("  - { id: z1, type: polygon, vertices: [[0,0],[1,1]], owner: nato }\n"), "needs at least");
    rejects(zone("  - { id: z1, type: circle, center: [0,0,0], radius: 100 }\n"), "missing required field: owner");
    rejects(zone("  - { id: z1, type: circle, center: [0,0,0], radius: 100, owner: atlantis }\n"),
            "is not in the sides list");
    rejects(zone("  - { id: z1, type: circle, center: [0,0,0], radius: 100, owner: nato }\n"
                 "  - { id: z1, type: circle, center: [0,0,0], radius: 100, owner: nato }\n"),
            "is duplicated");
    rejects(zone("  - { id: z1, type: circle, center: [0,0,0], radius: 100, owner: nato,"
                 " alt_floor: 5000, alt_ceiling: 1000 }\n"),
            "alt_ceiling must be greater than alt_floor");

    const auto ok = parseMission(zone("  - { id: z1, type: circle, center: [0,0,0], radius: 5000, owner: nato }\n"));
    CHECK(ok.ok);
}

// ---------------------------------------------------------------------------
// triggers
// ---------------------------------------------------------------------------

TEST_CASE("parseMission: triggers are required and reference real objects (#1145)", "[mission][parser]") {
    rejects(withTriggers(""), "missing required field: triggers");
    rejects(withTriggers("triggers: notalist\n"), "triggers must be a sequence");
    rejects(withTriggers("triggers:\n  - scalar\n"), "is not a mapping");
    rejects(withTriggers("triggers:\n  - { do: mission_success }\n"), "missing required field: on");
    rejects(withTriggers("triggers:\n  - { on: timer(5) }\n"), "missing required field: do");
    rejects(withTriggers("triggers:\n  - { on: destroy(ghost), do: mission_success }\n"),
            "references unknown object id");

    CHECK(parseMission(withTriggers("triggers:\n  - { on: timer(60), do: mission_failure }\n")).ok);
}

TEST_CASE("parseMission: a malformed destroy/timer ref is an error, not silently unchecked (#1239)",
          "[mission][parser]") {
    // The defect: `destroy(sam1))` matched neither the validator's old regex (so the unknown-id
    // check never ran and validation PASSED) nor anything the runtime could fire. The shared
    // grammar makes it a validation error.
    rejects(withTriggers("triggers:\n  - { on: \"destroy(sam1))\", do: mission_success }\n"), "malformed trigger ref");
    rejects(withTriggers("triggers:\n  - { on: \"timer(5))\", do: mission_success }\n"), "malformed trigger ref");
    rejects(withTriggers("triggers:\n  - { on: \"destroy()\", do: mission_success }\n"), "malformed trigger ref");

    // Unrelated predicates stay legal and unchecked — reach/zone extensions and Lua-only
    // predicates never fire in the builtin evaluator by design (missions.md).
    CHECK(parseMission(withTriggers("triggers:\n  - { on: zone_entered, do: mission_success }\n")).ok);
}

TEST_CASE("parseMission: a timer argument has to be a number (#1244)", "[mission][parser]") {
    // The runtime parses this strictly, so a non-numeric timer would validate and then never fire.
    // Same class of trigger #1239 set out to make impossible: accepted here, impossible there.
    rejects(withTriggers("triggers:\n  - { on: \"timer(soon)\", do: mission_success }\n"), "non-numeric timer");
    rejects(withTriggers("triggers:\n  - { on: \"timer(5s)\", do: mission_success }\n"), "non-numeric timer");
    rejects(withTriggers("triggers:\n  - { on: \"timer(  )\", do: mission_success }\n"), "non-numeric timer");

    CHECK(parseMission(withTriggers("triggers:\n  - { on: \"timer(0)\", do: mission_success }\n")).ok);
    CHECK(parseMission(withTriggers("triggers:\n  - { on: \"timer(2.5)\", do: mission_success }\n")).ok);
    CHECK(parseMission(withTriggers("triggers:\n  - { on: \"timer(1e3)\", do: mission_success }\n")).ok);
}

// ---------------------------------------------------------------------------
// cinematic camera track
// ---------------------------------------------------------------------------

TEST_CASE("parseMission: the camera track validates its shot vocabulary (#1145)", "[mission][parser]") {
    auto cam = [](std::string_view shots) { return append(std::string("cameras:\n  shots:\n") + std::string(shots)); };

    rejects(append("cameras: notamap\n"), "cameras must be a mapping");
    rejects(append("cameras:\n  shots: notalist\n"), "cameras.shots must be a sequence");
    rejects(cam("    - scalar\n"), "must be a mapping");
    rejects(cam("    - { start: 0, duration: 5 }\n"), "missing required field: type");
    rejects(cam("    - { type: teleport, start: 0, duration: 5 }\n"), "type must be static|orbit|chase|move");
    rejects(cam("    - { type: static, duration: 5 }\n"), "missing required field: start");
    rejects(cam("    - { type: static, start: -1, duration: 5 }\n"), "start must be >= 0");
    rejects(cam("    - { type: static, start: 0 }\n"), "missing required field: duration");
    rejects(cam("    - { type: static, start: 0, duration: 0 }\n"), "duration must be > 0");
    rejects(cam("    - { type: static, start: 0, duration: 5, look_at: player1 }\n"),
            "(static) missing required field: pos");
    rejects(cam("    - { type: static, start: 0, duration: 5, pos: [0,0,0] }\n"),
            "(static) missing required field: look_at");
    rejects(cam("    - { type: orbit, start: 0, duration: 5 }\n"), "(orbit) missing required field: target");
    rejects(cam("    - { type: chase, start: 0, duration: 5 }\n"), "(chase) missing required field: target");
    rejects(cam("    - { type: move, start: 0, duration: 5, look_at: player1 }\n"),
            "(move) missing required field: keyframes");
    rejects(cam("    - { type: static, start: 0, duration: 5, pos: [0,0,0], look_at: ghost }\n"),
            "references unknown object id");
    rejects(cam("    - { type: static, start: 0, duration: 5, pos: [0,0], look_at: player1 }\n"), "must have exactly");

    const auto ok =
        parseMission(cam("    - { type: static, start: 0, duration: 5, pos: [0,100,0], look_at: player1 }\n"));
    if (!ok.errors.empty())
        INFO(ok.errors[0]);
    CHECK(ok.ok);
}

TEST_CASE("parseMission: orbit and chase shots validate their own knobs (#1145)", "[mission][parser]") {
    auto cam = [](std::string_view shots) { return append(std::string("cameras:\n  shots:\n") + std::string(shots)); };
    rejects(cam("    - { type: orbit, start: 0, duration: 5, target: player1, period: 0 }\n"),
            "period must be non-zero");
    rejects(cam("    - { type: chase, start: 0, duration: 5, target: player1, stiffness: -1 }\n"),
            "stiffness must be >= 0");
    rejects(cam("    - { type: static, start: 0, duration: 5, pos: [0,0,0], look_at: player1, fov: 400 }\n"),
            "fov must be in");
}

TEST_CASE("parseMission: move shots need enough keyframes, well-formed (#1145)", "[mission][parser]") {
    auto cam = [](std::string_view shots) { return append(std::string("cameras:\n  shots:\n") + std::string(shots)); };
    rejects(cam("    - { type: move, start: 0, duration: 5, look_at: player1, keyframes: [] }\n"),
            "keyframes must have at least");
    rejects(cam("    - type: move\n      start: 0\n      duration: 5\n      look_at: player1\n"
                "      keyframes:\n        - scalar\n        - scalar\n"),
            "must be a mapping with `time` and `pos`");
    rejects(cam("    - type: move\n      start: 0\n      duration: 5\n      look_at: player1\n      ease: bouncy\n"
                "      keyframes:\n        - { time: 0, pos: [0,0,0] }\n        - { time: 1, pos: [1,1,1] }\n"),
            "ease must be linear|smooth");
}
