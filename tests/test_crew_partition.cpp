// SPDX-License-Identifier: GPL-3.0-or-later
//
// validateCrewPartition: the one-owner-per-channel invariant (#966, #1145).
//
// This function is the entire reason a multi-crew aircraft can merge two seats' control inputs by
// mask without arbitration. Every rule it enforces exists so that no control channel has two owners:
// one Fly seat, at most one Radar seat, at most one Countermeasures seat, and every hardpoint slot
// fired by exactly one seat.
//
// Nothing downstream re-checks any of it. The parser throws on a non-empty return and validate-entity
// surfaces the same string with a file name — so a rule that is not enforced here is a rule that does
// not exist, and the failure it lets through is two seats sending conflicting commands to the same
// station in flight.

#include <catch2/catch_test_macros.hpp>

#include "entity/CrewDef.h"

#include <string>
#include <vector>

using namespace fl;

namespace {

CrewCapabilityMask caps(std::initializer_list<CrewCapability> cs) {
    CrewCapabilityMask m = 0;
    for (CrewCapability c : cs)
        m = withCapability(m, c);
    return m;
}

SeatDef seat(std::string role, std::initializer_list<CrewCapability> cs, std::vector<int> stations = {},
             std::string turret = {}) {
    SeatDef s;
    s.role = std::move(role);
    s.capabilities = caps(cs);
    s.stations = std::move(stations);
    s.turret = std::move(turret);
    return s;
}

TurretDef turret(std::string id, std::vector<int> stations) {
    TurretDef t;
    t.id = std::move(id);
    t.stations = std::move(stations);
    return t;
}

const std::vector<int> kSlots{1, 2, 3, 4};

} // namespace

// ---------------------------------------------------------------------------
// The shapes that are valid
// ---------------------------------------------------------------------------

TEST_CASE("validateCrewPartition: no crew is an implicit single pilot (#1145)", "[entity][crew]") {
    // The #675 fallback. An entity with no authored crew must stay exactly as it was before crew
    // seats existed, so an empty list is valid rather than "zero Fly seats".
    CHECK(validateCrewPartition({}, {}, kSlots).empty());
    CHECK(validateCrewPartition({}, {turret("dorsal", {1})}, kSlots).empty());
}

TEST_CASE("validateCrewPartition: a two-seat fighter partitions cleanly (#1145)", "[entity][crew]") {
    const std::vector<SeatDef> crew{
        seat("pilot", {CrewCapability::Fly, CrewCapability::Fire, CrewCapability::Countermeasures}, {1, 2}),
        seat("wso", {CrewCapability::Radar, CrewCapability::Fire}, {3, 4}),
    };
    CHECK(validateCrewPartition(crew, {}, kSlots).empty());
}

TEST_CASE("validateCrewPartition: a gunner fires through a turret it aims (#1145)", "[entity][crew]") {
    // The seat binds no stations directly — its Fire responsibility comes entirely from the turret's
    // mounted stations, which is what makes a bomber's gunner legal.
    const std::vector<TurretDef> turrets{turret("dorsal", {3, 4})};
    const std::vector<SeatDef> crew{
        seat("pilot", {CrewCapability::Fly}),
        seat("gunner", {CrewCapability::Fire}, {}, "dorsal"),
    };
    CHECK(validateCrewPartition(crew, turrets, kSlots).empty());
}

// ---------------------------------------------------------------------------
// One owner per control channel
// ---------------------------------------------------------------------------

TEST_CASE("validateCrewPartition: there is exactly one Fly seat (#1145)", "[entity][crew]") {
    // Two seats owning the flight controls is the failure the whole invariant exists to prevent:
    // both would send elevator commands and the merge would have to arbitrate mid-flight.
    const std::vector<SeatDef> twoPilots{
        seat("pilot", {CrewCapability::Fly}),
        seat("copilot", {CrewCapability::Fly}),
    };
    const std::string two = validateCrewPartition(twoPilots, {}, kSlots);
    CHECK_FALSE(two.empty());
    CHECK(two.find("exactly one Fly seat") != std::string::npos);
    CHECK(two.find("found 2") != std::string::npos); // the message says how many, not just "wrong"

    // And an aircraft nobody flies is equally invalid.
    const std::vector<SeatDef> noPilot{seat("wso", {CrewCapability::Radar})};
    CHECK(validateCrewPartition(noPilot, {}, kSlots).find("found 0") != std::string::npos);
}

