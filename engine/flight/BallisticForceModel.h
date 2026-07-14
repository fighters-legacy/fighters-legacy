// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "flight/IForceModel.h"

namespace fl {

// Boost/coast/reentry point-mass force model (#354): the `type = "ballistic"` vehicle.
//
//   - BOOST: constant motor thrust along body-x while propellant (fuel_kg) remains — the burn ends
//     through the integrator's ordinary fuel path (the parser folded burn_time into the fuel flow).
//     During boost the vehicle has control authority via thrust vectoring: small pitch/yaw/roll
//     moments proportional to thrust, which is what the #355 guidance controller steers with.
//   - COAST: no thrust, no moments — inertial flight. Attitude freezes; the velocity vector keeps
//     evolving under gravity (the integrator's −ω×v transport and gravity terms, untouched).
//   - DRAG, always: 0.5·ρ·V·S·cd0 opposing the body-frame velocity vector. With the #354
//     atmosphere extension ρ falls to vacuum above 86 km and rises again on the way down — reentry
//     deceleration emerges from the same term that resisted the boost, no special case.
//
// ZERO LIFT, ZERO stability derivatives: a ballistic vehicle has no wings, and pretending
// otherwise would let `[aero]` numbers nobody validated fly an ICBM. Stateless; plugs into
// FlightIntegrator via the existing setForceModel seam, leaving F=ma, quaternion integration,
// ground collision and fuel burn exactly where they were.
class BallisticForceModel final : public IForceModel {
  public:
    static const BallisticForceModel& instance() {
        static const BallisticForceModel m;
        return m;
    }

    ForceMoment compute(const FlightState& state, const ControlInput& ctrl, const PayloadEffect& payload,
                        const FlightModelData& data, const AtmosphereState& atmos,
                        const AeroInputs& aero) const override;

    // TVC moment arm as a fraction of thrust: moment = ctrl × thrust × kTvcArmM.
    static constexpr float kTvcArmM = 1.5f;
};

} // namespace fl
