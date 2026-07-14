// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "flight/AeroForces.h"
#include "flight/FlightModelData.h"

namespace fl {

// One flight condition to trim the aircraft at.
//
// PINNING A CONDITION IS THE WHOLE POINT (#826). Published flight-manual data is almost never quoted
// as a speed-maximised value: every F-5E turn number in T.O. 1F-5E-1 is given AT A SPECIFIC MACH
// (15 000 ft, M 0.60: sustained 3.3 g), and its specific-excess-power ladder is given at a specific
// Mach AND a specific load factor (M 0.60, n = 4.0 → Ps = −225 ft/s). Those are not the same
// quantities as "the best turn the aircraft can manage at any speed", so they cannot be compared
// against a maximised number — and the Ps ladder is the richest drag constraint the aircraft has. It
// is what [aero.cd_table] is fitted to (#820), and without a way to pin the condition, CI could not
// check the model's single most important calibration.
struct TrimPoint {
    float altitude_m{0.f};
    float mass_kg{0.f}; // empty + fuel + payload; the caller decides the loading

    // 0 = evaluate the speed-maximised envelope (the default). Non-zero = evaluate AT this Mach, which
    // is how the flight manual quotes it.
    float mach{0.f};

    // 0 = not applicable. Non-zero = the load factor to evaluate ps_mps at.
    float load_factor{0.f};

    // Thrust setting for the thrust-dependent metrics. The manual's turn and Ps numbers are quoted at
    // "max thrust", so this defaults to on (and falls back to MIL for a model with no AB table).
    bool afterburner{true};
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

    // ── evaluated only when TrimPoint::mach is pinned (#826) ─────────────────────────────────────
    // When a Mach is given, sustained_*/instant_* above describe THAT Mach rather than the best the
    // aircraft can do at any speed, and these two become meaningful.
    float max_lift_g{0.f}; // the lift-limited load factor at this Mach — what pins CL_max
    float ps_mps{0.f};     // specific excess power, V·(T − D)/W, at this Mach and load_factor

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
