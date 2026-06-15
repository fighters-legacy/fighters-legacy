// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "flight/AeroForces.h" // ControlInput, PayloadEffect, FlightModelData
#include "flight/Atmosphere.h" // AtmosphereState

#include <array>

namespace fl {

struct FlightState; // defined in FlightIntegrator.h

// Derived per-tick aerodynamic inputs the integrator computes before the force model runs.
struct AeroInputs {
    float alpha_rad{0.f}; // angle of attack
    float beta_rad{0.f};  // sideslip
    float mach{0.f};
    float speed_m_s{0.f}; // relative airspeed magnitude
    float altitude_m{0.f};
};

// Aerodynamic + propulsive forces and moments in the body frame [x=forward, y=up, z=right],
// EXCLUDING gravity and turbulence (the integrator adds those).
struct ForceMoment {
    std::array<float, 3> force_body{};  // N
    std::array<float, 3> moment_body{}; // N*m [roll, pitch, yaw]
};

// Vehicle force/moment model. The default FixedWingForceModel implements fixed-wing aerodynamics
// (lift/drag from AoA + stability derivatives + thrust tables). A multirotor or rotor-disc model
// plugs in here for drones/helicopters (issues #349/#350) without modifying the integrator's F=ma,
// quaternion-integration, ground-collision, and fuel-burn core.
struct IForceModel {
    virtual ~IForceModel() = default;
    virtual ForceMoment compute(const FlightState& state, const ControlInput& ctrl, const PayloadEffect& payload,
                                const FlightModelData& data, const AtmosphereState& atmos,
                                const AeroInputs& aero) const = 0;
};

} // namespace fl
