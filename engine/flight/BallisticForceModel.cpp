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
    // while the motor burns, exactly like the real article.
    if (state.fuel_kg > 0.f && data.boost_thrust_n > 0.f) {
        fm.force_body[0] += data.boost_thrust_n;
        const float tvc = data.boost_thrust_n * kTvcArmM;
        fm.moment_body[0] += ctrl.aileron * tvc * 0.1f; // roll authority is a fraction of pitch/yaw
        fm.moment_body[1] += ctrl.elevator * tvc;       // pitch (body-y axis is up; moment[1] = pitch
                                                        // in the integrator's [roll, pitch, yaw] order)
        fm.moment_body[2] += ctrl.rudder * tvc;
    }

    return fm;
}

} // namespace fl
