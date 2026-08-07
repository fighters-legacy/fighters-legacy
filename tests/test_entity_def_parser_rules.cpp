// SPDX-License-Identifier: GPL-3.0-or-later
//
// EntityDefParser's rejection rules (#1145) — hardpoints, turrets, crew seats, sensors and decks.
//
// This parser is the gate between a downloaded content pack and the entity registry. Its rules are
// the ones a mod author trips over, and each one exists because the alternative is an entity that
// registers and then misbehaves: a hardpoint whose default store it cannot carry, two seats claiming
// the same slot, a deck nothing can land on.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "entity/EntityDefParser.h"

#include <stdexcept>
#include <string>

using Catch::Matchers::ContainsSubstring;
using namespace fl;

namespace {

// The smallest def that parses. Every negative case appends to or perturbs it.
constexpr const char* kMinimal = R"([entity]
id = "test:jet"
name = "Test Jet"
category = "air_vehicle"
max_hp = 100.0
)";

std::string with(std::string_view extra) {
    return std::string(kMinimal) + std::string(extra);
}

// A ROOT-level key. Appending after kMinimal would scope it into [entity] instead — the exact
// footgun the parser's unknown-key check exists for, and one that quietly made three of these
// cases assert nothing at all.
std::string atRoot(std::string_view rootKeys) {
    return std::string(rootKeys) + std::string(kMinimal);
}

void rejects(const std::string& toml, std::string_view diagnosis) {
    INFO("expected diagnosis: " << diagnosis);
    REQUIRE_THROWS_WITH(parseEntityDef(toml), ContainsSubstring(std::string(diagnosis)));
}

} // namespace

TEST_CASE("parseEntityDef: the minimal def parses (#1145)", "[entity][parser]") {
    const EntityDef d = parseEntityDef(kMinimal);
    CHECK(d.id == "test:jet");
    CHECK(d.name == "Test Jet");
    CHECK(d.maxHp == 100.f);
}

// ---------------------------------------------------------------------------
// Document and identity
// ---------------------------------------------------------------------------

TEST_CASE("parseEntityDef: malformed TOML and a missing [entity] table (#1145)", "[entity][parser]") {
    rejects("[entity\nid = ", "entity def parse error");
    rejects("[something_else]\nid = \"x\"\n", "missing required table [entity]");
}

TEST_CASE("parseEntityDef: id and name are required (#1145)", "[entity][parser]") {
    rejects("[entity]\nname = \"N\"\ncategory = \"air_vehicle\"\nmax_hp = 1.0\n", "missing required field");
    rejects("[entity]\nid = \"x\"\ncategory = \"air_vehicle\"\nmax_hp = 1.0\n", "missing required field");
}

TEST_CASE("parseEntityDef: the category vocabulary is closed (#1145)", "[entity][parser]") {
    rejects("[entity]\nid = \"x\"\nname = \"X\"\ncategory = \"spacecraft\"\nmax_hp = 1.0\n", "unknown category");
}

TEST_CASE("parseEntityDef: projectile_kind belongs only to projectiles (#1145)", "[entity][parser]") {
    rejects(with("projectile_kind = \"missile\"\n"), "only valid on category");
    rejects("[entity]\nid = \"x\"\nname = \"X\"\ncategory = \"projectile\"\nmax_hp = 1.0\n"
            "projectile_kind = \"deathray\"\n",
            "unknown projectile_kind");
}

// ---------------------------------------------------------------------------
// Hardpoints
// ---------------------------------------------------------------------------

TEST_CASE("parseEntityDef: hardpoints must be an array of tables (#1145)", "[entity][parser]") {
    rejects(atRoot("hardpoints = [1, 2]\n"), "must be an array of tables");
}

TEST_CASE("parseEntityDef: a hardpoint needs a non-negative slot (#1145)", "[entity][parser]") {
    rejects(with("[[hardpoints]]\nallowed = [\"w:aim9\"]\n"), "missing required field: hardpoints.slot");
    rejects(with("[[hardpoints]]\nslot = -1\nallowed = [\"w:aim9\"]\n"), "slot must be >= 0");
}

TEST_CASE("parseEntityDef: duplicate hardpoint slots are rejected (#1145)", "[entity][parser]") {
    // Two stations numbered the same is unresolvable at load-out time — which physical pylon did the
    // player just fill?
    rejects(with("[[hardpoints]]\nslot = 1\nallowed = [\"w:a\"]\ndefault = \"w:a\"\n\n"
                 "[[hardpoints]]\nslot = 1\nallowed = [\"w:b\"]\ndefault = \"w:b\"\n"),
            "duplicate hardpoints.slot: 1");
}

TEST_CASE("parseEntityDef: a hardpoint's allowed list must be non-empty strings (#1145)", "[entity][parser]") {
    rejects(with("[[hardpoints]]\nslot = 1\n"), "non-empty array of weapon ids");
    rejects(with("[[hardpoints]]\nslot = 1\nallowed = []\n"), "non-empty array of weapon ids");
    rejects(with("[[hardpoints]]\nslot = 1\nallowed = [\"\"]\n"), "must be non-empty strings");
}

