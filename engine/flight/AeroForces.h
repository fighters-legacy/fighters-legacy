// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

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
    float speedbrake{0.f}; // 0–1
    bool gear_down{false};
    float tvc_angle_deg{0.f}; // commanded nozzle deflection (pitch axis)

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
std::array<float, 3> computeForces(float alpha_rad, float beta_rad, float mach, float speed_m_s, float altitude_m,
                                   float current_sweep_deg, bool ab_engaged, float throttle_actual,
                                   const ControlInput& ctrl, const PayloadEffect& payload, const FlightModelData& data,
                                   const AtmosphereState& atmos);

// Moments in body frame [roll, pitch, yaw] (N·m).
std::array<float, 3> computeMoments(float alpha_rad, float beta_rad, float p_rad_s, float q_rad_s, float r_rad_s,
                                    float speed_m_s, float thrust_n, float tvc_angle_rad, const ControlInput& ctrl,
                                    const FlightModelData& data, const AtmosphereState& atmos);

} // namespace fl
