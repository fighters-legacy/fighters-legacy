// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "util/SimThreadOwnership.h"
#include "world/AirspaceZone.h"
#include "world/AlertLevel.h"
#include "world/FactionDef.h"
#include "world/FactionRegistry.h"

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

using namespace fl;

namespace {

// FactionRegistry is non-copyable/non-movable (it holds a std::mutex), so tests
// construct it in place and load this shared set of defs.
std::vector<FactionDef> threeFactionDefs() {
    return {
        FactionDef{"usa", "United States", AlertLevel::Peacetime},
        FactionDef{"russia", "Russia", AlertLevel::Elevated},
        FactionDef{"china", "China", AlertLevel::Conflict},
    };
}

} // namespace

// ---------------------------------------------------------------------------
// load / count
// ---------------------------------------------------------------------------

TEST_CASE("FactionRegistry: empty registry is safe", "[faction_registry]") {
    FactionRegistry reg;
    CHECK(reg.count() == 0u);
    CHECK(reg.indexOf("usa") == UINT16_MAX);
    CHECK(reg.get(0) == nullptr);
    CHECK(reg.relationship(0, 1) == FactionRelation::Neutral);
    CHECK(reg.alertLevel(0) == AlertLevel::Peacetime);
}

TEST_CASE("FactionRegistry: load populates count and index", "[faction_registry]") {
    FactionRegistry reg;
    reg.load(threeFactionDefs());
    CHECK(reg.count() == 3u);
    CHECK(reg.indexOf("usa") == 0u);
    CHECK(reg.indexOf("russia") == 1u);
    CHECK(reg.indexOf("china") == 2u);
}

TEST_CASE("FactionRegistry: indexOf returns sentinel on miss", "[faction_registry]") {
    FactionRegistry reg;
    reg.load(threeFactionDefs());
    CHECK(reg.indexOf("atlantis") == UINT16_MAX);
}

TEST_CASE("FactionRegistry: re-load replaces all state", "[faction_registry]") {
    FactionRegistry reg;
    reg.load(threeFactionDefs());
    reg.setRelationship(0, 1, FactionRelation::Hostile);
    reg.setAlertLevel(0, AlertLevel::WarState);

    reg.load({FactionDef{"neutralia", "Neutralia", AlertLevel::Elevated}});

    CHECK(reg.count() == 1u);
    CHECK(reg.indexOf("usa") == UINT16_MAX);          // old ids gone
    CHECK(reg.indexOf("neutralia") == 0u);            // new id present
    CHECK(reg.alertLevel(0) == AlertLevel::Elevated); // reseeded from startingAlertLevel
    CHECK(reg.get(1) == nullptr);                     // matrix/defs resized down
}

// ---------------------------------------------------------------------------
// get
// ---------------------------------------------------------------------------

TEST_CASE("FactionRegistry: get returns def in range", "[faction_registry]") {
    FactionRegistry reg;
    reg.load(threeFactionDefs());
    const FactionDef* def = reg.get(1);
    REQUIRE(def != nullptr);
    CHECK(def->id == "russia");
    CHECK(def->name == "Russia");
    CHECK(def->startingAlertLevel == AlertLevel::Elevated);
}

TEST_CASE("FactionRegistry: get out of range returns nullptr", "[faction_registry]") {
    FactionRegistry reg;
    reg.load(threeFactionDefs());
    CHECK(reg.get(3) == nullptr);
    CHECK(reg.get(UINT16_MAX) == nullptr);
}

// ---------------------------------------------------------------------------
// alert levels
// ---------------------------------------------------------------------------

TEST_CASE("FactionRegistry: alert levels seeded from defs", "[faction_registry]") {
    FactionRegistry reg;
    reg.load(threeFactionDefs());
    CHECK(reg.alertLevel(0) == AlertLevel::Peacetime);
    CHECK(reg.alertLevel(1) == AlertLevel::Elevated);
    CHECK(reg.alertLevel(2) == AlertLevel::Conflict);
}

TEST_CASE("FactionRegistry: setAlertLevel round-trips", "[faction_registry]") {
    FactionRegistry reg;
    reg.load(threeFactionDefs());
    reg.setAlertLevel(0, AlertLevel::WarState);
    CHECK(reg.alertLevel(0) == AlertLevel::WarState);
}