TEST_CASE("parseEntityDef: a default store must be one the station accepts (#1145)", "[entity][parser]") {
    // Otherwise the aircraft spawns carrying something its own pylon rejects.
    rejects(with("[[hardpoints]]\nslot = 1\nallowed = [\"w:aim9\"]\ndefault = \"w:aim120\"\n"), "default");

    CHECK_NOTHROW(parseEntityDef(with("[[hardpoints]]\nslot = 1\nallowed = [\"w:aim9\"]\ndefault = \"w:aim9\"\n")));
}

// ---------------------------------------------------------------------------
// Turrets, crew and sensors
// ---------------------------------------------------------------------------

TEST_CASE("parseEntityDef: turrets must be an array of tables (#1145)", "[entity][parser]") {
    rejects(atRoot("turrets = [1]\n"), "must be an array of tables");
}

TEST_CASE("parseEntityDef: crew must be an array of tables (#1145)", "[entity][parser]") {
    rejects(atRoot("crew = [\"pilot\"]\n"), "must be an array of tables");
}

TEST_CASE("parseEntityDef: a crew seat needs capabilities from the known vocabulary (#1145)", "[entity][parser]") {
    rejects(with("[[crew]]\nrole = \"gunner\"\n"), "capabilities must be a non-empty array");
    rejects(with("[[crew]]\nrole = \"gunner\"\ncapabilities = []\n"), "capabilities must be a non-empty array");
    rejects(with("[[crew]]\nrole = \"gunner\"\ncapabilities = [\"\"]\n"), "entries must be non-empty strings");
    rejects(with("[[crew]]\nrole = \"gunner\"\ncapabilities = [\"telepathy\"]\n"), "unknown crew capability");
}

TEST_CASE("parseEntityDef: a seat cannot be both bot-filled and empty (#1145)", "[entity][parser]") {
    // `bot` is a bot SPEC string, not a flag — `bot = true` is simply ignored, so the conflict only
    // exists between a named bot and empty = true.
    rejects(with("[[crew]]\nrole = \"pilot\"\ncapabilities = [\"fly\"]\n\n"
                 "[[crew]]\nrole = \"wso\"\ncapabilities = [\"radar\"]\nbot = \"veteran\"\nempty = true\n"),
            "declares both bot and empty");
}

TEST_CASE("parseEntityDef: crew damage and hit weight are bounded (#1145)", "[entity][parser]") {
    rejects(with("[[crew]]\nrole = \"gunner\"\ncapabilities = [\"radar\"]\ndamage_hp = -1.0\n"),
            "damage_hp must be >= 0");
    rejects(with("[[crew]]\nrole = \"gunner\"\ncapabilities = [\"radar\"]\nhit_weight = 0.0\n"),
            "hit_weight must be > 0");
}

TEST_CASE("parseEntityDef: the sensor list must be unique non-empty ids (#1145)", "[entity][parser]") {
    rejects(with("sensors = \"radar\"\n"), "must be an array of sensor ids");
    rejects(with("sensors = [\"\"]\n"), "entries must be non-empty strings");
    rejects(with("sensors = [\"s:radar\", \"s:radar\"]\n"), "duplicate entity.sensors id");

    CHECK_NOTHROW(parseEntityDef(with("sensors = [\"s:radar\", \"s:irst\"]\n")));
}

// ---------------------------------------------------------------------------
// Decks (#38)
// ---------------------------------------------------------------------------

TEST_CASE("parseEntityDef: a deck must accept landings and have real dimensions (#1145)", "[entity][parser]") {
    const std::string ship = R"([entity]
id = "test:carrier"
name = "Test Carrier"
category = "naval_vehicle"
max_hp = 10000.0
)";
    // accepts_landings is an [entity] key; the deck table only carries geometry.
    const std::string lands = ship + "accepts_landings = true\n";
    rejects(ship + "[deck]\nlength_m = 300.0\nwidth_m = 70.0\nheight_m = 20.0\n", "requires accepts_landings = true");
    rejects(lands + "[deck]\nlength_m = 0.0\nwidth_m = 70.0\nheight_m = 20.0\n", "must be > 0");
    rejects(lands + "[deck]\nlength_m = 300.0\nwidth_m = 0.0\nheight_m = 20.0\n", "must be > 0");
    rejects(lands + "[deck]\nwidth_m = 70.0\nheight_m = 20.0\n", "missing required field");

    CHECK_NOTHROW(parseEntityDef(lands + "[deck]\nlength_m = 300.0\nwidth_m = 70.0\nheight_m = 20.0\n"));
}

// ---------------------------------------------------------------------------
// Numeric bounds and array shapes
// ---------------------------------------------------------------------------

TEST_CASE("parseEntityDef: a signature multiplier is bounded, and zero is not stealth (#1145)", "[entity][parser]") {
    // (0, 100]: a signature of 0 is not "very stealthy", it is a target no sensor of that type can
    // ever detect at any range — an author who wants that has to say so with a number.
    rejects(with("[signatures]\nrcs = 0.0\n"), "must be in (0, 100]");
    rejects(with("[signatures]\nrcs = -1.0\n"), "must be in (0, 100]");
    rejects(with("[signatures]\nrcs = 200.0\n"), "must be in (0, 100]");
    CHECK_NOTHROW(parseEntityDef(with("[signatures]\nrcs = 0.3\n")));
}

TEST_CASE("parseEntityDef: an unknown key inside a known table is a typo, not a comment (#1145)", "[entity][parser]") {
    // A silently-ignored key is how "why is my setting not doing anything" happens.
    rejects(with("[signatures]\nnot_a_real_key = 1\n"), "unknown key in [");
}
