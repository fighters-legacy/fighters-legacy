// SPDX-License-Identifier: GPL-3.0-or-later
#include "flight/AeroForces.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace fl {

namespace {

constexpr float kDegToRad = static_cast<float>(std::numbers::pi) / 180.f;

// Wing sweep correction: returns effective cl_scale, k_scale, cd0_delta
// at the current sweep angle, interpolated between spread and swept configs.
struct SweepCorrection {
    float cl_scale;
    float k_scale;
    float cd0_delta;
};

SweepCorrection sweepCorrection(float current_sweep_deg, const WingSweepData& ws) {
    float t = (current_sweep_deg - ws.min_deg) / (ws.max_deg - ws.min_deg);
    t = std::clamp(t, 0.f, 1.f);
    return {
        ws.spread.cl_scale + t * (ws.swept.cl_scale - ws.spread.cl_scale),
        ws.spread.k_scale + t * (ws.swept.k_scale - ws.spread.k_scale),
        ws.spread.cd0_delta + t * (ws.swept.cd0_delta - ws.spread.cd0_delta),
    };
}

} // namespace

float engineThrustN(const EngineData& engine, float mach, float alt_km, bool ab_engaged, float throttle_actual) {
    if (ab_engaged && engine.ab_thrust)
        return engine.ab_thrust->lookup(mach, alt_km) * 1000.f;

    const float mil_kn = engine.mil_thrust.lookup(mach, alt_km);
    if (engine.idle_thrust) {
        // Blend idle → mil across throttle [0, 1] (#898). idle_kn may be negative (ram drag > idle
        // gross thrust). At throttle 0 the aircraft feels the published idle deck; at 1, MIL.
        const float idle_kn = engine.idle_thrust->lookup(mach, alt_km);
        return (idle_kn + throttle_actual * (mil_kn - idle_kn)) * 1000.f;
    }
    return throttle_actual * mil_kn * 1000.f;
}

