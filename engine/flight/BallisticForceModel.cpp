// SPDX-License-Identifier: GPL-3.0-or-later
#include "flight/BallisticForceModel.h"

#include "flight/FlightIntegrator.h" // FlightState
#include "flight/FlightModelData.h"

#include <cmath>

namespace fl {

ForceMoment BallisticForceModel::compute(const FlightState& state, const ControlInput& ctrl,
                                         const PayloadEffect& payload, const FlightModelData& data,
                                         const AtmosphereState& atmos, const AeroInputs& /*aero*/) const {
    ForceMoment fm{};

    // Drag opposing the body-frame velocity vector: F = −½·ρ·V·S·(cd0 + payload extra). At 86 km ρ
    // is zero and this term vanishes (vacuum coast); on the way back down it IS the reentry
    // deceleration.
    const float vx = static_cast<float>(state.vel_body[0]);
    const float vy = static_cast<float>(state.vel_body[1]);
    const float vz = static_cast<float>(state.vel_body[2]);
    const float speed = std::sqrt(vx * vx + vy * vy + vz * vz);
    const float cd = data.drag_polar.cd0 + payload.extra_cd0;
    const float qs = 0.5f * atmos.density_kg_m3 * speed * data.geometry.wing_area_m2 * cd;
    fm.force_body[0] -= qs * vx;
    fm.force_body[1] -= qs * vy;
    fm.force_body[2] -= qs * vz;

    // Boost: constant thrust along body-x while propellant remains. Control authority is thrust
    // vectoring — pitch (elevator), yaw (rudder) and roll (aileron) moments proportional to the
    // motor, so a burned-out vehicle is inertial: the #355 guidance controller can only steer
    // while the motor burns, exactly like the real article. The TVC command carries RATE FEEDBACK
    // (cmd − k·ω), because that loop lives in a real missile's autopilot, not in its guidance —
    // without it a bang-bang guidance command tumbles the airframe (22 rad/s² of raw authority on
    // a 4000 kg·m² booster is not something an outer loop at 60 Hz can stabilize alone).
    // AXIS NOTE. moment_body is ordered BY LABEL {roll, pitch, yaw} (IForceModel contract), but
    // the body frame is x=forward, y=UP, z=right — so the integrator maps moments[1] (pitch) onto
    // omega[2] (rate about body-Z/right) and moments[2] (yaw) onto omega[1] (about body-Y/up).
    // Rate feedback MUST pair each label with ITS rate: pairing moments[1] with omega[1] feeds the
    // yaw rate into the pitch damper and the cross-coupled loop tears the airframe into a tumble.
    if (state.fuel_kg > 0.f && data.boost_thrust_n > 0.f) {
        fm.force_body[0] += data.boost_thrust_n;
        const float tvc = data.boost_thrust_n * kTvcArmM;
        fm.moment_body[0] += (ctrl.aileron * 0.1f - state.omega[0] * kTvcRateFeedback) * tvc * 0.1f;
        fm.moment_body[1] += (ctrl.elevator - state.omega[2] * kTvcRateFeedback) * tvc; // pitch ↔ ω[2]
        fm.moment_body[2] += (ctrl.rudder - state.omega[1] * kTvcRateFeedback) * tvc;   // yaw   ↔ ω[1]
    }

    // Fin damping, always: a finned body weathervanes — rotation is resisted proportionally to
    // dynamic pressure. This is also what stops a burned-out vehicle from tumbling forever in air
    // (in vacuum it genuinely keeps its rates, which is physics, not a bug).
    const float finDamp = 0.5f * atmos.density_kg_m3 * speed * data.geometry.wing_area_m2 * data.geometry.mac_m *
                          data.geometry.mac_m * kFinDampCoeff;
    fm.moment_body[0] -= state.omega[0] * finDamp; // roll
    fm.moment_body[1] -= state.omega[2] * finDamp; // pitch
    fm.moment_body[2] -= state.omega[1] * finDamp; // yaw

    return fm;
}

} // namespace fl