TEST_CASE("FactionRegistry: alert level out-of-range is guarded", "[faction_registry]") {
    FactionRegistry reg;
    reg.load(threeFactionDefs());
    CHECK(reg.alertLevel(99) == AlertLevel::Peacetime); // OOB read -> default
    reg.setAlertLevel(99, AlertLevel::WarState);        // OOB write -> no-op
    CHECK(reg.alertLevel(99) == AlertLevel::Peacetime);
    CHECK(reg.alertLevel(0) == AlertLevel::Peacetime); // unrelated entry untouched
}

// ---------------------------------------------------------------------------
// relationships
// ---------------------------------------------------------------------------

TEST_CASE("FactionRegistry: default relationship matrix", "[faction_registry]") {
    FactionRegistry reg;
    reg.load(threeFactionDefs());
    CHECK(reg.relationship(0, 0) == FactionRelation::Friendly); // diagonal = self
    CHECK(reg.relationship(1, 1) == FactionRelation::Friendly);
    CHECK(reg.relationship(0, 1) == FactionRelation::Neutral); // off-diagonal default
    CHECK(reg.relationship(2, 0) == FactionRelation::Neutral);
}

TEST_CASE("FactionRegistry: setRelationship is symmetric", "[faction_registry]") {
    FactionRegistry reg;
    reg.load(threeFactionDefs());
    reg.setRelationship(0, 2, FactionRelation::Hostile);
    CHECK(reg.relationship(0, 2) == FactionRelation::Hostile);
    CHECK(reg.relationship(2, 0) == FactionRelation::Hostile); // reflected
}

TEST_CASE("FactionRegistry: relationship out-of-range is guarded", "[faction_registry]") {
    FactionRegistry reg;
    reg.load(threeFactionDefs());
    CHECK(reg.relationship(0, 99) == FactionRelation::Neutral); // OOB read
    CHECK(reg.relationship(99, 0) == FactionRelation::Neutral);
    reg.setRelationship(0, 99, FactionRelation::Hostile);      // OOB write -> no-op
    CHECK(reg.relationship(0, 1) == FactionRelation::Neutral); // nothing corrupted
}

// ---------------------------------------------------------------------------
// areHostile — coalition-aware hostility (#632)
// ---------------------------------------------------------------------------

TEST_CASE("FactionRegistry: areHostile guards neutral index and self", "[faction_registry]") {
    FactionRegistry reg;
    reg.load(threeFactionDefs());
    reg.setRelationship(0, 1, FactionRelation::Hostile); // even an explicit hostile with index 0...
    CHECK_FALSE(reg.areHostile(0, 1));                   // ...index 0 is reserved neutral: no enemies
    CHECK_FALSE(reg.areHostile(1, 0));
    CHECK_FALSE(reg.areHostile(1, 1)); // never hostile to itself
}

TEST_CASE("FactionRegistry: areHostile reads the relationship matrix", "[faction_registry]") {
    FactionRegistry reg;
    reg.load(threeFactionDefs());
    CHECK_FALSE(reg.areHostile(1, 2)); // default off-diagonal is Neutral, not Hostile
    reg.setRelationship(1, 2, FactionRelation::Hostile);
    CHECK(reg.areHostile(1, 2));
    CHECK(reg.areHostile(2, 1)); // symmetric
    reg.setRelationship(1, 2, FactionRelation::Friendly);
    CHECK_FALSE(reg.areHostile(1, 2)); // allies are not hostile
}

TEST_CASE("hostile() helper: null registry falls back to affiliation rule", "[faction_registry]") {
    // Null = "not evaluated" (pre-mission): distinct non-zero factions are hostile, like areFactionsHostile.
    CHECK(hostile(nullptr, 1, 2));
    CHECK_FALSE(hostile(nullptr, 0, 1)); // faction 0 neutral
    CHECK_FALSE(hostile(nullptr, 2, 2)); // self

    // Non-null = coalition-aware: two sides are friendly until declared hostile.
    FactionRegistry reg;
    reg.load(threeFactionDefs());
    CHECK_FALSE(hostile(&reg, 1, 2)); // registry present, relationship Neutral -> not hostile
    reg.setRelationship(1, 2, FactionRelation::Hostile);
    CHECK(hostile(&reg, 1, 2));
}

