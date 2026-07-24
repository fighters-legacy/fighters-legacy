// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Articulation — actuator POSITIONS and their transit dynamics (#842, Epic #837).
//
// Before this, gear and speedbrake existed only as COMMANDS on ControlInput, and the aero model
// added their full drag the instant the command flipped: gear that takes six seconds to travel
// produced all of its drag in one tick. Flaps, hook and canopy did not exist at all.
//
// So the simulation now carries a POSITION per actuator, slewed toward its command at a
// model-declared rate — the same discipline `advanceSweep`/`advanceTvc` have always used for wing
// sweep and TVC, which is why those two were already right. Drag follows the POSITION, which is both
// correct and the reason the animation can be driven from the same number (#841).
//
// TRANSIT TIMING LIVES HERE, NEVER IN THE CLIP. An author retiming a gear animation changes how the
// gear looks, not how long it takes — and the server, the client's prediction replay and the
// renderer all read one number.
//
// Pure and header-only so `FlightIntegrator::step` and `ClientPrediction`'s replay run the SAME code
// by construction: if the two sides ever disagreed about where the gear is, they would disagree
// about where the aircraft is, because gear position is drag.

#include <algorithm>
#include <cmath>

namespace fl {

// Normalized actuator positions. 0 = up/clean/stowed/closed, 1 = down/full/deployed/open.
struct ArticulationState {
    float gear{0.f};
    float flaps{0.f};
    float speedbrake{0.f};
    float hook{0.f};
    float canopy{0.f};
};

// Full-travel times in seconds, from the flight model's optional `[articulation]` table. Defaults are
// plausible for a light fighter, so every existing flight model keeps parsing and keeps flying.
struct ArticulationTimes {
    float gear_transit_s{6.0f};
    float flap_transit_s{4.0f};
    float speedbrake_transit_s{1.5f};
    float hook_transit_s{2.0f};
    float canopy_transit_s{5.0f};
};

// Slew one actuator toward its command. Reversing mid-travel reverses FROM THE CURRENT POSITION —
// there is no snap, because a hydraulic actuator asked to go back the way it came does not teleport.
// A non-positive transit time means "instantaneous", which is the honest reading of a model that
// declares no travel time for an actuator.
[[nodiscard]] inline float advanceActuator(float cur, float commanded, float dt, float transit_s) noexcept {
    const float target = std::clamp(commanded, 0.0f, 1.0f);
    if (!(transit_s > 0.0f) || !(dt > 0.0f))
        return target;
    const float step = dt / transit_s; // full travel is 0 -> 1, so the rate is 1/transit
    const float delta = target - cur;
    if (std::abs(delta) <= step)
        return target;
    return std::clamp(cur + std::copysign(step, delta), 0.0f, 1.0f);
}

// Advance every actuator one tick from its command. Commands are the booleans/scalars on
// ControlInput; the caller passes them in rather than this header depending on AeroForces.h, which
// keeps the module usable from the validator and the tests without pulling the aero model in.
struct ArticulationCommand {
    bool gear_down{false};
    float flaps{0.f};
    float speedbrake{0.f};
    bool hook_down{false};
    bool canopy_open{false};
};

inline void advanceArticulation(ArticulationState& s, const ArticulationCommand& cmd, const ArticulationTimes& t,
                                float dt) noexcept {
    s.gear = advanceActuator(s.gear, cmd.gear_down ? 1.0f : 0.0f, dt, t.gear_transit_s);
    s.flaps = advanceActuator(s.flaps, cmd.flaps, dt, t.flap_transit_s);
    s.speedbrake = advanceActuator(s.speedbrake, cmd.speedbrake, dt, t.speedbrake_transit_s);
    s.hook = advanceActuator(s.hook, cmd.hook_down ? 1.0f : 0.0f, dt, t.hook_transit_s);
    s.canopy = advanceActuator(s.canopy, cmd.canopy_open ? 1.0f : 0.0f, dt, t.canopy_transit_s);
}

} // namespace fl
