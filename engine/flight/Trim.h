// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "flight/AeroForces.h"
#include "flight/FlightModelData.h"

namespace fl {

// One flight condition to trim the aircraft at.
struct TrimPoint {
    float altitude_m{0.f};
    float mass_kg{0.f}; // empty + fuel + payload; the caller decides the loading
};

// Derived performance at one TrimPoint.
//
// This is the aircraft's spec sheet, computed FROM the flight model rather than transcribed
// alongside it. #54's Phase-4 acceptance gate is "flight model stall speed + fuel burn match design
// spec", and until now there was no way to compute either: nothing in the tree derived performance
// from a FlightModelData, so the gate was unmeasurable and a content author's shortest feedback loop
// was "build the game and fly it".
struct TrimResult {
    float stall_speed_1g_mps{0.f};   // slowest speed at which CL_max still carries the weight
    float min_level_speed_mps{0.f};  // slowest speed the ENGINE can sustain: above the stall on the back
                                     // side of the power curve the wing still carries the weight, but the
                                     // engine cannot pay for the drag, so the aircraft sinks (#825)
    float max_level_mach{0.f};       // where thrust = drag in level flight
    float roc_mps_mil{0.f};          // best rate of climb, military power
    float roc_mps_ab{0.f};           // best rate of climb, afterburner (== MIL if the model has no AB)
    float sustained_turn_deg_s{0.f}; // fastest turn the engine can hold speed in
    float instant_turn_deg_s{0.f};   // fastest turn the wing can make, structure permitting
    float corner_speed_mps{0.f};     // where the two turn curves meet: the best turning speed
    float sustained_g{0.f};          // the load factor behind sustained_turn_deg_s
    float instant_g{0.f};            // the load factor behind instant_turn_deg_s
    float fuel_flow_mil_kg_s{0.f};
    float fuel_flow_ab_kg_s{0.f};
    float specific_range_m_per_kg{0.f}; // best cruise efficiency at this condition

    // FALSE MEANS "COULD NOT TRIM HERE", AND IT IS REPORTED, NOT GUESSED. An aircraft that cannot
    // hold level flight at 20 km has no max level Mach there, and inventing one would be worse than
    // saying so. Callers must check this before believing any number above.
    bool converged{false};
};

// Trim the model at one condition. Pure: it calls computeForces directly rather than stepping the
// FlightIntegrator, because trim is a function of state, and routing it through an integrator would
// fold integration error into numbers whose entire purpose is comparison against a published chart.
[[nodiscard]] TrimResult trim(const FlightModelData& data, const TrimPoint& pt, const PayloadEffect& payload = {});

} // namespace fl