// ---------------------------------------------------------------------------
// Enum / POD contract guards (lock the ordinals downstream wire/Lua code depends on)
// ---------------------------------------------------------------------------

TEST_CASE("World types: enum ordinals are stable", "[faction_registry]") {
    CHECK(static_cast<uint8_t>(AlertLevel::Peacetime) == 0u);
    CHECK(static_cast<uint8_t>(AlertLevel::Elevated) == 1u);
    CHECK(static_cast<uint8_t>(AlertLevel::Conflict) == 2u);
    CHECK(static_cast<uint8_t>(AlertLevel::WarState) == 3u);

    CHECK(static_cast<uint8_t>(EscalationStage::Clean) == 0u);
    CHECK(static_cast<uint8_t>(EscalationStage::InZone) == 1u);
    CHECK(static_cast<uint8_t>(EscalationStage::Warned) == 2u);
    CHECK(static_cast<uint8_t>(EscalationStage::Intercept) == 3u);
    CHECK(static_cast<uint8_t>(EscalationStage::Hostile) == 4u);

    CHECK(static_cast<uint8_t>(FactionRelation::Friendly) == 0u);
    CHECK(static_cast<uint8_t>(FactionRelation::Neutral) == 1u);
    CHECK(static_cast<uint8_t>(FactionRelation::Hostile) == 2u);

    CHECK(static_cast<uint8_t>(ZoneShape::Circle) == 0u);
    CHECK(static_cast<uint8_t>(ZoneShape::Polygon) == 1u);
}

