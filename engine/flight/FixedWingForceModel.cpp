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

    // Thrust magnitude (for the TVC moment and prop effects inside computeMoments). Same helper
    // computeForces uses, so the two thrust figures — and the idle deck (#898) — cannot drift.
    const float alt_km = aero.altitude_m / 1000.f;
    const float thrust_n = engineThrustN(data.engine, aero.mach, alt_km, s.ab_engaged, s.throttle_actual);

    // omega[0]=roll(X), omega[1]=yaw(Y=up), omega[2]=pitch(Z=right); computeMoments wants (p,q,r) in
    // the STANDARD aero body frame (x=fwd, y=right, z=down), where yaw rate r is positive nose-RIGHT.
    // The engine's yaw axis is +Y=UP, and a positive rate about +Y is nose-LEFT (the two frames
    // differ by a 180° roll: aero_z=down = −engine_y=up). So the yaw rate handed to computeMoments
    // must be −omega[1] (roll and pitch axes coincide and pass straight through). See the matching
    // moment flip below — together they were #891's directional divergence: a sideslip fed the
    // weathercock moment back with the wrong sign and the aircraft departed on any perturbation.
    auto moments = computeMoments(aero.alpha_rad, aero.beta_rad, s.omega[0], s.omega[2], -s.omega[1], aero.speed_m_s,
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
        moments[2] += leftOut ? -yaw : yaw; // aero convention (nose-right positive), like computeMoments
    }

    // #891: convert the yaw moment from the standard aero convention (positive N = nose right, about
    // aero +Z=down) that computeMoments and the engine-out term above both use, into the engine body
    // frame's yaw axis (+Y=up, positive = nose left). The integrator maps moments[2] straight onto
    // omega[1] (omega[1] += moments[2]/Izz), and BallisticForceModel already emits its yaw moment in
    // that +Y convention, so the flip belongs here, at the aero→engine seam — not in the integrator.
    // Roll (moments[0]) and pitch (moments[1]) need no flip: their axes coincide in both frames.
    moments[2] = -moments[2];

    return {forces, moments};
}

const FixedWingForceModel& FixedWingForceModel::instance() {
    static const FixedWingForceModel model;
    return model;
}

} // namespace fl
