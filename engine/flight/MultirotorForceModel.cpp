// SPDX-License-Identifier: GPL-3.0-or-later
#include "flight/MultirotorForceModel.h"

#include "flight/EngineFailFlags.h"  // kEngineFail* — per-rotor loss + total-loss cases
#include "flight/FlightIntegrator.h" // FlightState full definition

#include <cmath>

namespace fl {

ForceMoment MultirotorForceModel::compute(const FlightState& s, const ControlInput& ctrl, const PayloadEffect& payload,
                                          const FlightModelData& data, const AtmosphereState& atmos,
                                          const AeroInputs& /*aero*/) const {
    ForceMoment fm{};
    const MultirotorData mr = data.multirotor.value_or(MultirotorData{});

    // Rotor thrust falls off with air density — this is what gives a multirotor a hover ceiling
    // without any table: the throttle needed to hover rises with altitude until it saturates.
    const float densityScale = atmos.density_kg_m3 / kSeaLevelDensity;
    const int rotors = mr.rotor_count > 0 ? mr.rotor_count : 4;
    const float tMax = static_cast<float>(rotors) * mr.rotor_thrust_max_n * densityScale;
    float thrust = s.throttle_actual * tMax;

    // Engine/rotor failures. Total-loss bits (incl. fuel-starvation flameout, #308 — a dead battery
    // or dry tank stops every motor) kill all thrust; a single L/R loss removes 1/rotor_count of
    // the commanded thrust AND rolls toward the dead side — the surviving rotors' lift is
    // asymmetric about the CG. (A real FC remixes around a dead motor; the residual moment models
    // the degraded authority that remix cannot hide.)
    const uint8_t fail = s.engineFailFlags;
    const bool leftOut = engineLeftOut(fail);
    float rollFromFailure = 0.f;
    if (engineTotalLoss(fail)) {
        thrust = 0.f;
    } else if (leftOut || engineRightOut(fail)) {
        const float lost = engineLostThrust(thrust, rotors);
        thrust -= lost;
        // moment_body[0] is roll about +X, positive = right wing down; losing the LEFT rotor drops
        // the left side (negative roll).
        rollFromFailure = (leftOut ? -1.f : 1.f) * lost * mr.rotor_arm_m;
    }

    fm.force_body[1] += thrust; // rotor thrust along body +Y (up)

    // Flat-plate frame drag opposing the body velocity on every axis. A multirotor's "top speed"
    // is where the tilted thrust's horizontal component meets this drag.
    const float vx = static_cast<float>(s.vel_body[0]);
    const float vy = static_cast<float>(s.vel_body[1]);
    const float vz = static_cast<float>(s.vel_body[2]);
    const float speed = std::sqrt(vx * vx + vy * vy + vz * vz);
    const float qs = 0.5f * atmos.density_kg_m3 * speed * (mr.frame_cd + payload.extra_cd0) * mr.frame_area_m2;
    fm.force_body[0] -= qs * vx;
    fm.force_body[1] -= qs * vy;
    fm.force_body[2] -= qs * vz;

    // Attitude control: rate-mode mixing, the FC inner loop. Stick deflection commands a body
    // rate; the moment is proportional to (command − rate·k), saturating at the differential
    // thrust the frame can generate (attitude_authority × per-rotor max × arm). Full stick settles
    // near 1/rate_damping_s rad/s. moment_body is ordered {roll, pitch, yaw} with pitch ↔ omega[2]
    // and yaw ↔ omega[1] (the BallisticForceModel axis note); yaw about +Y is positive = nose
    // LEFT, and rudder +1 = right yaw, hence the sign flip on the pedal.
    const float ctrlM = mr.attitude_authority * mr.rotor_thrust_max_n * densityScale * mr.rotor_arm_m;
    const float k = mr.rate_damping_s;
    fm.moment_body[0] += (ctrl.aileron - s.omega[0] * k) * ctrlM + rollFromFailure; // roll  ↔ ω[0]
    fm.moment_body[1] += (ctrl.elevator - s.omega[2] * k) * ctrlM;                  // pitch ↔ ω[2]
    fm.moment_body[2] += (-ctrl.rudder - s.omega[1] * k) * mr.yaw_torque_nm;        // yaw   ↔ ω[1]

    return fm;
}

const MultirotorForceModel& MultirotorForceModel::instance() {
    static const MultirotorForceModel model;
    return model;
}

} // namespace fl