TEST_CASE("validateCrewPartition: radar, countermeasures and command are single-owner (#1145)", "[entity][crew]") {
    const std::vector<int> slots{1};
    struct Case {
        CrewCapability cap;
        const char* needle;
    };
    for (const Case& c : {Case{CrewCapability::Radar, "Radar capability is on more than one seat"},
                          Case{CrewCapability::Countermeasures, "Countermeasures capability is on more than one seat"},
                          Case{CrewCapability::Command, "Command capability is on more than one seat"}}) {
        INFO(c.needle);
        const std::vector<SeatDef> crew{
            seat("pilot", {CrewCapability::Fly, c.cap}),
            seat("second", {c.cap}),
        };
        CHECK(validateCrewPartition(crew, {}, slots).find(c.needle) != std::string::npos);
    }
}

TEST_CASE("validateCrewPartition: a hardpoint slot is fired by exactly one seat (#1145)", "[entity][crew]") {
    // Two seats on one station means two firing commands for one pylon.
    const std::vector<SeatDef> crew{
        seat("pilot", {CrewCapability::Fly, CrewCapability::Fire}, {1, 2}),
        seat("wso", {CrewCapability::Fire}, {2, 3}), // slot 2 is claimed twice
    };
    const std::string err = validateCrewPartition(crew, {}, kSlots);
    CHECK(err.find("slot 2 is fired by more than one seat") != std::string::npos);
    CHECK(err.find("seats 0 and 1") != std::string::npos); // both are named, so the fix is obvious
}

TEST_CASE("validateCrewPartition: a turret's stations count as fired by its gunner (#1145)", "[entity][crew]") {
    // The collision is INDIRECT: the pilot binds slot 3 directly and the gunner reaches it through
    // the turret. Only expanding turret stations catches it.
    const std::vector<TurretDef> turrets{turret("dorsal", {3})};
    const std::vector<SeatDef> crew{
        seat("pilot", {CrewCapability::Fly, CrewCapability::Fire}, {3}),
        seat("gunner", {CrewCapability::Fire}, {}, "dorsal"),
    };
    CHECK(validateCrewPartition(crew, turrets, kSlots).find("more than one seat") != std::string::npos);
}

