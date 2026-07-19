// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "flight/IForceModel.h"

namespace fl {

// Surface-vessel force model (#38), behind the IForceModel seam. A ship is propulsion along the
// keel, quadratic water drag that meets the thrust at the declared top speed, a rate-commanded
// rudder that needs steerage way, and hard damping of the motions a displacement hull does not
// perform (roll, pitch, sideslip). Gravity and the water floor (the radial ground floor clamped to
// sea level by the caller) come from the integrator core — a vessel is an ordinary controlled
// entity, which is the whole point: it replicates, takes damage, and is steered by any
// IEntityController with no bespoke platform machinery. Reads FlightModelData::vessel.
class VesselForceModel final : public IForceModel {
  public:
    ForceMoment compute(const FlightState& state, const ControlInput& ctrl, const PayloadEffect& payload,
                        const FlightModelData& data, const AtmosphereState& atmos,
                        const AeroInputs& aero) const override;

    static const VesselForceModel& instance();
};

} // namespace fl