TEST_CASE("AirspaceZone: POD defaults", "[faction_registry]") {
    AirspaceZone zone{};
    CHECK(zone.shape == ZoneShape::Circle);
    CHECK(zone.centerX == 0.0);
    CHECK(zone.centerZ == 0.0);
    CHECK(zone.radiusM == 0.0);
    CHECK(zone.vertices.empty());
    CHECK(zone.altFloorM == 0.0);
    CHECK(zone.altCeilingM == 999'999.0);
    CHECK(zone.ownerFactionId.empty());
    CHECK(zone.policyId.empty());
}

// ---------------------------------------------------------------------------
// Thread-ownership contract (#1094)
// ---------------------------------------------------------------------------
//
// The registry's three tiers are asserted in debug builds. An assert() cannot be caught, so what is
// tested here is the MECHANISM those assertions read -- SimThreadOwnership -- plus the fact that a
// real GameLoop publishes its sim thread through it. That is the part that can silently break: if
// claim() stopped being called, every sim-thread assertion in the engine would pass unconditionally
// and the guard would be a no-op nobody noticed. The tiers themselves are exercised by driving the
// registry from a claimed sim thread and from an unclaimed one, both of which must be legal.

namespace {

// Runs `fn` on a thread that has claimed sim-thread ownership, exactly as GameLoop's sim thread does.
template <typename Fn> void onSimThread(Fn&& fn) {
    std::thread t([&fn] {
        SimThreadOwnership::claim();
        fn();
        SimThreadOwnership::release();
    });
    t.join();
}

} // namespace

TEST_CASE("SimThreadOwnership: no sim thread means single-threaded, which every tier permits",
          "[faction_registry][threading]") {
    // The state a unit test, a validator and server init all run in.
    CHECK_FALSE(SimThreadOwnership::simThreadActive());
    CHECK_FALSE(SimThreadOwnership::onSimThread());
    CHECK(SimThreadOwnership::onSimThreadOrSingleThreaded());
}

TEST_CASE("SimThreadOwnership: only the claiming thread is the sim thread", "[faction_registry][threading]") {
    std::atomic<bool> claimedSeesItself{false};
    std::atomic<bool> otherSeesItself{true};
    std::atomic<bool> otherSeesActive{false};

    std::thread sim([&] {
        SimThreadOwnership::claim();
        claimedSeesItself = SimThreadOwnership::onSimThread();

        // A second thread, live at the same time: it must see that a sim thread exists and that it is
        // not the one. This is the case the assertions exist to catch -- an admin or network thread
        // touching a sim-thread-only member while the sim runs.
        std::thread other([&] {
            otherSeesActive = SimThreadOwnership::simThreadActive();
            otherSeesItself = SimThreadOwnership::onSimThread();
        });
        other.join();

        SimThreadOwnership::release();
    });
    sim.join();

    CHECK(claimedSeesItself);
    CHECK(otherSeesActive);
    CHECK_FALSE(otherSeesItself);
    // Released on the way out, so teardown reads as single-threaded again.
    CHECK_FALSE(SimThreadOwnership::simThreadActive());
}

TEST_CASE("FactionRegistry: the sim-thread tier is usable from the sim thread", "[faction_registry][threading]") {
    FactionRegistry reg;
    reg.load(threeFactionDefs()); // tier 1: before any sim thread exists

    onSimThread([&reg] {
        reg.setRelationship(1, 2, FactionRelation::Hostile);
        CHECK(reg.relationship(1, 2) == FactionRelation::Hostile);
        CHECK(reg.areHostile(1, 2));
    });

    // And still readable afterwards, when the process is single-threaded again.
    CHECK(reg.relationship(1, 2) == FactionRelation::Hostile);
}

TEST_CASE("FactionRegistry: the alert-level tier takes any thread, which is why it has a lock",
          "[faction_registry][threading]") {
    FactionRegistry reg;
    reg.load(threeFactionDefs());

    // A concurrent writer off the sim thread is LEGAL here and is the #162 production path: the
    // network thread raises a posture while the sim reads it. No assertion may fire on either side.
    std::atomic<bool> stop{false};
    std::thread sim([&] {
        SimThreadOwnership::claim();
        while (!stop.load(std::memory_order_relaxed))
            (void)reg.alertLevel(1);
        SimThreadOwnership::release();
    });

    for (int i = 0; i < 200; ++i)
        reg.setAlertLevel(1, (i % 2) ? AlertLevel::Conflict : AlertLevel::Elevated);

    stop.store(true, std::memory_order_relaxed);
    sim.join();

    const AlertLevel final = reg.alertLevel(1);
    CHECK((final == AlertLevel::Conflict || final == AlertLevel::Elevated));
}

// The finding that corrected the contract (#1094). The header used to call the relationship tier
// "sim-thread-only" outright, and the first debug fl-server run under an assertion written from that
// comment aborted on tick 2 -- from a JobSystem worker inside SensorSystem::evaluateObserver, because
// the parallel per-observer sensor pass reads relationships from every worker through the
// hostile(registry, a, b) seam. It is sound: JobSystem::dispatch is a BLOCKING parallel_for, the owner
// thread participates and waits, so no write is in flight while workers read.
//
// This test pins that: parallel reads from threads that are NOT the sim thread must be legal. Without
// it, someone "consistency-fixing" the read side back to match the write side would abort every debug
// server run, and would have a green test suite while doing it.
TEST_CASE("FactionRegistry: relationships are readable in parallel from non-sim threads",
          "[faction_registry][threading]") {
    FactionRegistry reg;
    reg.load(threeFactionDefs());
    onSimThread([&reg] { reg.setRelationship(1, 2, FactionRelation::Hostile); });

    // The shape of a JobSystem batch: the owner thread claims the sim tier and participates, workers
    // read alongside it, and the owner waits for all of them before returning.
    std::atomic<int> hostileSeen{0};
    std::thread owner([&] {
        SimThreadOwnership::claim();
        std::vector<std::thread> workers;
        for (int w = 0; w < 4; ++w)
            workers.emplace_back([&reg, &hostileSeen] {
                for (int i = 0; i < 500; ++i)
                    if (reg.areHostile(1, 2) && reg.relationship(1, 2) == FactionRelation::Hostile)
                        ++hostileSeen;
            });
        for (int i = 0; i < 500; ++i)
            if (reg.areHostile(1, 2))
                ++hostileSeen;
        for (auto& t : workers)
            t.join();
        SimThreadOwnership::release();
    });
    owner.join();

    CHECK(hostileSeen.load() == 2500); // 4 workers + the owner, every read agreeing
}
