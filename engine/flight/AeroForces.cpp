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

std::array<float, 3> computeForces(float alpha_rad, float beta_rad, float mach, float speed_m_s, float altitude_m,
                                   float current_sweep_deg, bool ab_engaged, float throttle_actual,
                                   const ControlInput& ctrl, const PayloadEffect& payload, const FlightModelData& data,
                                   const AtmosphereState& atmos) {
    (void)beta_rad; // lateral side force from sideslip omitted — handled via moments only
    const float alpha_deg = alpha_rad / kDegToRad;
    const float q_dyn = 0.5f * atmos.density_kg_m3 * speed_m_s * speed_m_s;
    const float S = data.geometry.wing_area_m2;

    // ── Lift ──────────────────────────────────────────────────────────────────
    float cl = data.cl_table.lookup(alpha_deg, mach);

    if (data.wing_sweep) {
        auto sc = sweepCorrection(current_sweep_deg, *data.wing_sweep);
        cl *= sc.cl_scale;
    }

    float lift = q_dyn * S * cl; // N, perpendicular to velocity vector

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

    float cd_device =
        ctrl.speedbrake * data.drag_polar.speedbrake_cd + (ctrl.gear_down ? data.drag_polar.gear_cd : 0.f);

    float cd_total = cd0 + k_eff * cl * cl + cd_wave + cd_device;
    float drag = q_dyn * S * cd_total; // N, opposing velocity

    // ── Thrust ────────────────────────────────────────────────────────────────
    float alt_km = altitude_m / 1000.f;
    float mil_kn = data.engine.mil_thrust.lookup(mach, alt_km);
    float thrust_n = 0.f;

    if (ab_engaged && data.engine.ab_thrust) {
        float ab_kn = data.engine.ab_thrust->lookup(mach, alt_km);
        thrust_n = ab_kn * 1000.f;
    } else {
        thrust_n = throttle_actual * mil_kn * 1000.f;
    }

    // ── Body-frame assembly ───────────────────────────────────────────────────
    // Lift is normal to velocity; drag is anti-velocity.
    // In body frame with small beta assumption:
    //   x (forward): thrust − drag*cos(alpha) + lift*sin(alpha)
    //   y (right):   0 (neglecting side force from beta; handled via moments)
    //   z (down):    −lift*cos(alpha) − drag*sin(alpha) + weight component (gravity added by integrator)
    float cos_a = std::cos(alpha_rad);
    float sin_a = std::sin(alpha_rad);

    std::array<float, 3> forces{};
    forces[0] = thrust_n - drag * cos_a + lift * sin_a; // x: forward
    forces[1] = 0.f;                                    // y: right (side force omitted)
    forces[2] = -(lift * cos_a + drag * sin_a);         // z: down (lift acts upward = negative z)

    return forces;
}

std::array<float, 3> computeMoments(float alpha_rad, float beta_rad, float p_rad_s, float q_rad_s, float r_rad_s,
                                    float speed_m_s, float thrust_n, float tvc_angle_rad, const ControlInput& ctrl,
                                    const FlightModelData& data, const AtmosphereState& atmos) {
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
    const float elev_rad = -ctrl.elevator * data.controls.max_elevator_deg * kDegToRad;
    const float ail_rad = ctrl.aileron * data.controls.max_aileron_deg * kDegToRad;
    const float rudder_rad = -ctrl.rudder * data.controls.max_rudder_deg * kDegToRad;

    // Pitch moment (positive = nose up)
    float cm = data.moments.cm_alpha * alpha_rad + data.moments.cm_q * (q_rad_s * mac * inv_2v) +
               data.moments.cm_de * elev_rad;
    float pitch_moment = q_dyn * S * mac * cm;

    // TVC pitch contribution: moment arm from CG assumed ≈ 0 (moment = thrust × sin(tvc))
    if (data.tvc)
        pitch_moment += thrust_n * std::sin(tvc_angle_rad);

    // Roll moment (positive = right wing down)
    float cl_coeff =
        data.moments.cl_beta * beta_rad + data.moments.cl_p * (p_rad_s * span * inv_2v) + data.moments.cl_da * ail_rad;
    float roll_moment = q_dyn * S * span * cl_coeff;

    // Prop torque and gyroscopic effects
    if (data.prop && data.prop->rotation != PropRotation::Contra) {
        float sign = (data.prop->rotation == PropRotation::CW) ? -1.f : 1.f;
        roll_moment += sign * data.prop->torque_factor * thrust_n;
        // Gyroscopic: pitching generates yaw (handled in yaw moment below)
    }

    // Yaw moment (positive = nose right)
    float cn_coeff = data.moments.cn_beta * beta_rad + data.moments.cn_r * (r_rad_s * span * inv_2v) +
                     data.moments.cn_dr * rudder_rad;
    float yaw_moment = q_dyn * S * span * cn_coeff;

    // Prop gyroscopic: pitching nose-up (positive q) creates right yaw for CW prop
    if (data.prop && data.prop->rotation == PropRotation::CW)
        yaw_moment += data.prop->gyro_factor * thrust_n * q_rad_s;
    else if (data.prop && data.prop->rotation == PropRotation::CCW)
        yaw_moment -= data.prop->gyro_factor * thrust_n * q_rad_s;

    return {roll_moment, pitch_moment, yaw_moment};
}

} // namespace fl
