// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ejection / pilot survival model tests (#672): the pure survivability envelope and the
// territory -> outcome mapping that lets the campaign express KIA/MIA/rescued/captured.

#include "entity/Ejection.h"

#include <catch2/catch_test_macros.hpp>

using namespace fl;

TEST_CASE("ejectionSurvivable: a zero-zero seat saves a pilot on the deck in level flight (#672)", "[ejection]") {
    // On the runway, wings level, no sink: the seat works at zero altitude.
    CHECK(ejectionSurvivable(EjectionEnvelope{/*alt=*/0.f, /*speed=*/60.f, /*sink=*/0.f}));
}

TEST_CASE("ejectionSurvivable: diving into the ground below the chute-deploy altitude is fatal (#672)", "[ejection]") {
    // Descending at 50 m/s with only 20 m AGL: the chute cannot deploy in time (needs 50*1.5 = 75 m).
    CHECK_FALSE(ejectionSurvivable(EjectionEnvelope{/*alt=*/20.f, /*speed=*/200.f, /*sink=*/50.f}));
    // The same sink rate with ample altitude survives.
    CHECK(ejectionSurvivable(EjectionEnvelope{/*alt=*/2000.f, /*speed=*/200.f, /*sink=*/50.f}));
}

TEST_CASE("ejectionSurvivable: above the windblast speed limit is fatal regardless of altitude (#672)", "[ejection]") {
    CHECK_FALSE(ejectionSurvivable(EjectionEnvelope{/*alt=*/8000.f, /*speed=*/400.f, /*sink=*/0.f}));
    CHECK(ejectionSurvivable(EjectionEnvelope{/*alt=*/8000.f, /*speed=*/300.f, /*sink=*/0.f}));
}

TEST_CASE("pilotOutcome: a dead seat is KIA; territory decides a survivor's fate (#672)", "[ejection]") {
    CHECK(pilotOutcome(false, TerritoryControl::Friendly) == EjectionOutcome::KIA); // seat failed -> KIA
    CHECK(pilotOutcome(true, TerritoryControl::Friendly) == EjectionOutcome::Rescued);
    CHECK(pilotOutcome(true, TerritoryControl::Hostile) == EjectionOutcome::Captured);
    CHECK(pilotOutcome(true, TerritoryControl::Neutral) == EjectionOutcome::MIA);
}

TEST_CASE("pilotSurvived: KIA is the only career loss (#672)", "[ejection]") {
    CHECK_FALSE(pilotSurvived(EjectionOutcome::KIA));
    CHECK(pilotSurvived(EjectionOutcome::Rescued));
    CHECK(pilotSurvived(EjectionOutcome::MIA));
    CHECK(pilotSurvived(EjectionOutcome::Captured));
}