TEST_CASE("validateCrewPartition: a turret is aimed by exactly one seat (#1145)", "[entity][crew]") {
    const std::vector<TurretDef> turrets{turret("dorsal", {3})};
    const std::vector<SeatDef> crew{
        seat("pilot", {CrewCapability::Fly}),
        seat("gunner", {CrewCapability::Fire}, {}, "dorsal"),
        seat("spare", {CrewCapability::Fire}, {}, "dorsal"),
    };
    const std::string err = validateCrewPartition(crew, turrets, kSlots);
    CHECK(err.find("aimed by more than one seat") != std::string::npos);
    CHECK(err.find("seats 1 and 2") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Seats that do not make sense
// ---------------------------------------------------------------------------

TEST_CASE("validateCrewPartition: a seat needs a role and at least one capability (#1145)", "[entity][crew]") {
    // A capability-less seat owns no channel, so it is a seat that cannot do anything — almost
    // certainly a half-written definition rather than an intention.
    std::vector<SeatDef> crew{seat("", {CrewCapability::Fly})};
    CHECK(validateCrewPartition(crew, {}, kSlots).find("empty role") != std::string::npos);

    crew = {seat("pilot", {CrewCapability::Fly}), seat("passenger", {})};
    CHECK(validateCrewPartition(crew, {}, kSlots).find("declares no capabilities") != std::string::npos);
}

TEST_CASE("validateCrewPartition: binding a station requires the Fire capability (#1145)", "[entity][crew]") {
    // Otherwise the seat owns a station it has no channel to fire, and the pylon answers to nobody.
    std::vector<SeatDef> crew{
        seat("pilot", {CrewCapability::Fly}),
        seat("wso", {CrewCapability::Radar}, {1}),
    };
    CHECK(validateCrewPartition(crew, {}, kSlots).find("lacks the Fire capability") != std::string::npos);

    // A turret binding counts the same way.
    const std::vector<TurretDef> turrets{turret("dorsal", {3})};
    crew = {seat("pilot", {CrewCapability::Fly}), seat("observer", {CrewCapability::Radar}, {}, "dorsal")};
    CHECK(validateCrewPartition(crew, turrets, kSlots).find("lacks the Fire capability") != std::string::npos);
}

TEST_CASE("validateCrewPartition: a Fire seat that fires nothing is rejected (#1145)", "[entity][crew]") {
    // The mirror of the rule above. A Fire seat with no stations is a gunner with no gun — the
    // author meant to bind something and did not.
    const std::vector<SeatDef> crew{
        seat("pilot", {CrewCapability::Fly}),
        seat("gunner", {CrewCapability::Fire}),
    };
    CHECK(validateCrewPartition(crew, {}, kSlots).find("fires no station") != std::string::npos);
}

TEST_CASE("validateCrewPartition: a seat cannot fire or aim something that does not exist (#1145)", "[entity][crew]") {
    std::vector<SeatDef> crew{
        seat("pilot", {CrewCapability::Fly, CrewCapability::Fire}, {99}), // no such hardpoint
    };
    CHECK(validateCrewPartition(crew, {}, kSlots).find("fires unknown hardpoint slot 99") != std::string::npos);

    crew = {seat("pilot", {CrewCapability::Fly}), seat("gunner", {CrewCapability::Fire}, {}, "ventral")};
    CHECK(validateCrewPartition(crew, {turret("dorsal", {3})}, kSlots).find("unknown turret") != std::string::npos);
}

TEST_CASE("validateCrewPartition: the default skill is a fraction (#1145)", "[entity][crew]") {
    // Skill scales AI behaviour multiplicatively. A value outside [0, 1] would not read as "very
    // good" — it would push the derived parameters past the ranges they were tuned over.
    SeatDef pilot = seat("pilot", {CrewCapability::Fly});
    pilot.defaultSkill = 1.5f;
    CHECK(validateCrewPartition({pilot}, {}, kSlots).find("default skill must be in [0, 1]") != std::string::npos);

    pilot.defaultSkill = -0.1f;
    CHECK(validateCrewPartition({pilot}, {}, kSlots).find("default skill must be in [0, 1]") != std::string::npos);

    pilot.defaultSkill = 0.f; // the endpoints are legal
    CHECK(validateCrewPartition({pilot}, {}, kSlots).empty());
    pilot.defaultSkill = 1.f;
    CHECK(validateCrewPartition({pilot}, {}, kSlots).empty());
}

// ---------------------------------------------------------------------------
// Turret definitions
// ---------------------------------------------------------------------------

TEST_CASE("validateCrewPartition: turret ids are present and unique (#1145)", "[entity][crew]") {
    // The id is how a seat references the turret, so a duplicate makes "which one?" unanswerable.
    const std::vector<SeatDef> crew{seat("pilot", {CrewCapability::Fly})};

    CHECK(validateCrewPartition(crew, {turret("", {1})}, kSlots).find("empty id") != std::string::npos);
    CHECK(validateCrewPartition(crew, {turret("dorsal", {1}), turret("dorsal", {2})}, kSlots)
              .find("duplicate turret id") != std::string::npos);
}

TEST_CASE("validateCrewPartition: turret limits must be orderable and the servo must move (#1145)", "[entity][crew]") {
    // Inverted limits describe an empty arc, and a zero slew rate is a servo that never reaches its
    // commanded angle — the runtime would hold the turret at rest forever with no diagnosis.
    const std::vector<SeatDef> crew{seat("pilot", {CrewCapability::Fly})};

    TurretDef t = turret("dorsal", {1});
    t.slewRateDegS = 0.f;
    CHECK(validateCrewPartition(crew, {t}, kSlots).find("slew_rate_deg_s must be > 0") != std::string::npos);

    t = turret("dorsal", {1});
    t.azMinDeg = 90.f;
    t.azMaxDeg = -90.f;
    CHECK(validateCrewPartition(crew, {t}, kSlots).find("az_min_deg must be <= az_max_deg") != std::string::npos);

    t = turret("dorsal", {1});
    t.elMinDeg = 60.f;
    t.elMaxDeg = 10.f;
    CHECK(validateCrewPartition(crew, {t}, kSlots).find("el_min_deg must be <= el_max_deg") != std::string::npos);
}

TEST_CASE("validateCrewPartition: a turret cannot mount a hardpoint that does not exist (#1145)", "[entity][crew]") {
    const std::vector<SeatDef> crew{seat("pilot", {CrewCapability::Fly})};
    CHECK(validateCrewPartition(crew, {turret("dorsal", {99})}, kSlots).find("mounts unknown hardpoint slot 99") !=
          std::string::npos);
}

// ---------------------------------------------------------------------------
// The capability vocabulary
// ---------------------------------------------------------------------------

TEST_CASE("crew capabilities round-trip through their authored tokens (#1145)", "[entity][crew]") {
    // These strings are what a mod author types. A name and its parser drifting apart would make a
    // valid definition unloadable, so they are checked against each other rather than by hand.
    for (CrewCapability c : allCrewCapabilities()) {
        const std::string_view name = crewCapabilityName(c);
        INFO(name);
        CHECK_FALSE(name.empty());
        REQUIRE(parseCrewCapability(name).has_value());
        CHECK(*parseCrewCapability(name) == c);
    }
    CHECK(allCrewCapabilities().size() == 5u); // None is not an authorable capability

    CHECK(crewCapabilityName(CrewCapability::None) == "none");
    CHECK_FALSE(parseCrewCapability("none").has_value()); // ...and "none" is not authorable either
    CHECK_FALSE(parseCrewCapability("navigator").has_value());
    CHECK_FALSE(parseCrewCapability("").has_value());
    CHECK_FALSE(parseCrewCapability("FLY").has_value()); // tokens are lowercase
}
