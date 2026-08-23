// SPDX-License-Identifier: GPL-3.0-or-later
#include "flight/HelicopterForceModel.h"

#include "flight/Atmosphere.h"       // kSeaLevelDensity
#include "flight/EngineFailFlags.h"  // kEngineFail* — engine-out / autorotation entry
#include "flight/FlightIntegrator.h" // FlightState full definition

#include <algorithm>
#include <cmath>
#include <numbers>

namespace fl {

ForceMoment HelicopterForceModel::compute(const FlightState& s, const ControlInput& ctrl, const PayloadEffect& payload,
                                          const FlightModelData& data, const AtmosphereState& atmos,
                                          const AeroInputs& aero) const {
    ForceMoment fm{};
    const HelicopterData h = data.helicopter.value_or(HelicopterData{});

    const float vx = static_cast<float>(s.vel_body[0]);
    const float vy = static_cast<float>(s.vel_body[1]);
    const float vz = static_cast<float>(s.vel_body[2]);

    // ── Main rotor thrust: collective × density-scaled max, engine-out aware. ─────────────────
    const float densityScale = atmos.density_kg_m3 / kSeaLevelDensity;
    float thrust = s.throttle_actual * h.main_rotor_max_thrust_n * densityScale;

    // Engine failures (#308/#675). Total-loss bits (flameout, generic, centreline, surge) unpower
    // the rotor — autorotation below is what keeps the machine alive. A single engine of a
    // multi-engine ship out sheds 1/engine_count of available power; the rotor stays centreline,
    // so unlike a fixed-wing twin there is NO yaw from the loss.
    const uint8_t fail = s.engineFailFlags;
    if (engineTotalLoss(fail)) {
        thrust = 0.f;
    } else if (engineLeftOut(fail) || engineRightOut(fail)) {
        thrust -= engineLostThrust(thrust, data.engine.engine_count);
    }

    // Ground effect: within ~one rotor diameter of the surface the disc rides its own outwash —
    // up to ground_effect_frac more thrust on the deck, fading linearly out by 2R AGL.
    const float diameterAgl = (h.main_rotor_radius_m > 0.f) ? aero.agl_m / (2.f * h.main_rotor_radius_m) : 1.f;
    thrust *= 1.f + h.ground_effect_frac * std::clamp(1.f - diameterAgl, 0.f, 1.f);

    // Effective translational lift: forward flight feeds the disc clean air; thrust efficiency
    // rises with speed, saturating at translational_lift_mps.
    const float horizSpd = std::sqrt(vx * vx + vz * vz);
    if (h.translational_lift_mps > 0.f)
        thrust *= 1.f + h.translational_lift_frac * std::min(horizSpd / h.translational_lift_mps, 1.f);

    fm.force_body[1] += thrust;

    // ── Autorotation / axial disc momentum drag. ──────────────────────────────────────────────
    // Air forced axially through the disc resists axial motion; unpowered and descending this is
    // what caps the sink rate at the autorotation figure (terminal sink =
    // sqrt(2·W / (ρ·A·cd_auto))), and it is always on — at hover it is negligible (vy ≈ 0).
    const float discArea = std::numbers::pi_v<float> * h.main_rotor_radius_m * h.main_rotor_radius_m;
    fm.force_body[1] -= 0.5f * atmos.density_kg_m3 * discArea * h.autorotation_cd * std::abs(vy) * vy;

    // ── Parasite drag: flat plate on every axis. ──────────────────────────────────────────────
    const float speed = std::sqrt(vx * vx + vy * vy + vz * vz);
    const float qs = 0.5f * atmos.density_kg_m3 * speed * (h.frame_cd + payload.extra_cd0) * h.frame_area_m2;
    fm.force_body[0] -= qs * vx;
    fm.force_body[1] -= qs * vy;
    fm.force_body[2] -= qs * vz;

    // ── Moments. moment_body is {roll, pitch, yaw}; pitch ↔ omega[2], yaw ↔ omega[1] (positive =
    // nose LEFT about +Y, hence the pedal sign flip). Cyclic authority follows density and
    // collective loosely in the real article; kept constant here — the rotor-follow damping term
    // is what makes the response feel like a disc, and a constant keeps the model predictable.
    const float k = h.rate_damping_s;
    fm.moment_body[0] += (ctrl.aileron - s.omega[0] * k) * h.cyclic_moment_nm;  // roll  ↔ ω[0]
    fm.moment_body[1] += (ctrl.elevator - s.omega[2] * k) * h.cyclic_moment_nm; // pitch ↔ ω[2]

    // Flapback: the advancing blade flaps up, tilting the disc aft — the nose rises with forward
    // speed. This is the speed stability a helicopter pilot trims against.
    fm.moment_body[1] += h.flapback_nm_per_mps * vx;

    // Tail rotor vs main-rotor torque reaction. A CCW-viewed-from-above main rotor torques the
    // fuselage nose-RIGHT (negative about +Y=up); pedals hold against it. torque_factor = 0 models
    // an auto-trimmed hover (no pedal work), which is the forgiving default.
    const float torqueReaction = h.torque_factor * thrust * h.main_rotor_radius_m;
    fm.moment_body[2] += (-ctrl.rudder - s.omega[1] * k) * h.yaw_moment_max_nm - torqueReaction;

    return fm;
}

const HelicopterForceModel& HelicopterForceModel::instance() {
    static const HelicopterForceModel model;
    return model;
}

} // namespace fl
