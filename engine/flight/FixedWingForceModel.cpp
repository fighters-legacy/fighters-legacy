// SPDX-License-Identifier: GPL-3.0-or-later
#include "flight/FixedWingForceModel.h"

#include "flight/FlightIntegrator.h" // FlightState full definition

#include <numbers>

namespace fl {

ForceMoment FixedWingForceModel::compute(const FlightState& s, const ControlInput& ctrl, const PayloadEffect& payload,
                                         const FlightModelData& data, const AtmosphereState& atmos,
                                         const AeroInputs& aero) const {
    constexpr float kDegToRad = static_cast<float>(std::numbers::pi) / 180.f;

    auto forces = computeForces(aero.alpha_rad, aero.beta_rad, aero.mach, aero.speed_m_s, aero.altitude_m,
                                s.current_sweep_deg, s.ab_engaged, s.throttle_actual, ctrl, payload, data, atmos);

    // Thrust magnitude (for the TVC moment and prop effects inside computeMoments).
    const float alt_km = aero.altitude_m / 1000.f;
    const float mil_kn = data.engine.mil_thrust.lookup(aero.mach, alt_km);
    float thrust_n;
    if (s.ab_engaged && data.engine.ab_thrust)
        thrust_n = data.engine.ab_thrust->lookup(aero.mach, alt_km) * 1000.f;
    else
        thrust_n = s.throttle_actual * mil_kn * 1000.f;

    // omega[0]=roll(X), omega[1]=yaw(Y=up), omega[2]=pitch(Z=right); computeMoments wants (p,q,r).
    auto moments = computeMoments(aero.alpha_rad, aero.beta_rad, s.omega[0], s.omega[2], s.omega[1], aero.speed_m_s,
                                  thrust_n, s.tvc_angle_deg * kDegToRad, ctrl, data, atmos);

    return {forces, moments};
}

const FixedWingForceModel& FixedWingForceModel::instance() {
    static const FixedWingForceModel model;
    return model;
}

} // namespace fl