std::array<float, 3> computeForces(float alpha_rad, float beta_rad, float mach, float speed_m_s, float altitude_m,
                                   float current_sweep_deg, bool ab_engaged, float throttle_actual,
                                   const PayloadEffect& payload, const FlightModelData& data,
                                   const AtmosphereState& atmos, const ArticulationState& art) {
    (void)beta_rad; // lateral side force from sideslip omitted — handled via moments only
    const float alpha_deg = alpha_rad / kDegToRad;
    const float q_dyn = 0.5f * atmos.density_kg_m3 * speed_m_s * speed_m_s;
    const float S = data.geometry.wing_area_m2;

    // ── Lift ──────────────────────────────────────────────────────────────────
    // High-lift device (#842): the flap's alpha shift moves the CL-table lookup, so a given body
    // alpha reads more CL AND the stall arrives at a lower body alpha -- which is what a flap
    // actually does. Both terms scale linearly with flap POSITION, so mid-transit is mid-effect.
    const float flapPos = std::clamp(art.flaps, 0.f, 1.f);
    float cl = data.cl_table.lookup(alpha_deg + flapPos * data.flaps.alpha_shift_deg, mach);
    cl += flapPos * data.flaps.dcl;

    if (data.wing_sweep) {
        auto sc = sweepCorrection(current_sweep_deg, *data.wing_sweep);
        cl *= sc.cl_scale;
    }

    float lift = q_dyn * S * cl; // N, perpendicular to velocity vector

    // Speed-brake normal-force (lift) increment (#899, ΔCZ,sb). A real airbrake changes lift as well
    // as drag; added straight to the lift force so it does NOT feed the induced-drag term (that is the
    // speed-brake's own speedbrake_cd's job). 0 by default, so most content is unaffected.
    lift += q_dyn * S * (data.drag_polar.speedbrake_cl * art.speedbrake);

    // ── Drag ──────────────────────────────────────────────────────────────────
    float cd0 = data.drag_polar.cd0 + payload.extra_cd0;
    float k_eff = data.drag_polar.k;

    if (data.wing_sweep) {
        auto sc = sweepCorrection(current_sweep_deg, *data.wing_sweep);
        cd0 += sc.cd0_delta;
        k_eff *= sc.k_scale;
    }

    float cd_wave = 0.f;
    if (data.cd_wave)
        cd_wave = data.cd_wave->lookup(mach);

    // Device drag follows the actuator POSITION, not the command (#842). Before this, gear that takes
    // six seconds to travel produced its full drag in the tick the switch moved -- and there was no
    // number an animation could have been driven from.
    float cd_device =
        art.speedbrake * data.drag_polar.speedbrake_cd + art.gear * data.drag_polar.gear_cd + flapPos * data.flaps.dcd;

    // Clean drag: tabulated when the model provides one, parabolic otherwise (#820).
    //
    // A cd_table is TOTAL clean drag — it already contains the induced term, because that is how real
    // aerodynamic data is published (NASA TP-1538 gives the F-16's CD against alpha and Mach; it
    // cannot be transcribed into cd0 + k*CL^2 at all). So the table REPLACES the parabolic pair
    // rather than adding to it: summing them would double-count induced drag, a silent 2x drag bug
    // that a content author has no way to debug. The validator makes authoring both an error.
    //
    // Device drag, wave drag and the payload's cd0 still add on top of either path — they are
    // additive and independent of how the clean airframe's drag was expressed.
    float cd_clean;
    if (data.cd_table) {
        cd_clean = data.cd_table->lookup(alpha_deg, mach) + payload.extra_cd0;
        if (data.wing_sweep) {
            auto sc = sweepCorrection(current_sweep_deg, *data.wing_sweep);
            cd_clean += sc.cd0_delta;
        }
    } else {
        cd_clean = cd0 + k_eff * cl * cl;
    }

    float cd_total = cd_clean + cd_wave + cd_device;
    float drag = q_dyn * S * cd_total; // N, opposing velocity

    // ── Thrust ────────────────────────────────────────────────────────────────
    // Shared helper so the body-x thrust here and the moment-arm thrust in FixedWingForceModel cannot
    // diverge; also carries the optional idle deck (#898).
    float alt_km = altitude_m / 1000.f;
    float thrust_n = engineThrustN(data.engine, mach, alt_km, ab_engaged, throttle_actual);

    // ── Body-frame assembly ───────────────────────────────────────────────────
    // Body frame: x=forward, y=up, z=right. Gravity added by integrator.
    // Lift is normal to velocity (acts upward = +y). Drag opposes velocity.
    //   x (forward): thrust − drag*cos(alpha) + lift*sin(alpha)
    //   y (up):      lift*cos(alpha) + drag*sin(alpha)   (net upward aero force)
    //   z (right):   0 (side force from beta handled via moments only)
    float cos_a = std::cos(alpha_rad);
    float sin_a = std::sin(alpha_rad);

    std::array<float, 3> forces{};
    forces[0] = thrust_n - drag * cos_a + lift * sin_a; // x: forward
    forces[1] = lift * cos_a + drag * sin_a;            // y: up
    forces[2] = 0.f;                                    // z: right (side force omitted)

    return forces;
}

