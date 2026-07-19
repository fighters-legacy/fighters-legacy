// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "flight/IForceModel.h"

namespace fl {

// Multirotor (quad/hex/octo) force model (#349), behind the IForceModel seam: total rotor thrust
// along body +Y scaled by air density, attitude moments from differential per-rotor thrust, yaw
// from differential rotor torque, flat-plate frame drag. The flight-control INNER LOOP (rate
// feedback on every axis) lives here, because a real multirotor is unflyable without its FC — the
// stick commands a body rate, exactly like the real article's rate mode. Reads
// FlightModelData::multirotor (the [multirotor] TOML block); stateless singleton like the other
// force models. Gravity/turbulence/ground handling stay in the integrator core.
class MultirotorForceModel final : public IForceModel {
  public:
    ForceMoment compute(const FlightState& state, const ControlInput& ctrl, const PayloadEffect& payload,
                        const FlightModelData& data, const AtmosphereState& atmos,
                        const AeroInputs& aero) const override;

    static const MultirotorForceModel& instance();
};

} // namespace fl
