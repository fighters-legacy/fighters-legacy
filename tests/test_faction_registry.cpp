// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "world/AirspaceZone.h"
#include "world/AlertLevel.h"
#include "world/FactionDef.h"
#include "world/FactionRegistry.h"

#include <cstdint>
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
