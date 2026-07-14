// SPDX-License-Identifier: GPL-3.0-or-later
#include "flight/FlightIntegrator.h"

#include "flight/Atmosphere.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace fl {

namespace {

constexpr float kDegToRad = 0.0174532925f;
constexpr float kG0 = 9.80665f; // standard gravity, for expressing load factor in g

// [aero.limits] enforcement (#816). Exceeding the structural limit by more than kOverGMargin for
// longer than kOverGSeconds damages the airframe. The margin exists because a momentary spike from
// a gust or a single-tick control input is not what breaks an aeroplane — sustained overstress is.
constexpr float kOverGMargin = 0.10f;  // 10% past the limit before it counts
constexpr float kOverGSeconds = 0.50f; // ...held for this long before the airframe pays for it

// The FBW limiter starts easing off aft stick slightly BELOW the structural limit, so the aircraft
// settles at the limit instead of overshooting it and then being dragged back. A real flight computer
// does the same thing; a limiter that only reacts once the wing is already overstressed is not a limiter.
constexpr float kFbwGuardBand = 0.95f;
constexpr float kFbwAoaKp = 0.60f; // elevator per degree of AoA error
constexpr float kFbwAoaKd = 1.20f; // elevator per rad/s of pitch rate (damps the approach)

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

// Double-precision rotation: float quaternion q rotates double vector v.
// Used for the vel_body → pos_world position update so double velocity precision is
// not truncated to float before accumulation into the double pos_world fields.
std::array<double, 3> quatRotateD(const float* q, const double* v) {
    double qx = q[0], qy = q[1], qz = q[2], qw = q[3];
    double vx = v[0], vy = v[1], vz = v[2];
    double tx = 2.0 * (qy * vz - qz * vy);
    double ty = 2.0 * (qz * vx - qx * vz);
    double tz = 2.0 * (qx * vy - qy * vx);
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

void FlightIntegrator::step(float dt, const ControlInput& ctrlIn, const PayloadEffect& payload,
                            const WindInfluence& wind, float groundElev) {
    // Progressive damage penalties (#626): DamageDef's thrustFactor scales the throttle COMMAND and
    // controlFactor scales the surface deflection COMMANDS, applied once here so every downstream
    // consumer (spool, FBW reference, parking brake, force model) sees the degraded inputs. This is
    // a command-authority model — a shot-up engine cannot be asked for full power, shot-up linkages
    // cannot be asked for full deflection — which is the honest granularity for a 3-level global
    // damage model; per-subsystem effects layer on top later (#675).
    ControlInput ctrl = ctrlIn;
    ctrl.throttle *= m_damageThrust;
    ctrl.elevator *= m_damageControl;
    ctrl.aileron *= m_damageControl;
    ctrl.rudder *= m_damageControl;

    // Clear the previous tick's one-shot outputs.
    m_state.ground_impact_speed = 0.f;

    // Ground contact (evaluated from the start-of-step position). While the gear carries the
    // aircraft, steady wind and turbulence do not blow it around (aero is computed from ground
    // velocity only — see steps 2/5/8b) and a parked/slow aircraft is held by static ground
    // friction (step 14c). Without this a stationary entity slides downwind whenever the weather
    // changes, because the relative-airspeed model turns steady wind into aerodynamic drag.
    constexpr float kGroundContactMarginM = 0.5f;
    // Radial ground contact: compare the aircraft's geodetic (MSL) altitude against the terrain
    // elevation above the datum (groundElev), so contact is correct anywhere on the planet — not
    // just where world-Y aliases altitude near the origin (#477). Reduces exactly to the old planar
    // test at the world origin, where geodeticAltitude == pos_world[1] and the local up is world +Y.
    const double startAlt = m_gravity->geodeticAltitude(m_state.pos_world);
    const bool inGroundContact = (startAlt - static_cast<double>(groundElev)) <= kGroundContactMarginM;

    // 1. Spool and optional gear/control surfaces
    advanceSpool(dt, ctrl.throttle);
    m_state.ab_engaged = ctrl.afterburner && m_data->engine.ab_thrust.has_value();

    // 2. Wing sweep: follow auto-schedule based on current Mach, or manual override.
    // Use relative airspeed (aircraft velocity minus body-frame wind) for Mach — consistent with step 5.
    if (m_data->wing_sweep) {
        AtmosphereState atmos2 = computeAtmosphere(static_cast<float>(m_gravity->geodeticAltitude(m_state.pos_world)));
        float q_conj2[4] = {-m_state.quat[0], -m_state.quat[1], -m_state.quat[2], m_state.quat[3]};
        float vel_f_sweep[3] = {float(m_state.vel_body[0]), float(m_state.vel_body[1]), float(m_state.vel_body[2])};
        const float* vel = vel_f_sweep;
        std::array<float, 3> wind_body2{};
        if (!inGroundContact)
            wind_body2 = quatRotate(q_conj2, wind.wind_world);
        float rel0 = vel[0] - wind_body2[0];
        float rel1 = vel[1] - wind_body2[1];
        float rel2 = vel[2] - wind_body2[2];
        float spd = std::sqrt(rel0 * rel0 + rel1 * rel1 + rel2 * rel2);
        float mach = (atmos2.speed_of_sound_m_s > 0.f) ? spd / atmos2.speed_of_sound_m_s : 0.f;
        float sched_sweep = m_data->wing_sweep->schedule.lookup(mach);
        advanceSweep(dt, sched_sweep);
    }

    // 3. TVC
    advanceTvc(dt, ctrl.tvc_angle_deg);

    // 4. Compute atmosphere at current geodetic altitude (uses gravity field for spherical planets).
    float altitude_m = static_cast<float>(m_gravity->geodeticAltitude(m_state.pos_world));
    AtmosphereState atmos = computeAtmosphere(altitude_m);

    // Conjugate quaternion for world→body rotation (used for wind and gravity transforms).
    float q_conj[4] = {-m_state.quat[0], -m_state.quat[1], -m_state.quat[2], m_state.quat[3]};

    // 5. Relative airspeed: subtract body-frame wind from aircraft velocity.
    // Aerodynamic forces depend on velocity relative to the air mass, not the ground.
    float vel_f[3] = {float(m_state.vel_body[0]), float(m_state.vel_body[1]), float(m_state.vel_body[2])};
    const float* vel = vel_f;
    std::array<float, 3> wind_body{};
    if (!inGroundContact)
        wind_body = quatRotate(q_conj, wind.wind_world);
    float rel0 = vel[0] - wind_body[0];
    float rel1 = vel[1] - wind_body[1];
    float rel2 = vel[2] - wind_body[2];
    float spd = std::sqrt(rel0 * rel0 + rel1 * rel1 + rel2 * rel2);
    float mach = (atmos.speed_of_sound_m_s > 0.f) ? spd / atmos.speed_of_sound_m_s : 0.f;

    // Body frame: x=forward, y=up, z=right.
    // Pitched up → velocity dips below body nose → negative body-y component → positive alpha.
    // Sideslip right → positive body-z component → positive beta.
    float alpha_rad = (spd > 0.f) ? std::atan2(-rel1, rel0) : 0.f;
    float beta_rad = (spd > 0.f) ? std::asin(std::clamp(rel2 / spd, -1.f, 1.f)) : 0.f;

    // 6. Effective mass including payload
    float eff_mass = m_state.mass_kg + payload.extra_mass_kg;
    if (eff_mass < 1.f)
        eff_mass = 1.f; // safety clamp

    // 6b. Stall flag (#816). alpha_stall_deg is now load-bearing: it says where THIS aeroplane departs.
    // Deliberately only a FLAG. The CL collapse past the stall belongs in the author's cl_table (and
    // the validator now checks that the table's peak agrees with this angle); clamping CL here on top
    // of an honest table would double-count the stall, and would reward an author who wrote a
    // dishonest one. The consequences — buffet, HUD, audio — are the caller's, not the integrator's.
    const float alpha_deg_now = alpha_rad / kDegToRad;
    const float q_dyn_now = 0.5f * atmos.density_kg_m3 * spd * spd;
    m_state.stalled = (alpha_deg_now > m_data->limits.alpha_stall_deg);

    // 7. Aerodynamic + propulsive forces and moments via the swappable force model (default
    // FixedWingForceModel). Gravity and turbulence are added below by the integrator core.
    const AeroInputs aero{alpha_rad, beta_rad, mach, spd, altitude_m};
    ControlInput eff_ctrl = ctrl;

    // 7a. G-LIMITER (#816) — and this is the ONLY thing has_fbw should ever mean.
    //
    // A fly-by-wire aircraft's flight computer will not let the pilot overstress the airframe. A 1972
    // airframe with cables and pushrods has no such opinion. Applying a limiter to both would erase
    // the difference the content pack exists to express: an F-16 cannot be pulled past 9 g, and an
    // F-5E absolutely can be pulled past 7.33 -- and then it breaks, which is the next block.
    //
    // It limits ANGLE OF ATTACK, not measured g -- which is what a real flight computer does, and it
    // is the only version that works. Elevator does not change lift on the tick it is applied: it
    // changes the pitch RATE, which changes AoA, which changes lift. A limiter that waits for the
    // G-meter to read high is always a tick late, and on an agile airframe a tick is worth several g
    // (a purely reactive version of this loop let a test model spike to 23 g on an 8 g airframe).
    //
    // So: find the AoA that would produce exactly max_g_structural at the current dynamic pressure,
    // and hold the aircraft there with a proportional-derivative loop on AoA and pitch rate. The
    // pilot's aft stick is honoured right up to that boundary and refused past it.
    if (m_data->meta.has_fbw && ctrl.elevator > 0.f && m_data->limits.max_g_structural > 0.f && q_dyn_now > 1.f) {
        const float S = m_data->geometry.wing_area_m2;
        const float clLimit = (m_data->limits.max_g_structural * eff_mass * kG0) / (q_dyn_now * S);

        // Invert the CL curve by bisection over [0, alpha_stall], where it is monotonic. If the wing
        // cannot make clLimit at all, the aircraft is lift-limited rather than structure-limited and
        // there is nothing for the limiter to do.
        const float alphaStall = m_data->limits.alpha_stall_deg;
        if (m_data->cl_table.lookup(alphaStall, mach) > clLimit) {
            float lo = 0.f, hi = alphaStall;
            for (int i = 0; i < 12; ++i) {
                const float mid = 0.5f * (lo + hi);
                if (m_data->cl_table.lookup(mid, mach) < clLimit)
                    lo = mid;
                else
                    hi = mid;
            }
            const float alphaLimit = 0.5f * (lo + hi) * kFbwGuardBand;

            // omega[2] is the pitch rate (about the body's right axis).
            const float cmd = kFbwAoaKp * (alphaLimit - alpha_deg_now) - kFbwAoaKd * m_state.omega[2];
            eff_ctrl.elevator = std::clamp(cmd, -1.f, ctrl.elevator); // never MORE than the pilot asked for
        }
    }

    const ForceMoment fm = m_forceModel->compute(m_state, eff_ctrl, payload, *m_data, atmos, aero);
    auto forces = fm.force_body;

    // 7b. LOAD FACTOR + OVER-G DAMAGE (#816).
    //
    // n is the AERODYNAMIC normal force over weight -- it excludes gravity, which is what a cockpit
    // G-meter reads. Everything needed was already sitting here; nothing computed it.
    //
    // Exceeding the limit by more than kOverGMargin for longer than kOverGSeconds raises a one-shot
    // damage flag. NOT a silent clamp: a limiter on an aeroplane that has none is a lie about the
    // aircraft. An F-5E pilot CAN overstress the jet. The sim should let them, and then bill them.
    m_state.load_factor = forces[1] / (eff_mass * kG0);

    const float n = m_state.load_factor;
    const float nMax = m_data->limits.max_g_structural;
    const float nMin = m_data->limits.min_g_structural;
    const bool overStressed =
        (nMax > 0.f && n > nMax * (1.f + kOverGMargin)) || (nMin < 0.f && n < nMin * (1.f + kOverGMargin));

    m_state.overg_damage = false; // one-shot: cleared every tick, raised only on the tick it fires
    if (overStressed) {
        m_state.overg_seconds += dt;
        if (m_state.overg_seconds >= kOverGSeconds) {
            m_state.overg_damage = true;
            m_state.overg_seconds = 0.f; // re-arm, so a sustained overstress keeps costing the airframe
        }
    } else {
        m_state.overg_seconds = 0.f;
    }

    // 8. Gravity in body frame. World convention: x=forward, y=up, z=right.
    // Queried from the gravity field (default: uniform -world_y), transformed to body frame via the
    // conjugate quaternion.
    const std::array<float, 3> grav_world = m_gravity->accelWorld(m_state.pos_world);
    auto grav_body = quatRotate(q_conj, grav_world.data());

    forces[0] += eff_mass * grav_body[0];
    forces[1] += eff_mass * grav_body[1];
    forces[2] += eff_mass * grav_body[2];

    // 8b. Turbulence: stochastic body-frame impulse (F = m*a, treating as acceleration).
    // Steady wind is already accounted for via relative airspeed in step 5. Skipped while in
    // ground contact — the gear absorbs gusts rather than letting them shove a parked aircraft.
    if (!inGroundContact) {
        forces[0] += eff_mass * wind.turbulence_body[0];
        forces[1] += eff_mass * wind.turbulence_body[1];
        forces[2] += eff_mass * wind.turbulence_body[2];
    }

    // 9-10. Moments come from the force model (thrust magnitude and TVC are handled inside it).
    const auto& moments = fm.moment_body;

    // 11. Semi-implicit Euler: angular velocity.
    // moments = {roll, pitch, yaw}; omega = {roll(X), yaw(Y), pitch(Z)}.
    float Ixx = m_data->geometry.ixx_kg_m2;
    float Iyy = m_data->geometry.iyy_kg_m2;
    float Izz = m_data->geometry.izz_kg_m2;
    m_state.omega[0] += (moments[0] / Ixx) * dt; // roll  (omega[0] = around X=fwd)
    m_state.omega[2] += (moments[1] / Iyy) * dt; // pitch (omega[2] = around Z=right)
    m_state.omega[1] += (moments[2] / Izz) * dt; // yaw   (omega[1] = around Y=up)

    // Clamp angular rates: prevents float overflow when aerodynamic moments are
    // extreme (e.g. 90° AoA freefall).  50 rad/s ≈ 2865°/s — well above any
    // physically reachable rate for the builtin model.
    constexpr float kMaxOmega = 50.f;
    m_state.omega[0] = std::clamp(m_state.omega[0], -kMaxOmega, kMaxOmega);
    m_state.omega[1] = std::clamp(m_state.omega[1], -kMaxOmega, kMaxOmega);
    m_state.omega[2] = std::clamp(m_state.omega[2], -kMaxOmega, kMaxOmega);

    // 12. Semi-implicit Euler: translational velocity.
    //
    // THE TRANSPORT TERM (−ω × v) IS NOT OPTIONAL. The body-frame equation of motion is
    //
    //     v̇_body = F/m − ω × v_body
    //
    // and the second term was missing. Its absence meant the velocity vector rotated WITH the
    // airframe: pitching the nose up simply carried the flight path along with it, so the aircraft
    // developed no angle of attack, generated no extra lift, and could not pull g at all. A test
    // model commanded to full aft stick span 400 deg/s of pitch rate and peaked at 0.6 g.
    //
    // Everything that reads AoA was quietly wrong because of it — turn rate, sustained g, stall
    // entry, and the load factor #816 adds. It is fixed here rather than worked around, because no
    // amount of enforcement on top of a flight model that cannot develop AoA would mean anything.
    //
    // omega is stored in body axes as {x=roll, y=yaw, z=pitch} (x=fwd, y=up, z=right), so this is a
    // plain cross product in that basis.
    const double wx = m_state.omega[0], wy = m_state.omega[1], wz = m_state.omega[2];
    const double vx = m_state.vel_body[0], vy = m_state.vel_body[1], vz = m_state.vel_body[2];
    const double cross_x = wy * vz - wz * vy;
    const double cross_y = wz * vx - wx * vz;
    const double cross_z = wx * vy - wy * vx;

    m_state.vel_body[0] += (forces[0] / eff_mass - cross_x) * dt;
    m_state.vel_body[1] += (forces[1] / eff_mass - cross_y) * dt;
    m_state.vel_body[2] += (forces[2] / eff_mass - cross_z) * dt;

    // NUMERICAL GUARD, NOT FLIGHT PHYSICS (#816). This exists to stop a NaN position or an absurd
    // aero force from overflowing the quaternion integration -- nothing more. It was doing double
    // duty as a top-speed limiter because `max_mach` was dead and a cd_wave table clamps flat above
    // its last breakpoint, so an over-thrusted model would simply accelerate into this wall.
    //
    // An aircraft's real top speed is set by its drag rising to meet its thrust. If a model can
    // exceed its declared max_mach in level flight, the MODEL is wrong, and fm-trim (#817) fails it
    // in CI. The engine does not paper over that with an artificial wall, so this is raised well
    // clear of any flyable regime and left as the overflow backstop it always was.
    constexpr double kMaxBodySpeed = 2000.0; // m/s ≈ Mach 6 at sea level — pure overflow backstop
    m_state.vel_body[0] = std::clamp(m_state.vel_body[0], -kMaxBodySpeed, kMaxBodySpeed);
    m_state.vel_body[1] = std::clamp(m_state.vel_body[1], -kMaxBodySpeed, kMaxBodySpeed);
    m_state.vel_body[2] = std::clamp(m_state.vel_body[2], -kMaxBodySpeed, kMaxBodySpeed);

    // 13. Integrate rotation quaternion
    integrateRotation(dt);

    // 14. Update world position: rotate body velocity to world frame.
    // quatRotateD preserves double precision throughout — float vel would truncate before
    // accumulation into pos_world and degrade ICBM-range trajectory accuracy.
    auto vel_world = quatRotateD(m_state.quat, m_state.vel_body);
    m_state.pos_world[0] += vel_world[0] * double(dt);
    m_state.pos_world[1] += vel_world[1] * double(dt);
    m_state.pos_world[2] += vel_world[2] * double(dt);

    // 14b. Ground collision response (radial).
    // If the entity dropped below the terrain surface this tick, snap it back out along the local
    // radial up and apply an impulse: high-speed impact bounces (coefficient of restitution 0.35),
    // low-speed contact stops the vertical (radial) component; the horizontal remainder decays via
    // friction. "Vertical" is the component along the local up, so this is correct far from the
    // origin; near it the up is world +Y and this reduces exactly to the old planar clamp.
    const double altPost = m_gravity->geodeticAltitude(m_state.pos_world);
    if (altPost < static_cast<double>(groundElev)) {
        const std::array<float, 3> up = m_gravity->geodeticUp(m_state.pos_world);
        const double penetration = static_cast<double>(groundElev) - altPost;
        m_state.pos_world[0] += static_cast<double>(up[0]) * penetration;
        m_state.pos_world[1] += static_cast<double>(up[1]) * penetration;
        m_state.pos_world[2] += static_cast<double>(up[2]) * penetration;
        const float vUp = float(vel_world[0]) * up[0] + float(vel_world[1]) * up[1] + float(vel_world[2]) * up[2];
        if (vUp < 0.f) {
            constexpr float kCoR = 0.35f;         // coefficient of restitution
            constexpr float kSlideImpact = 0.80f; // hard-landing friction (≥10 m/s vertical)
            constexpr float kSlideRoll = 0.999f;  // ground-roll friction (near-zero vertical)
            const float impactSpd = std::abs(vUp);
            // Crash-damage report (#626): a firm landing is ~3 m/s of sink; anything past this
            // threshold is an arrival, not a landing. One-shot, consumed serially by the caller —
            // same discipline as overg_damage.
            constexpr float kCrashReportThresholdMps = 6.f;
            if (impactSpd >= kCrashReportThresholdMps)
                m_state.ground_impact_speed = impactSpd;
            // Scale friction by impact severity so gravity's ~0.16 m/s/frame floor-tickle
            // does not act as a continuous brake during ground roll.
            const float kSlide = kSlideRoll + (kSlideImpact - kSlideRoll) * std::min(impactSpd / 10.f, 1.f);
            const float newVUp = (impactSpd < 2.f) ? 0.f : -vUp * kCoR;
            std::array<float, 3> vw = {float(vel_world[0]), float(vel_world[1]), float(vel_world[2])};
            // Decompose into radial (vertical) + horizontal, reflect/stop the radial part, apply
            // ground friction to the horizontal remainder, then recombine.
            std::array<float, 3> horiz = {vw[0] - vUp * up[0], vw[1] - vUp * up[1], vw[2] - vUp * up[2]};
            horiz[0] *= kSlide;
            horiz[1] *= kSlide;
            horiz[2] *= kSlide;
            vw[0] = horiz[0] + newVUp * up[0];
            vw[1] = horiz[1] + newVUp * up[1];
            vw[2] = horiz[2] + newVUp * up[2];
            // Attenuate angular rates on impact to prevent post-contact spinning.
            m_state.omega[0] *= 0.5f;
            m_state.omega[1] *= 0.5f;
            m_state.omega[2] *= 0.5f;
            // Rotate corrected world velocity back to body frame.
            float q_c[4] = {-m_state.quat[0], -m_state.quat[1], -m_state.quat[2], m_state.quat[3]};
            auto vb = quatRotate(q_c, vw.data());
            m_state.vel_body[0] = vb[0];
            m_state.vel_body[1] = vb[1];
            m_state.vel_body[2] = vb[2];
        }
    }

    // 14c. Static ground friction (parking brake). A stationary aircraft on the ground is held
    // by its gear/brakes and must not creep under residual forcing (gravity tickle, numerical
    // drift, a gust the instant before contact). Engages only at very low ground speed and
    // near-idle throttle, so it never interferes with the takeoff roll.
    if ((m_gravity->geodeticAltitude(m_state.pos_world) - static_cast<double>(groundElev)) <= kGroundContactMarginM) {
        constexpr float kParkingSpeedM_s = 1.0f;  // hold below ~1 m/s of horizontal motion
        constexpr float kParkingThrottle = 0.05f; // and only near idle
        const float horizSpd =
            float(std::sqrt(m_state.vel_body[0] * m_state.vel_body[0] + m_state.vel_body[2] * m_state.vel_body[2]));
        if (horizSpd < kParkingSpeedM_s && ctrl.throttle < kParkingThrottle) {
            m_state.vel_body[0] = 0.f; // forward
            m_state.vel_body[2] = 0.f; // right (vertical vel_body[1] left to the impact clamp)
            // Brakes/gear also resist any yaw/roll/pitch creep, so a parked aircraft is fully
            // static (no residual rotation from settling or gusts before contact).
            m_state.omega[0] = 0.f;
            m_state.omega[1] = 0.f;
            m_state.omega[2] = 0.f;
        }
    }

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