std::array<float, 3> computeMoments(float alpha_rad, float beta_rad, float p_rad_s, float q_rad_s, float r_rad_s,
                                    float speed_m_s, float thrust_n, float tvc_angle_rad, const ControlInput& ctrl,
                                    const FlightModelData& data, const AtmosphereState& atmos,
                                    const ArticulationState& art) {
    const float q_dyn = 0.5f * atmos.density_kg_m3 * speed_m_s * speed_m_s;
    const float S = data.geometry.wing_area_m2;
    const float mac = data.geometry.mac_m;
    const float span = data.geometry.wingspan_m;

    // Guard against near-zero airspeed to avoid division by zero in rate terms.
    // Below 1 m/s, aerodynamic moments are negligible.
    const bool has_airspeed = speed_m_s >= 1.f;
    const float inv_2v = has_airspeed ? 1.f / (2.f * speed_m_s) : 0.f;

    // Map pilot inputs to NACA/DATCOM deflection convention (trailing-edge direction).
    // cm_de < 0: trailing-edge-DOWN increases nose-DOWN moment.
    // Pull (+1 elevator) = trailing-edge UP = negative NACA deflection → positive pitch moment.
    // Right yaw (+1 rudder) = trailing-edge LEFT = negative NACA deflection → positive yaw moment.
    // Right roll (+1 aileron) = left-aileron-down convention → positive roll moment (no sign flip).
    // Asymmetric pitch travel (#822): a fighter's stabilator has far more nose-up authority than
    // nose-down (17° vs 5° on an F-5E). ctrl.elevator is +1 = pull = nose-up, so the positive side
    // uses max_elevator_deg and the negative side uses max_elevator_neg_deg — which equals it unless
    // the model says otherwise, so a symmetric model is unaffected.
    const float elev_travel_deg =
        (ctrl.elevator >= 0.f) ? data.controls.max_elevator_deg : data.controls.max_elevator_neg_deg;
    const float elev_rad = -ctrl.elevator * elev_travel_deg * kDegToRad;
    const float ail_rad = ctrl.aileron * data.controls.max_aileron_deg * kDegToRad;
    const float rudder_rad = -ctrl.rudder * data.controls.max_rudder_deg * kDegToRad;

    // Alpha-dependent dynamic dampers (#899): the table replaces the scalar when present, else the
    // scalar. A high-alpha airframe's pitch/roll/yaw damping changes across the alpha sweep, and a
    // deep-stall model needs the post-stall values a single scalar cannot carry.
    const float alpha_deg = alpha_rad / kDegToRad;
    const float cm_q_eff = data.moments.cm_q_table ? data.moments.cm_q_table->lookup(alpha_deg) : data.moments.cm_q;
    const float cl_p_eff = data.moments.cl_p_table ? data.moments.cl_p_table->lookup(alpha_deg) : data.moments.cl_p;
    const float cn_r_eff = data.moments.cn_r_table ? data.moments.cn_r_table->lookup(alpha_deg) : data.moments.cn_r;

    // Pitch moment (positive = nose up). cm0 (#899) is the zero-alpha pitching moment — a cambered
    // wing trims at a non-zero alpha with zero elevator. cm_speedbrake adds the airbrake's pitch
    // increment when deployed. Both default 0.
    float cm = data.moments.cm0 + data.moments.cm_alpha * alpha_rad + cm_q_eff * (q_rad_s * mac * inv_2v) +
               data.moments.cm_de * elev_rad + data.moments.cm_speedbrake * art.speedbrake;
    float pitch_moment = q_dyn * S * mac * cm;

    // TVC pitch contribution: moment arm from CG assumed ≈ 0 (moment = thrust × sin(tvc))
    if (data.tvc)
        pitch_moment += thrust_n * std::sin(tvc_angle_rad);

    // Roll moment (positive = right wing down). cl_dr (#899) is rudder-induced roll — rudder
    // deflection rolls the aircraft; 0 by default.
    float cl_coeff = data.moments.cl_beta * beta_rad + cl_p_eff * (p_rad_s * span * inv_2v) +
                     data.moments.cl_da * ail_rad + data.moments.cl_dr * rudder_rad;
    float roll_moment = q_dyn * S * span * cl_coeff;

    // Prop torque and gyroscopic effects
    if (data.prop && data.prop->rotation != PropRotation::Contra) {
        float sign = (data.prop->rotation == PropRotation::CW) ? -1.f : 1.f;
        roll_moment += sign * data.prop->torque_factor * thrust_n;
        // Gyroscopic: pitching generates yaw (handled in yaw moment below)
    }

    // Yaw moment (positive = nose right). cn_da (#899) is adverse yaw — aileron deflection yaws
    // against the commanded roll; 0 by default.
    float cn_coeff = data.moments.cn_beta * beta_rad + cn_r_eff * (r_rad_s * span * inv_2v) +
                     data.moments.cn_dr * rudder_rad + data.moments.cn_da * ail_rad;
    float yaw_moment = q_dyn * S * span * cn_coeff;

    // Prop gyroscopic: pitching nose-up (positive q) creates right yaw for CW prop
    if (data.prop && data.prop->rotation == PropRotation::CW)
        yaw_moment += data.prop->gyro_factor * thrust_n * q_rad_s;
    else if (data.prop && data.prop->rotation == PropRotation::CCW)
        yaw_moment -= data.prop->gyro_factor * thrust_n * q_rad_s;

    return {roll_moment, pitch_moment, yaw_moment};
}

} // namespace fl
