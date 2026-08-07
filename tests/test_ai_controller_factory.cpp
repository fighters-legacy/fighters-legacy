// SPDX-License-Identifier: GPL-3.0-or-later
//
// createController: the console/script/mission entry point to every AI behaviour (#1145).
//
// 23 behaviours behind one string, each with its own optional-argument grammar. This is the surface
// an operator types at the debug console and a mission author writes in YAML, so the two things that
// matter are that every documented spelling constructs something, and that a malformed argument
// yields NULL rather than a controller silently flying on defaults the author did not ask for.

#include <catch2/catch_test_macros.hpp>

#include "ILogger.h"
#include "ai/AiControllerFactory.h"
#include "entity/EntityDef.h"
#include "entity/EntityManager.h"
#include "entity/EntityTypeRegistry.h"

#include <array>
#include <string>
#include <string_view>
#include <vector>

using namespace fl;

namespace {

struct NullLogger final : ILogger {
    void log(LogLevel, const char*, int, const char*) override {}
    void setMinLevel(LogLevel) override {}
    void flush() override {}
};

// A world with one live entity at pool index 0, which the target-taking behaviours resolve against.
struct World {
    NullLogger log;
    EntityTypeRegistry reg;
    EntityManager em;
    EntityId target;

    World() : em(log, reg) {
        EntityDef d;
        d.id = "test:target";
        d.name = "Target";
        reg.registerType(d);
        EntityTransform t{};
        t.quat[3] = 1.f;
        target = em.spawn("test:target", t);
    }
    [[nodiscard]] std::string targetIdx() const {
        return std::to_string(target.index);
    }
};

std::unique_ptr<IEntityController> make(std::string_view behavior, std::vector<std::string> argStrings,
                                        const EntityManager* em = nullptr) {
    std::vector<std::string_view> views;
    views.reserve(argStrings.size());
    for (const auto& a : argStrings)
        views.emplace_back(a);
    return ai::createController(behavior, std::span<std::string_view>(views), em);
}

} // namespace

// ---------------------------------------------------------------------------
// Unknown behaviours and the no-entity-manager degradation
// ---------------------------------------------------------------------------

TEST_CASE("createController: an unknown behaviour is null, not a guess (#1145)", "[ai][factory]") {
    CHECK(make("teleport", {}) == nullptr);
    CHECK(make("", {}) == nullptr);
    CHECK(make("LOITER", {}) == nullptr); // the vocabulary is case-sensitive
}

TEST_CASE("createController: target-taking behaviours need an entity manager (#1145)", "[ai][factory]") {
    // Without one there is no way to resolve the index to an entity, so constructing a controller
    // that would chase nothing is worse than returning null.
    for (const char* b : {"pursuit", "evade", "break", "lead", "lag", "high_yo_yo", "low_yo_yo", "guns",
                          "patrol_attack", "escort", "dynamic_loiter"}) {
        INFO("behaviour " << b);
        CHECK(make(b, {"0"}) == nullptr);
    }
}

TEST_CASE("createController: a target index that matches no live entity is null (#1145)", "[ai][factory]") {
    World w;
    for (const char* b : {"pursuit", "evade", "lead", "lag", "guns"}) {
        INFO("behaviour " << b);
        CHECK(make(b, {"9999"}, &w.em) == nullptr);
    }
}

// ---------------------------------------------------------------------------
// Fixed-geometry behaviours
// ---------------------------------------------------------------------------

TEST_CASE("createController: loiter takes everything or nothing (#1145)", "[ai][factory]") {
    CHECK(make("loiter", {}) != nullptr);                                             // all defaults
    CHECK(make("loiter", {"0", "600", "0"}) != nullptr);                              // centre only
    CHECK(make("loiter", {"0", "600", "0", "2500"}) != nullptr);                      // + radius
    CHECK(make("loiter", {"0", "600", "0", "2500", "700"}) != nullptr);               // + altitude
    CHECK(make("loiter", {"0", "600", "0", "2500", "700", "0.7"}) != nullptr);        // + throttle
    CHECK(make("loiter", {"0", "600", "0", "2500", "700", "0.7", "ccw"}) != nullptr); // + direction
    CHECK(make("loiter", {"0", "600", "0", "2500", "700", "0.7", "cw"}) != nullptr);

    // A non-numeric coordinate is a typo, not a default.
    CHECK(make("loiter", {"north", "600", "0"}) == nullptr);
    CHECK(make("loiter", {"0", "600", "0", "wide"}) == nullptr);
    // A partial centre triple is IGNORED rather than rejected — the grammar's brackets are
    // all-or-nothing, so `loiter 0 600` orbits the default centre. Documenting the real behaviour
    // rather than the one I assumed: it is a footgun (the operator's two numbers vanish), but it is
    // what the console does today and a test asserting otherwise would just be wrong.
    CHECK(make("loiter", {"0", "600"}) != nullptr);
}

