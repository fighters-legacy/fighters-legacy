// SPDX-License-Identifier: GPL-3.0-or-later
//
// buildAiController (#1236): the ONE AI-controller construction ladder behind the admin `spawn`
// command and the mission `onSpawned` path. Three copies of it had drifted — the admin copy handed
// LuaController the ATC service and both mission copies did not, so the same script reached atc.*
// (#705) or silently did not depending on which block built it.
//
// The parity these cases pin is the helper's: whatever the caller puts in `atcService` is what the
// script gets. Since #1288 BOTH callers put the same thing there -- AtcService construction moved to
// initWorld, ahead of initMission, so a mission-attached script is no longer handed null while an
// admin-spawned one gets the live service. The null case below is now only the [atc] enabled = false
// configuration, which both paths reach together.
//
// This file cannot see the wiring itself, only the ladder it feeds. The end-to-end pin for #1288 is
// the mission_harness_atc_scramble ctest: a mission-attached script calls atc.scramble and the run
// is asserted to end with the departure ATC launched still flying.

#include "AiControllerBuild.h"
#include "mock_log.h"

#include "ILogger.h"
#include "atc/AtcService.h"
#include "entity/EntityManager.h"
#include "entity/EntityTypeRegistry.h"
#include "flight/Geodetic.h"
#include "world/AirportRegistry.h"
#include "world/BuiltinAirport.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace fl;

namespace {

EntityState makeState() {
    EntityState s{};
    s.transform.quat[3] = 1.f;
    return s;
}

// A script that reports, on its first tick, whether the atc.* module reached a live service.
const char* kAtcProbe = "scrambled = false\n"
                        "function compute_control(state, tick, dt)\n"
                        "  if tick == 0 then scrambled = atc.scramble('builtin:airfield', 'test:basic', 1) end\n"
                        "  return {}\n"
                        "end";

} // namespace

TEST_CASE("buildAiController: a Lua script reaches the ATC service the caller passes (#1236)") {
    NullLogger log;
    EntityTypeRegistry reg;
    EntityDef d;
    d.id = "test:basic";
    d.name = "B";
    d.category = ObjectCategory::AirVehicle;
    d.maxHp = 100.f;
    reg.registerType(d);
    EntityManager em(log, reg);

    AirportRegistry airports;
    airports.load({builtinAirfield()}, kEarthRadiusM, nullptr);
    atc::AtcService service(em, airports, kEarthRadiusM);
    int spawns = 0;
    service.setSpawnHandler([&](const atc::AtcService::DepartureSpawn&) { ++spawns; });

    AiControllerRequest req;
    req.luaSource = kAtcProbe;
    req.entityManager = &em;
    req.atcService = &service; // the admin `spawn` path's shape

    auto built = buildAiController(req);
    REQUIRE(built.controller != nullptr);
    CHECK(built.error == AiBuildError::None);
    built.controller->sample(makeState(), 0, 1.0 / 60.0);
    CHECK(spawns == 1);
}

TEST_CASE("buildAiController: with no ATC service the same script runs, atc.* being a safe no-op (#1236)") {
    // The [atc] enabled = false shape — since #1288 that is the only way either caller passes null.
    // The script must still LOAD and RUN: atc.* degrading to no-ops is the documented behaviour
    // (#705), and a script that failed to construct here would take the mission's aircraft down
    // with it.
    NullLogger log;
    EntityTypeRegistry reg;
    EntityDef d;
    d.id = "test:basic";
    d.name = "B";
    d.category = ObjectCategory::AirVehicle;
    d.maxHp = 100.f;
    reg.registerType(d);
    EntityManager em(log, reg);

    AiControllerRequest req;
    req.luaSource = kAtcProbe;
    req.entityManager = &em;
    req.atcService = nullptr;

    auto built = buildAiController(req);
    REQUIRE(built.controller != nullptr);
    CHECK(built.error == AiBuildError::None);
    CHECK_NOTHROW(built.controller->sample(makeState(), 0, 1.0 / 60.0));
}

TEST_CASE("buildAiController: a broken Lua script is a classified error carrying the parser's reason (#1236)") {
    NullLogger log;
    EntityTypeRegistry reg;
    EntityManager em(log, reg);

    AiControllerRequest req;
    req.luaSource = "this is not lua ((("; // syntax error
    req.entityManager = &em;

    auto built = buildAiController(req);
    CHECK(built.controller == nullptr);
    CHECK(built.error == AiBuildError::LuaScriptError);
    // The callers format their own operator-facing message around this detail, so it must be real.
    CHECK_FALSE(built.detail.empty());
}

TEST_CASE("buildAiController: the factory path reports an unknown behavior without a controller (#1236)") {
    NullLogger log;
    EntityTypeRegistry reg;
    EntityManager em(log, reg);

    AiControllerRequest req;
    req.behavior = "not-a-real-behavior";
    req.entityManager = &em;

    auto built = buildAiController(req);
    CHECK(built.controller == nullptr);
    CHECK(built.error == AiBuildError::UnknownBehavior);
}

TEST_CASE("buildAiController: a known behavior builds, and an empty request is neither (#1236)") {
    NullLogger log;
    EntityTypeRegistry reg;
    EntityManager em(log, reg);

    AiControllerRequest known;
    known.behavior = "loiter";
    known.entityManager = &em;
    auto builtKnown = buildAiController(known);
    CHECK(builtKnown.controller != nullptr);
    CHECK(builtKnown.error == AiBuildError::None);

    // "no ai requested" must not read as an error — the mission path calls this for every object.
    AiControllerRequest none;
    none.entityManager = &em;
    auto builtNone = buildAiController(none);
    CHECK(builtNone.controller == nullptr);
    CHECK(builtNone.error == AiBuildError::None);

    // The bare "lua" selector with no resolved source is the caller's not-found case, not ours.
    AiControllerRequest bareLua;
    bareLua.behavior = "lua";
    bareLua.entityManager = &em;
    auto builtBare = buildAiController(bareLua);
    CHECK(builtBare.controller == nullptr);
    CHECK(builtBare.error == AiBuildError::None);
}
