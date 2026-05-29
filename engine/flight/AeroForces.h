// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "flight/Atmosphere.h"
#include "flight/FlightModelData.h"

#include <array>

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
};

// Per-tick payload summary (computed from current weapon loadout).
struct PayloadEffect {
    float extra_mass_kg{0.f};
    float extra_cd0{0.f};
};

// Forces in body frame [x=forward, y=right, z=down] (N).
std::array<float, 3> computeForces(float alpha_rad, float beta_rad, float mach, float speed_m_s, float altitude_m,
                                   float current_sweep_deg, bool ab_engaged, float throttle_actual,
                                   const ControlInput& ctrl, const PayloadEffect& payload, const FlightModelData& data,
                                   const AtmosphereState& atmos);

// Moments in body frame [roll, pitch, yaw] (N·m).
std::array<float, 3> computeMoments(float alpha_rad, float beta_rad, float p_rad_s, float q_rad_s, float r_rad_s,
                                    float speed_m_s, float thrust_n, float tvc_angle_rad, const ControlInput& ctrl,
                                    const FlightModelData& data, const AtmosphereState& atmos);

} // namespace fl
