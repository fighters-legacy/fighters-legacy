// SPDX-License-Identifier: GPL-3.0-or-later
#include "flight/VesselForceModel.h"

#include "flight/EngineFailFlags.h"  // kEngineFail* — a dead plant stops the screws
#include "flight/FlightIntegrator.h" // FlightState full definition

#include <algorithm>
#include <cmath>
#include <numbers>

namespace fl {

namespace {
constexpr float kDegToRad = static_cast<float>(std::numbers::pi) / 180.f;
// Rate-loop gains, in units of the axis inertia per second — the moment that closes a rate error
// in roughly a second. Ships are heavily damped; precision here is not the point, stability is.
constexpr float kYawRateGain = 0.8f;
constexpr float kLevelDampGain = 1.5f;   // roll/pitch: a displacement hull stays level
constexpr float kLeewayDampPerSec = 1.f; // sideslip (body-z) decay: the hull resists going sideways
} // namespace

ForceMoment VesselForceModel::compute(const FlightState& s, const ControlInput& ctrl, const PayloadEffect& /*payload*/,
                                      const FlightModelData& data, const AtmosphereState& /*atmos*/,
                                      const AeroInputs& /*aero*/) const {
    ForceMoment fm{};
    const VesselData v = data.vessel.value_or(VesselData{});

    // ── Propulsion: throttle × max thrust along the keel; engine failures stop the screws. ────
    float thrust = s.throttle_actual * v.max_thrust_n;
    const uint8_t fail = s.engineFailFlags;
    const bool leftOut = (fail & kEngineFailLeft) != 0;
    const bool rightOut = (fail & kEngineFailRight) != 0;
    if ((fail & (kEngineFailGeneric | kEngineFlameout | kEngineFailCenter | kEngineCompStall)) != 0 ||
        (leftOut && rightOut)) {
        thrust = 0.f;
    } else if (leftOut || rightOut) {
        const int engines = data.engine.engine_count > 0 ? data.engine.engine_count : 2;
        thrust -= thrust / static_cast<float>(engines);
    }
    fm.force_body[0] += thrust;

    // ── Water drag: quadratic along the keel, sized so max_thrust_n meets it exactly at
    // max_speed_mps — the declared top speed IS where the ship stops accelerating. ─────────────
    const float vx = static_cast<float>(s.vel_body[0]);
    const float vy = static_cast<float>(s.vel_body[1]);
    const float vz = static_cast<float>(s.vel_body[2]);
    if (v.max_speed_mps > 0.1f) {
        const float k = v.max_thrust_n / (v.max_speed_mps * v.max_speed_mps);
        fm.force_body[0] -= k * std::abs(vx) * vx;
    }
    // Leeway: the hull resists sideways and vertical motion far harder than forward motion.
    fm.force_body[2] -= s.mass_kg * kLeewayDampPerSec * vz;
    fm.force_body[1] -= s.mass_kg * kLeewayDampPerSec * vy;

    // ── Steering: the rudder commands a yaw RATE, scaled by steerage way — a stopped ship does
    // not answer the helm. omega[1] is yaw about +Y (positive = nose LEFT); rudder +1 = starboard
    // turn, hence the sign flip. Roll and pitch are damped flat: no waves in this model. ────────
    const float speedFrac = (v.steerage_mps > 0.f) ? std::clamp(std::abs(vx) / v.steerage_mps, 0.f, 1.f) : 1.f;
    const float targetYawRate = -ctrl.rudder * v.turn_rate_deg_s * kDegToRad * speedFrac;
    fm.moment_body[2] += (targetYawRate - s.omega[1]) * kYawRateGain * data.geometry.izz_kg_m2;
    fm.moment_body[0] += -s.omega[0] * kLevelDampGain * data.geometry.ixx_kg_m2;
    fm.moment_body[1] += -s.omega[2] * kLevelDampGain * data.geometry.iyy_kg_m2;

    return fm;
}

const VesselForceModel& VesselForceModel::instance() {
    static const VesselForceModel model;
    return model;
}

} // namespace fl
