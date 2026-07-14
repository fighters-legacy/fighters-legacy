// SPDX-License-Identifier: GPL-3.0-or-later
#include "flight/FixedWingForceModel.h"

#include "flight/EngineFailFlags.h"  // kEngineFail* (asymmetric thrust, #675)
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

    // Engine-out asymmetry (#675). engineFailFlags is set by per-subsystem damage; until now it was
    // parsed and ignored. A total loss (both engines, flameout, or the generic flag) zeroes the
    // propulsive force. A SINGLE engine out halves it AND yaws the nose toward the dead engine —
    // the remaining engine's thrust line is off the centreline, so it produces a yawing moment
    // proportional to the lost thrust times a moment arm (a fraction of the semi-span, since engine
    // positions are not modelled). This is the asymmetric-thrust behaviour #675 exists to deliver.
    const uint8_t fail = s.engineFailFlags;
    const bool leftOut = (fail & kEngineFailLeft) != 0;
    const bool rightOut = (fail & kEngineFailRight) != 0;
    const bool totalLoss = (fail & (kEngineFailGeneric | kEngineFlameout)) != 0 || (leftOut && rightOut);
    if (totalLoss) {
        forces[0] -= thrust_n; // computeForces already added the full thrust to body-x; remove it
    } else if (leftOut || rightOut) {
        const float lost = 0.5f * thrust_n; // one of two engines
        forces[0] -= lost;
        // Moment about body-Y (yaw) = lost thrust × arm; sign yaws toward the dead engine. The
        // integrator maps computeMoments' index [2] (yaw) onto omega[1].
        const float arm = 0.15f * data.geometry.wingspan_m;
        const float yaw = lost * arm;
        moments[2] += leftOut ? -yaw : yaw; // left-out yaws nose left (−yaw about +Y)
    }

    return {forces, moments};
}

const FixedWingForceModel& FixedWingForceModel::instance() {
    static const FixedWingForceModel model;
    return model;
}

} // namespace fl
