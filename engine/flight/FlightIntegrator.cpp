// SPDX-License-Identifier: GPL-3.0-or-later
#include "flight/FlightIntegrator.h"

#include "flight/Atmosphere.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace fl {

namespace {

constexpr float kG = 9.80665f;
constexpr float kDegToRad = static_cast<float>(std::numbers::pi) / 180.f;

// Quaternion: multiply q = (x,y,z,w)
std::array<float, 4> quatMul(const float* a, const float* b) {
    return {
        a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
        a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
        a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
        a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2],
    };
}

// Normalise quaternion in-place.
void quatNorm(float* q) {
    float len = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (len > 1e-6f) {
        q[0] /= len;
        q[1] /= len;
        q[2] /= len;
        q[3] /= len;
    }
}

// Rotate vector v by quaternion q.
std::array<float, 3> quatRotate(const float* q, const float* v) {
    // q * [v, 0] * q^{-1}
    float qx = q[0], qy = q[1], qz = q[2], qw = q[3];
    float vx = v[0], vy = v[1], vz = v[2];
    float tx = 2.f * (qy * vz - qz * vy);
    float ty = 2.f * (qz * vx - qx * vz);
    float tz = 2.f * (qx * vy - qy * vx);
    return {vx + qw * tx + qy * tz - qz * ty, vy + qw * ty + qz * tx - qx * tz, vz + qw * tz + qx * ty - qy * tx};
}

// Euler angles (roll=x, pitch=y, yaw=z) from quaternion (ZYX convention).
std::array<float, 3> quatToEuler(const float* q) {
    float sinr_cosp = 2.f * (q[3] * q[0] + q[1] * q[2]);
    float cosr_cosp = 1.f - 2.f * (q[0] * q[0] + q[1] * q[1]);
    float roll = std::atan2(sinr_cosp, cosr_cosp);

    float sinp = 2.f * (q[3] * q[1] - q[2] * q[0]);
    float pitch = std::abs(sinp) >= 1.f ? std::copysign(std::numbers::pi_v<float> / 2.f, sinp) : std::asin(sinp);

    float siny_cosp = 2.f * (q[3] * q[2] + q[0] * q[1]);
    float cosy_cosp = 1.f - 2.f * (q[1] * q[1] + q[2] * q[2]);
    float yaw = std::atan2(siny_cosp, cosy_cosp);

    return {roll, pitch, yaw};
}

} // namespace

FlightIntegrator::FlightIntegrator(std::shared_ptr<const FlightModelData> data) : m_data(std::move(data)) {
    m_state.mass_kg = m_data->geometry.mass_kg + m_data->geometry.fuel_kg;
    m_state.fuel_kg = m_data->geometry.fuel_kg;
    m_state.current_sweep_deg = m_data->wing_sweep ? m_data->wing_sweep->ref_sweep_deg : 0.f;
}

void FlightIntegrator::reset(const FlightState& state) {
    m_state = state;
}

void FlightIntegrator::advanceSpool(float dt, float commanded) {
    float& actual = m_state.throttle_actual;
    const float spool = m_data->engine.spool_time_s;
    if (spool <= 0.f) {
        actual = commanded;
    } else {
        actual += (commanded - actual) / spool * dt;
    }
    actual = std::clamp(actual, 0.f, 1.f);
}

void FlightIntegrator::advanceSweep(float dt, float commanded_deg) {
    if (!m_data->wing_sweep)
        return;
    const auto& ws = *m_data->wing_sweep;
    float& current = m_state.current_sweep_deg;
    float delta = commanded_deg - current;
    float step = ws.slew_rate_deg_s * dt;
    if (std::abs(delta) <= step)
        current = commanded_deg;
    else
        current += std::copysign(step, delta);
    current = std::clamp(current, ws.min_deg, ws.max_deg);
}

void FlightIntegrator::advanceTvc(float dt, float commanded_deg) {
    if (!m_data->tvc)
        return;
    const auto& tvc = *m_data->tvc;
    float& angle = m_state.tvc_angle_deg;
    float delta = commanded_deg - angle;
    float step = tvc.slew_rate_deg_s * dt;
    if (std::abs(delta) <= step)
        angle = commanded_deg;
    else
        angle += std::copysign(step, delta);
    angle = std::clamp(angle, tvc.min_angle_deg, tvc.max_angle_deg);
}

