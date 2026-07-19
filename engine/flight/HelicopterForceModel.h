// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "flight/IForceModel.h"

namespace fl {

// Helicopter rotor-disc force model (#350), behind the IForceModel seam. The rotor is modelled as
// a DISC, not blade elements: collective (throttle) drives density-scaled thrust along body-up,
// scaled up by ground effect near the surface and by effective translational lift with forward
// speed; cyclic tilts the disc (pitch/roll moments with rotor-follow rate damping); pedals command
// the tail rotor against the optional main-rotor torque reaction; blade flapping appears as the
// classic flapback speed-stability moment. An UNPOWERED disc autorotates: axial momentum drag
// through the disc area caps the sink rate at a survivable figure — the gameplay truth of
// autorotation without carrying rotor-RPM state (a flare-energy model needs stored rotor momentum
// and is out of scope; the simplification is documented, not hidden). Reads
// FlightModelData::helicopter (the [helicopter] TOML block); stateless singleton.
class HelicopterForceModel final : public IForceModel {
  public:
    ForceMoment compute(const FlightState& state, const ControlInput& ctrl, const PayloadEffect& payload,
                        const FlightModelData& data, const AtmosphereState& atmos,
                        const AeroInputs& aero) const override;

    static const HelicopterForceModel& instance();
};

} // namespace fl