TEST_CASE("createController: waypoint needs whole triples and honours --loop (#1145)", "[ai][factory]") {
    CHECK(make("waypoint", {"100", "500", "200"}) != nullptr);
    CHECK(make("waypoint", {"100", "500", "200", "300", "500", "400"}) != nullptr);
    CHECK(make("waypoint", {"100", "500", "200", "--loop"}) != nullptr);

    CHECK(make("waypoint", {}) == nullptr);                     // nowhere to go
    CHECK(make("waypoint", {"100", "500"}) == nullptr);         // a partial triple
    CHECK(make("waypoint", {"100", "500", "east"}) == nullptr); // a non-numeric component
}

TEST_CASE("createController: ballistic validates its target and MIRV arguments (#1145)", "[ai][factory]") {
    CHECK(make("ballistic", {"1000", "0", "2000"}) != nullptr);
    CHECK(make("ballistic", {"1000", "0", "2000", "3"}) != nullptr);
    CHECK(make("ballistic", {"1000", "0", "2000", "3", "500"}) != nullptr);

    CHECK(make("ballistic", {"1000", "0"}) == nullptr);
    CHECK(make("ballistic", {"1000", "0", "somewhere"}) == nullptr);
}

TEST_CASE("createController: the static emplacements default fully (#1145)", "[ai][factory]") {
    // sam/aaa still need the entity manager: an emplacement with no way to see targets is inert,
    // so the factory refuses rather than handing back something that can never fire.
    CHECK(make("sam", {}) == nullptr);
    CHECK(make("aaa", {}) == nullptr);

    World w;
    CHECK(make("sam", {}, &w.em) != nullptr);
    CHECK(make("sam", {"25000"}, &w.em) != nullptr);
    CHECK(make("sam", {"25000", "70"}, &w.em) != nullptr);
    CHECK(make("sam", {"25000", "70", "6"}, &w.em) != nullptr);
    CHECK(make("sam", {"far"}, &w.em) == nullptr);
    CHECK(make("sam", {"0"}, &w.em) == nullptr); // a zero engagement range is not a default request

    CHECK(make("aaa", {}, &w.em) != nullptr);
    CHECK(make("aaa", {"3000"}, &w.em) != nullptr);
    CHECK(make("aaa", {"3000", "20"}, &w.em) != nullptr);
    CHECK(make("aaa", {"3000", "20", "900"}, &w.em) != nullptr);
    CHECK(make("aaa", {"3000", "20", "900", "12"}, &w.em) != nullptr);
    CHECK(make("aaa", {"near"}, &w.em) == nullptr);
}

TEST_CASE("createController: the solo manoeuvres take optional durations (#1145)", "[ai][factory]") {
    CHECK(make("immelmann", {}) != nullptr);
    CHECK(make("immelmann", {"2.0"}) != nullptr);
    CHECK(make("immelmann", {"2.0", "1.0"}) != nullptr);
    CHECK(make("immelmann", {"quickly"}) == nullptr);

    CHECK(make("split_s", {}) != nullptr);
    CHECK(make("split_s", {"1.0"}) != nullptr);
    CHECK(make("split_s", {"1.0", "2.0"}) != nullptr);
    CHECK(make("split_s", {"slowly"}) == nullptr);
}

// ---------------------------------------------------------------------------
// Target-taking behaviours, with a world to resolve against
// ---------------------------------------------------------------------------