void FlightIntegrator::integrateRotation(float dt) {
    // Integrate angular velocity into quaternion via small-angle approximation.
    float p = m_state.omega[0];
    float q = m_state.omega[1];
    float r = m_state.omega[2];
    float dq[4] = {0.5f * p * dt, 0.5f * q * dt, 0.5f * r * dt, 1.f};
    auto q_new = quatMul(m_state.quat, dq);
    for (int i = 0; i < 4; ++i)
        m_state.quat[i] = q_new[i];
    quatNorm(m_state.quat);
    auto euler = quatToEuler(m_state.quat);
    m_state.euler[0] = euler[0];
    m_state.euler[1] = euler[1];
    m_state.euler[2] = euler[2];
}

void FlightIntegrator::step(float dt, const ControlInput& ctrl, const PayloadEffect& payload,
                            const WindInfluence& wind) {
    // 1. Spool and optional gear/control surfaces
    advanceSpool(dt, ctrl.throttle);

    // 2. Wing sweep: follow auto-schedule based on current Mach, or manual override
    if (m_data->wing_sweep) {
        const float* vel = m_state.vel_body;
        float spd = std::sqrt(vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2]);
        AtmosphereState atmos = computeAtmosphere(m_state.pos_world[1]);
        float mach = (atmos.speed_of_sound_m_s > 0.f) ? spd / atmos.speed_of_sound_m_s : 0.f;
        float sched_sweep = m_data->wing_sweep->schedule.lookup(mach);
        advanceSweep(dt, sched_sweep);
    }

    // 3. TVC
    advanceTvc(dt, ctrl.tvc_angle_deg);

    // 4. Compute atmosphere at current altitude (world y = altitude, Y-up convention)
    float altitude_m = m_state.pos_world[1];
    AtmosphereState atmos = computeAtmosphere(altitude_m);

    // 5. Airspeed and aerodynamic angles
    const float* vel = m_state.vel_body;
    float spd = std::sqrt(vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2]);
    float mach = (atmos.speed_of_sound_m_s > 0.f) ? spd / atmos.speed_of_sound_m_s : 0.f;

    // Body frame: x=forward, y=up, z=right.
    // Pitched up → velocity dips below body nose → negative body-y component → positive alpha.
    // Sideslip right → positive body-z component → positive beta.
    float alpha_rad = (spd > 0.f) ? std::atan2(-vel[1], vel[0]) : 0.f;
    float beta_rad = (spd > 0.f) ? std::asin(std::clamp(vel[2] / spd, -1.f, 1.f)) : 0.f;

    // 6. Effective mass including payload
    float eff_mass = m_state.mass_kg + payload.extra_mass_kg;
    if (eff_mass < 1.f)
        eff_mass = 1.f; // safety clamp

    // 7. Forces in body frame
    auto forces = computeForces(alpha_rad, beta_rad, mach, spd, altitude_m, m_state.current_sweep_deg,
                                m_state.ab_engaged, m_state.throttle_actual, ctrl, payload, *m_data, atmos);

    // 8. Gravity in body frame. World convention: x=forward, y=up, z=right.
    // Gravity acts downward = -world_y. Transform to body frame via conjugate quaternion.
    const float grav_world[3] = {0.f, -kG, 0.f}; // world: x=forward, y=up, z=right
    // Conjugate quaternion for world→body rotation
    float q_conj[4] = {-m_state.quat[0], -m_state.quat[1], -m_state.quat[2], m_state.quat[3]};
    auto grav_body = quatRotate(q_conj, grav_world);

    forces[0] += eff_mass * grav_body[0];
    forces[1] += eff_mass * grav_body[1];
    forces[2] += eff_mass * grav_body[2];

    // 8b. Wind and turbulence perturbations.
    // Turbulence: stochastic body-frame impulse (F = m*a, treating as acceleration).
    forces[0] += eff_mass * wind.turbulence_body[0];
    forces[1] += eff_mass * wind.turbulence_body[1];
    forces[2] += eff_mass * wind.turbulence_body[2];
    // Steady wind + gusts: rotate world-frame wind into body frame, add drag contribution.
    // Simplified linear model — full airspeed correction is a follow-on (see issue tracker).
    if (wind.wind_world[0] != 0.f || wind.wind_world[2] != 0.f) {
        auto wind_body = quatRotate(q_conj, wind.wind_world);
        float S = m_data->geometry.wing_area_m2;
        float CD = 0.03f; // approximate parasitic drag coefficient
        forces[0] += 0.5f * atmos.density_kg_m3 * S * CD * std::abs(wind_body[0]) * (wind_body[0] > 0.f ? 1.f : -1.f);
        forces[1] += 0.5f * atmos.density_kg_m3 * S * CD * std::abs(wind_body[1]) * (wind_body[1] > 0.f ? 1.f : -1.f);
        forces[2] += 0.5f * atmos.density_kg_m3 * S * CD * std::abs(wind_body[2]) * (wind_body[2] > 0.f ? 1.f : -1.f);
    }

    // 9. Thrust magnitude for TVC moment and prop effects
    float alt_km = altitude_m / 1000.f;
    float mil_kn = m_data->engine.mil_thrust.lookup(mach, alt_km);
    float thrust_n = 0.f;
    if (m_state.ab_engaged && m_data->engine.ab_thrust)
        thrust_n = m_data->engine.ab_thrust->lookup(mach, alt_km) * 1000.f;
    else
        thrust_n = m_state.throttle_actual * mil_kn * 1000.f;

    // 10. Moments in body frame.
    // omega[0]=roll(X), omega[1]=yaw(Y=up), omega[2]=pitch(Z=right).
    // computeMoments expects (p=roll, q=pitch, r=yaw).
    auto moments = computeMoments(alpha_rad, beta_rad, m_state.omega[0], m_state.omega[2], m_state.omega[1], spd,
                                  thrust_n, m_state.tvc_angle_deg * kDegToRad, ctrl, *m_data, atmos);

    // 11. Semi-implicit Euler: angular velocity.
    // moments = {roll, pitch, yaw}; omega = {roll(X), yaw(Y), pitch(Z)}.
    float Ixx = m_data->geometry.ixx_kg_m2;
    float Iyy = m_data->geometry.iyy_kg_m2;
    float Izz = m_data->geometry.izz_kg_m2;
    m_state.omega[0] += (moments[0] / Ixx) * dt; // roll  (omega[0] = around X=fwd)
    m_state.omega[2] += (moments[1] / Iyy) * dt; // pitch (omega[2] = around Z=right)
    m_state.omega[1] += (moments[2] / Izz) * dt; // yaw   (omega[1] = around Y=up)

    // 12. Semi-implicit Euler: translational velocity
    m_state.vel_body[0] += (forces[0] / eff_mass) * dt;
    m_state.vel_body[1] += (forces[1] / eff_mass) * dt;
    m_state.vel_body[2] += (forces[2] / eff_mass) * dt;

    // 13. Integrate rotation quaternion
    integrateRotation(dt);

    // 14. Update world position: rotate body velocity to world frame
    auto vel_world = quatRotate(m_state.quat, m_state.vel_body);
    m_state.pos_world[0] += vel_world[0] * dt;
    m_state.pos_world[1] += vel_world[1] * dt;
    m_state.pos_world[2] += vel_world[2] * dt;

    // 15. Fuel burn
    float flow;
    if (m_state.ab_engaged && m_data->engine.ab_thrust)
        flow = m_data->engine.fuel_flow_ab_kg_s;
    else
        flow = m_data->engine.fuel_flow_idle_kg_s +
               m_state.throttle_actual * (m_data->engine.fuel_flow_mil_kg_s - m_data->engine.fuel_flow_idle_kg_s);

    float burned = flow * dt;
    m_state.fuel_kg = std::max(0.f, m_state.fuel_kg - burned);
    m_state.mass_kg = m_data->geometry.mass_kg + m_state.fuel_kg;
}

} // namespace fl
