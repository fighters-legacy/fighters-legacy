// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "flight/Articulation.h"
#include "flight/Atmosphere.h"
#include "flight/FlightModelData.h"

#include <array>
#include <cstdint>

namespace fl {

// Controls input, all values normalised to [-1, +1].
// speedbrake: 0 = retracted, 1 = fully deployed.
struct ControlInput {
    float elevator{0.f}; // +1 = pull = nose-up command
    float aileron{0.f};  // +1 = right roll
    float rudder{0.f};   // +1 = right yaw
    float throttle{0.f}; // 0 = idle, 1 = MIL
    bool afterburner{false};
    // ── articulation COMMANDS (#842) ─────────────────────────────────────────
    // These are commands, not positions. The integrator slews FlightState::articulation toward them
    // at the model's declared transit rate, and the aero model reads the POSITION — so gear drag ramps
    // in over its travel window instead of appearing whole in the tick the switch moves.
    float speedbrake{0.f}; // 0-1
    bool gear_down{false};
    float flaps{0.f};         // 0 = clean, 1 = full
    bool hook_down{false};    // arresting hook
    bool canopy_open{false};  // canopy (no aero effect today; drives the animation and egress)
    float tvc_angle_deg{0.f}; // commanded nozzle deflection (pitch axis)

    // ── ground handling (#700) ───────────────────────────────────────────────
    // Wheel brakes, applied by FlightIntegrator ONLY while in ground contact. 0 = off, 1 = full
    // pedal. The integrator adds a brake deceleration (~0.35 g at wheelBrake=1) on top of the
    // baseline rolling resistance to the horizontal body velocity, so a lander can actually stop on
    // the runway. Ignored in the air (no wheels on the ground = no braking). Nosewheel steering is
    // driven off `rudder` inside the integrator, fading out with ground speed — no separate field.
    float wheelBrake{0.f};

    // ── fire intent (#625) ───────────────────────────────────────────────────
    // ONE seam for players, C++ AI, and Lua: PeerController maps these from the wire, AI
    // controllers set them directly, LuaController maps compute_control's return fields. The
    // integrator IGNORES them — firing branches off into FireControl in the weapons pass, never
    // through F=ma. `trigger` is level semantics (guns fire while held); `release` may be held
    // too — FireControl edge-detects per entity, so a stale-repeated input cannot double-fire.
    bool trigger{false};  // gun trigger
    bool release{false};  // fire the selected store (missile/bomb/rocket)
    uint8_t station{255}; // absolute selected station; 255 = keep the current selection

    // ── electronic warfare intent (#529) ─────────────────────────────────────
    // Same one-seam pattern as fire intent. `dispenseCm` pops chaff + flare (edge-detected per entity
    // in the weapons pass — a held input is one release). `ecm` is level: the jammer is on while set.
    // The integrator ignores both; EW branches off in the weapons pass, never through F=ma.
    bool dispenseCm{false};
    bool ecm{false};
};

// Per-tick payload summary (computed from current weapon loadout).
struct PayloadEffect {
    float extra_mass_kg{0.f};
    float extra_cd0{0.f};
};

// Net propulsive thrust (N) for the current throttle/AB state — the single source of truth shared by
// computeForces (body-x thrust) and FixedWingForceModel (the TVC/prop/engine-out moment arms), so the
// two cannot drift. AB uses the ab_thrust table outright. Otherwise, when an [engine.idle_thrust] deck
// is present the result is a linear blend idle → mil across throttle [0, 1] (idle may be negative from
// ram drag, #898); without it, the straight throttle × mil line. mach and alt_km index the tables.
[[nodiscard]] float engineThrustN(const EngineData& engine, float mach, float alt_km, bool ab_engaged,
                                  float throttle_actual);

// Forces in body frame [x=forward, y=up, z=right] (N).
// `art` carries the actuator POSITIONS (#842) — gear, flap and speed-brake drag follow where the
// device actually is, not what was commanded. That is also why ControlInput is no longer a parameter:
// once every device term reads a position, the force calculation depends on STATE alone, and passing
// the commands as well would only invite someone to read one of them again.
std::array<float, 3> computeForces(float alpha_rad, float beta_rad, float mach, float speed_m_s, float altitude_m,
                                   float current_sweep_deg, bool ab_engaged, float throttle_actual,
                                   const PayloadEffect& payload, const FlightModelData& data,
                                   const AtmosphereState& atmos, const ArticulationState& art);

// Moments in body frame [roll, pitch, yaw] (N·m).
std::array<float, 3> computeMoments(float alpha_rad, float beta_rad, float p_rad_s, float q_rad_s, float r_rad_s,
                                    float speed_m_s, float thrust_n, float tvc_angle_rad, const ControlInput& ctrl,
                                    const FlightModelData& data, const AtmosphereState& atmos,
                                    const ArticulationState& art);

} // namespace fl