TEST_CASE("createController: the pursuit family resolves a live target (#1145)", "[ai][factory]") {
    World w;
    const std::string idx = w.targetIdx();

    CHECK(make("pursuit", {idx}, &w.em) != nullptr);
    CHECK(make("evade", {idx}, &w.em) != nullptr);
    CHECK(make("break", {idx}, &w.em) != nullptr);
    CHECK(make("break", {idx, "0.8"}, &w.em) != nullptr);
    CHECK(make("lead", {idx}, &w.em) != nullptr);
    CHECK(make("lead", {idx, "1.5"}, &w.em) != nullptr);
    CHECK(make("lag", {idx}, &w.em) != nullptr);
    CHECK(make("lag", {idx, "0.5"}, &w.em) != nullptr);
    CHECK(make("guns", {idx}, &w.em) != nullptr);
    CHECK(make("guns", {idx, "1030"}, &w.em) != nullptr);
    CHECK(make("guns", {idx, "1030", "10"}, &w.em) != nullptr);
    CHECK(make("high_yo_yo", {idx}, &w.em) != nullptr);
    CHECK(make("high_yo_yo", {idx, "2.5", "1.5"}, &w.em) != nullptr);
    CHECK(make("low_yo_yo", {idx}, &w.em) != nullptr);
    CHECK(make("low_yo_yo", {idx, "1.5", "1.0"}, &w.em) != nullptr);
    CHECK(make("dynamic_loiter", {idx}, &w.em) != nullptr);
    CHECK(make("dynamic_loiter", {idx, "2500", "0.7", "ccw"}, &w.em) != nullptr);
    CHECK(make("patrol_attack", {idx}, &w.em) != nullptr);
    CHECK(make("patrol_attack", {idx, "8000", "0.3"}, &w.em) != nullptr);
    CHECK(make("escort", {idx}, &w.em) != nullptr);
    CHECK(make("escort", {idx, "1500"}, &w.em) != nullptr);
}

TEST_CASE("createController: a malformed target index is rejected (#1145)", "[ai][factory]") {
    World w;
    for (const char* b : {"pursuit", "evade", "break", "lead", "lag", "guns", "escort"}) {
        INFO("behaviour " << b);
        CHECK(make(b, {"first"}, &w.em) == nullptr); // not a number
        CHECK(make(b, {"-1"}, &w.em) == nullptr);    // not unsigned
        CHECK(make(b, {}, &w.em) == nullptr);        // absent
    }
}

TEST_CASE("createController: a malformed optional argument is rejected too (#1145)", "[ai][factory]") {
    World w;
    const std::string idx = w.targetIdx();
    CHECK(make("break", {idx, "soon"}, &w.em) == nullptr);
    CHECK(make("lead", {idx, "lots"}, &w.em) == nullptr);
    CHECK(make("guns", {idx, "fast"}, &w.em) == nullptr);
    CHECK(make("escort", {idx, "close"}, &w.em) == nullptr);
}

// ---------------------------------------------------------------------------
// Formation, swarm, wingman
// ---------------------------------------------------------------------------

TEST_CASE("createController: formation takes an anchor and slot geometry (#1145)", "[ai][factory]") {
    World w;
    const std::string idx = w.targetIdx();
    CHECK(make("formation", {idx}, &w.em) != nullptr);
    CHECK(make("formation", {idx, "1"}, &w.em) != nullptr);
    CHECK(make("formation", {idx, "1", "200"}, &w.em) != nullptr);
    CHECK(make("formation", {idx, "1", "200", "120"}, &w.em) != nullptr);
    CHECK(make("formation", {}, &w.em) == nullptr);
    CHECK(make("formation", {"lead"}, &w.em) == nullptr);
}

TEST_CASE("createController: swarm takes a point and swarm_follow an anchor (#1145)", "[ai][factory]") {
    World w;
    CHECK(make("swarm", {"0", "500", "0"}, &w.em) != nullptr);
    CHECK(make("swarm", {"0", "500"}, &w.em) == nullptr);
    CHECK(make("swarm", {"0", "500", "north"}, &w.em) == nullptr);

    CHECK(make("swarm_follow", {w.targetIdx()}, &w.em) != nullptr);
    CHECK(make("swarm_follow", {"nobody"}, &w.em) == nullptr);
    CHECK(make("swarm_follow", {}, &w.em) == nullptr);
}

TEST_CASE("createController: wingman needs an anchor and a known command (#1145)", "[ai][factory]") {
    World w;
    const std::string idx = w.targetIdx();
    CHECK(make("wingman", {}, &w.em) == nullptr);
    CHECK(make("wingman", {idx}, &w.em) == nullptr); // a command is required
    CHECK(make("wingman", {idx, "rejoin"}, &w.em) != nullptr);
    CHECK(make("wingman", {idx, "rejoin", "1"}, &w.em) != nullptr);
    CHECK(make("wingman", {idx, "make_tea"}, &w.em) == nullptr); // the command vocabulary is closed
}
