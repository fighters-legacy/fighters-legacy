// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "flight/IForceModel.h"

namespace fl {

// Fixed-wing aerodynamics: lift/drag from angle-of-attack curves, stability-derivative moments, and
// engine thrust tables — the existing computeForces()/computeMoments() (AeroForces.h) behaviour,
// behind the IForceModel seam. The default model for every FlightIntegrator. Stateless singleton.
class FixedWingForceModel final : public IForceModel {
  public:
    ForceMoment compute(const FlightState& state, const ControlInput& ctrl, const PayloadEffect& payload,
                        const FlightModelData& data, const AtmosphereState& atmos,
                        const AeroInputs& aero) const override;

    static const FixedWingForceModel& instance();
};

} // namespace fl
