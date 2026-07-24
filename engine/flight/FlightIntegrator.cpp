// SPDX-License-Identifier: GPL-3.0-or-later
#include "flight/FlightIntegrator.h"

#include "flight/Atmosphere.h"
#include "flight/EngineFailFlags.h" // kEngineFlameout / kEngineCompStall — the #308 transient bits

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

// Afterburner-envelope hysteresis (#309): once lit, AB tolerates crossing this far past the Mach /
// altitude limit before it drops; once out, it must be this far inside before it relights. Prevents
// chatter for a model flying the boundary.
constexpr float kAbMachHysteresis = 0.02f;
constexpr float kAbAltHysteresisKm = 0.3f;

// Engine failure dynamics (#308). A flameout above the combustion ceiling must descend this far
// back below it before a relight is attempted (prevents chatter riding the ceiling); a compressor
// surge holds kEngineCompStall for this long after the disturbed-flow condition clears (a surge is
// a violent, seconds-scale event, not a single-tick blip).
constexpr float kRelightAltMarginKm = 0.5f;
constexpr float kCompStallRecoverySeconds = 2.0f;
constexpr float kSurgeMinThrottle = 0.5f; // a windmilling/idle compressor does not surge

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

// Slew every actuator toward its command (#842). Shares the pure advanceArticulation with
// ClientPrediction's replay by construction: if the two sides ever disagreed about where the gear is,
// they would disagree about where the aircraft is, because gear position is drag.
void FlightIntegrator::advanceArticulationState(float dt, const ControlInput& ctrl) {
    ArticulationCommand cmd{};
    cmd.gear_down = ctrl.gear_down;
    cmd.flaps = ctrl.flaps;
    cmd.speedbrake = ctrl.speedbrake;
    cmd.hook_down = ctrl.hook_down;
    cmd.canopy_open = ctrl.canopy_open;
    advanceArticulation(m_state.articulation, cmd, m_data->articulation, dt);
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
                            const WindInfluence& wind, float groundElev, const GroundFriction& ground) {
    // Progressive damage penalties (#626): DamageDef's thrustFactor scales the throttle COMMAND and
    // controlFactor scales the surface deflection COMMANDS, applied once here so every downstream
    // consumer (spool, FBW reference, parking brake, force model) sees the degraded inputs. This is
    // a command-authority model — a shot-up engine cannot be asked for full power, shot-up linkages
    // cannot be asked for full deflection — which is the honest granularity for a 3-level global
    // damage model; per-subsystem effects layer on top later (#675).
    // The tier control factor (#626) and the per-subsystem control factor (#675 — controls +
    // hydraulics losses) multiply: two independent ways to lose deflection authority.
    const float controlFactor = m_damageControl * m_subsystemControl;
    ControlInput ctrl = ctrlIn;
    ctrl.throttle *= m_damageThrust;
    ctrl.elevator *= controlFactor;
    ctrl.aileron *= controlFactor;
    ctrl.rudder *= controlFactor;

    // Drone autopilot airspeed protection (#351): shapes the THROTTLE COMMAND from the previous
    // tick's measured airspeed — an autopilot reacts to its sensors, and one tick of latency is
    // more honest than clairvoyance. Overspeed sheds power; underspeed firewalls it (through the
    // damage-limited authority, like any command). The spool provides the smoothing.
    if (const auto& dl = m_data->drone_limits) {
        const double pvx = m_state.vel_body[0], pvy = m_state.vel_body[1], pvz = m_state.vel_body[2];
        const float prevSpd = static_cast<float>(std::sqrt(pvx * pvx + pvy * pvy + pvz * pvz));
        if (dl->max_airspeed_mps > 0.f && prevSpd > dl->max_airspeed_mps)
            ctrl.throttle = 0.f;
        else if (dl->min_airspeed_mps > 0.f && prevSpd < dl->min_airspeed_mps)
            ctrl.throttle = m_damageThrust;
    }

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
    // AB is COMMANDED here; the envelope gate (#309) below can only turn it off, once Mach/altitude
    // are known. Capture the prior tick's state so that gate's hysteresis has a reference.
    const bool abEngagedPrev = m_state.ab_engaged;
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
        float mach = machNumber(spd, atmos2.speed_of_sound_m_s);
        float sched_sweep = m_data->wing_sweep->schedule.lookup(mach);
        advanceSweep(dt, sched_sweep);
    }

    // 3. TVC
    advanceTvc(dt, ctrl.tvc_angle_deg);

    // 3b. Actuator transit (#842): gear, flaps, speed-brake, hook and canopy slew toward their
    // commands BEFORE the aero model runs, so this tick's drag reflects this tick's positions.
    advanceArticulationState(dt, ctrl);

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
    float mach = machNumber(spd, atmos.speed_of_sound_m_s);

    // Afterburner envelope (#309): outside the light-off window the augmentor extinguishes. Applied
    // here, after Mach/altitude are known, so it can only turn a COMMANDED AB off (set in step 1).
    // Each limit is optional — absent means that gate is skipped, so a model without envelope data is
    // bit-identical. The hysteresis is keyed off abEngagedPrev: a lit AB tolerates a margin past the
    // limit before dropping; an unlit one must be clearly inside before relighting.
    if (m_state.ab_engaged) {
        const EngineData& eng = m_data->engine;
        if (eng.ab_min_mach) {
            const float gate = *eng.ab_min_mach + (abEngagedPrev ? -kAbMachHysteresis : kAbMachHysteresis);
            if (mach < gate)
                m_state.ab_engaged = false;
        }
        if (m_state.ab_engaged && eng.ab_max_alt_km) {
            const float gate = *eng.ab_max_alt_km + (abEngagedPrev ? kAbAltHysteresisKm : -kAbAltHysteresisKm);
            if (altitude_m / 1000.f > gate)
                m_state.ab_engaged = false;
        }
    }

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

    // 6c. ENGINE FAILURE DYNAMICS (#308). The integrator raises and clears the two TRANSIENT
    // engine-fail bits it owns; the damage path owns Generic/Left/Right/Center and is never touched
    // here. Both effects are derived deterministically from the flight state, so the server
    // integrator and the client-prediction replay compute identical flags with nothing new on the
    // wire. Gated off for ballistic vehicles — a solid motor neither flames out nor surges; its
    // burn ends through the fuel path.
    if (!m_data->isBallistic()) {
        const EngineData& eng = m_data->engine;
        uint8_t ef = m_state.engineFailFlags;

        // Flameout: fuel starvation (an engine with nothing to burn makes no thrust — until #308
        // an empty tank changed the mass and nothing else), or climbing past the optional
        // combustion ceiling. Relight is a windmill start: fuel available, clearly back below the
        // ceiling, and enough airspeed to spin the spool. An engine whose model burns NO fuel
        // (zero mil flow — e.g. a vessel whose endurance is not modelled, #38) cannot starve.
        const bool fuelOut = m_state.fuel_kg <= 0.f && eng.fuel_flow_mil_kg_s > 0.f;
        const bool aboveCeiling = eng.flameout_alt_km && (altitude_m / 1000.f > *eng.flameout_alt_km);
        if (fuelOut || aboveCeiling) {
            ef |= kEngineFlameout;
        } else if (ef & kEngineFlameout) {
            const bool altOk =
                !eng.flameout_alt_km || (altitude_m / 1000.f < *eng.flameout_alt_km - kRelightAltMarginKm);
            if (altOk && spd >= eng.relight_min_mps)
                ef &= ~kEngineFlameout;
        }

        // Compressor surge (opt-in): deep past the stall alpha with the compressor working hard,
        // the intake blanks and the engine surges. Holds for a fixed recovery time after the
        // disturbed-flow condition ends — a surge is a seconds-scale event.
        if (eng.compressor_stall) {
            const bool surging = alpha_deg_now > m_data->limits.alpha_stall_deg + eng.surge_alpha_margin_deg &&
                                 m_state.throttle_actual > kSurgeMinThrottle;
            if (surging) {
                ef |= kEngineCompStall;
                m_state.comp_stall_seconds = kCompStallRecoverySeconds;
            } else if (ef & kEngineCompStall) {
                m_state.comp_stall_seconds = std::max(0.f, m_state.comp_stall_seconds - dt);
                if (m_state.comp_stall_seconds <= 0.f)
                    ef &= ~kEngineCompStall;
            }
        }

        m_state.engineFailFlags = ef;
    }

    // 6d. Drone autopilot bank-angle limit (#351): command shaping on the AILERON, mirroring the
    // FBW discipline below — never more than the controller asked for, never a silent clamp on the
    // physics. Bank is read from gravity in the body frame (atan2 of its lateral component), which
    // is planet-correct everywhere with no new plumbing.
    if (const auto& dl = m_data->drone_limits; dl && dl->max_bank_deg > 0.f) {
        const std::array<float, 3> gw = m_gravity->accelWorld(m_state.pos_world);
        const auto gb = quatRotate(q_conj, gw.data());
        const float bankRad = std::atan2(gb[2], -gb[1]); // + = right wing down
        const float maxBank = dl->max_bank_deg * kDegToRad;
        constexpr float kBankKp = 2.0f; // aileron per rad of bank error
        constexpr float kBankKd = 0.8f; // aileron per rad/s of roll rate (damps the approach)
        if (ctrl.aileron > 0.f && bankRad > maxBank * kFbwGuardBand) {
            ctrl.aileron = std::clamp(kBankKp * (maxBank - bankRad) - kBankKd * m_state.omega[0], -1.f, ctrl.aileron);
        } else if (ctrl.aileron < 0.f && bankRad < -maxBank * kFbwGuardBand) {
            ctrl.aileron = std::clamp(kBankKp * (-maxBank - bankRad) - kBankKd * m_state.omega[0], ctrl.aileron, 1.f);
        }
    }

    // 7. Aerodynamic + propulsive forces and moments via the swappable force model (default
    // FixedWingForceModel). Gravity and turbulence are added below by the integrator core.
    // agl_m: rotor ground effect (#350) reads height above terrain; fixed-wing ignores it.
    const AeroInputs aero{alpha_rad, beta_rad,   mach,
                          spd,       altitude_m, static_cast<float>(startAlt - static_cast<double>(groundElev))};
    ControlInput eff_ctrl = ctrl;

    // 7a. FLY-BY-WIRE ENVELOPE PROTECTION (#816, extended #900).
    //
    // has_fbw means the flight computer will not let the pilot leave the structural/AoA envelope. A
    // 1972 airframe with cables and pushrods has no such opinion, so this whole block is gated on
    // has_fbw and NOTHING here touches a non-FBW aircraft. An F-16 cannot be pulled past 9 g; an F-5E
    // absolutely can be pulled past 7.33 -- and then it breaks, which is the next block.
    //
    // It limits ANGLE OF ATTACK, not measured g -- which is what a real flight computer does, and it
    // is the only version that works. Elevator does not change lift on the tick it is applied: it
    // changes the pitch RATE, which changes AoA, which changes lift. A limiter that waits for the
    // G-meter to read high is always a tick late (a purely reactive version let a test model spike to
    // 23 g on an 8 g airframe). So: find the AoA that would produce the g limit at the current dynamic
    // pressure and hold the aircraft there with a PD loop on AoA and pitch rate.
    //
    // #900 broadens the envelope beyond the positive-g cap #816 shipped:
    //   * negative-g protection — forward stick is limited against min_g_structural, just as firmly as
    //     aft stick is against max_g_structural (the F-16's FLCS limits to −3 g as well as +9);
    //   * an optional FLCS AoA cap, alpha_limit_deg, tighter than the aerodynamic stall — at low q the
    //     wing cannot reach the structural g and the AoA cap is what holds the jet.
    // When alpha_limit_deg is unset the positive path is byte-identical to #816.
    //
    // #351: a drone autopilot's max_g runs the SAME AoA-limiting loop — a UAV without FBW still has
    // a computer refusing to bend the airframe, and a second hand-rolled g-limiter would be a second
    // chance to be a tick late. The effective limit is the tighter of structural and autopilot.
    const bool droneGLimited = m_data->drone_limits && m_data->drone_limits->max_g > 0.f;
    if ((m_data->meta.has_fbw || droneGLimited) && q_dyn_now > 1.f && (ctrl.elevator > 0.f || ctrl.elevator < 0.f)) {
        const float S = m_data->geometry.wing_area_m2;
        const float alphaStall = m_data->limits.alpha_stall_deg;
        const float alphaCap = m_data->limits.alpha_limit_deg; // 0 = unset
        const float denom = q_dyn_now * S;
        float nMaxLim = m_data->limits.max_g_structural;
        float nMinLim = m_data->limits.min_g_structural;
        if (droneGLimited) {
            const float g = m_data->drone_limits->max_g;
            nMaxLim = (nMaxLim > 0.f) ? std::min(nMaxLim, g) : g;
            nMinLim = (nMinLim < 0.f) ? std::max(nMinLim, -g) : -g;
        }

        // CL(alpha) is monotonic increasing across [-alpha_stall, +alpha_stall]; bisect for the alpha
        // that makes a target CL (signed) over an interval where it is increasing.
        auto invertCl = [&](float target, float aLo, float aHi) {
            float lo = aLo, hi = aHi;
            for (int i = 0; i < 12; ++i) {
                const float mid = 0.5f * (lo + hi);
                if (m_data->cl_table.lookup(mid, mach) < target)
                    lo = mid;
                else
                    hi = mid;
            }
            return 0.5f * (lo + hi);
        };

        // Resolve the applicable alpha limit for the commanded direction, then hold it with the PD loop.
        // omega[2] is the pitch rate (about the body's right axis).
        bool haveLimit = false;
        float alphaLimit = 0.f;
        if (ctrl.elevator > 0.f) {
            float posLimit = alphaStall;
            if (nMaxLim > 0.f) {
                const float clLimit = (nMaxLim * eff_mass * kG0) / denom;
                if (m_data->cl_table.lookup(alphaStall, mach) > clLimit) { // else lift-limited, nothing to do
                    posLimit = invertCl(clLimit, 0.f, alphaStall);
                    haveLimit = true;
                }
            }
            if (alphaCap > 0.f) {
                posLimit = haveLimit ? std::min(posLimit, alphaCap) : alphaCap;
                haveLimit = true;
            }
            alphaLimit = posLimit * kFbwGuardBand;
        } else { // forward stick — negative-g / negative-AoA protection (#900)
            float negLimit = -alphaStall;
            if (nMinLim < 0.f) {
                const float clLimit = (nMinLim * eff_mass * kG0) / denom;   // negative
                if (m_data->cl_table.lookup(-alphaStall, mach) < clLimit) { // else lift-limited
                    negLimit = invertCl(clLimit, -alphaStall, 0.f);
                    haveLimit = true;
                }
            }
            if (alphaCap > 0.f) {
                negLimit = haveLimit ? std::max(negLimit, -alphaCap) : -alphaCap;
                haveLimit = true;
            }
            alphaLimit = negLimit * kFbwGuardBand; // negative; *0.95 moves toward 0 (more conservative)
        }

        if (haveLimit) {
            const float cmd = kFbwAoaKp * (alphaLimit - alpha_deg_now) - kFbwAoaKd * m_state.omega[2];
            // Never MORE than the pilot asked for, in whichever direction the stick is deflected.
            eff_ctrl.elevator =
                (ctrl.elevator > 0.f) ? std::clamp(cmd, -1.f, ctrl.elevator) : std::clamp(cmd, ctrl.elevator, 1.f);
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

    // 8a. Earth rotation (#482): the world frame is Earth-fixed rotating about world +Y (the polar
    // axis; north pole = origin) at Ω = m_earthRotationRate. Add the two fictitious accelerations,
    // Coriolis −2ω×v and centrifugal −ω×(ω×r). With ω = (0,Ω,0) both reduce to the axis-perpendicular
    // world components only:
    //     a_coriolis   = (−2Ω·v_z,      0,  +2Ω·v_x)
    //     a_centrifugal = (+Ω²·x,        0,   +Ω²·z)      (r_perp = (x,0,z) from the world Y spin axis)
    // so they vanish EXACTLY at the world origin (x=z=0, on the axis) and stay negligible near it,
    // sharing the radial-floor near-origin regression gate. Default Ω=0 ⇒ this block is skipped and
    // the integrator is bit-identical to the inertial-frame version; WorldBroadcaster/ClientPrediction
    // enable it. Deterministic (no RNG), so server and client-prediction integrators match exactly.
    if (m_earthRotationRate != 0.0) {
        const double O = m_earthRotationRate;
        const auto vw = quatRotateD(m_state.quat, m_state.vel_body); // world velocity for Coriolis
        const std::array<float, 3> aFict_world = {static_cast<float>(-2.0 * O * vw[2] + O * O * m_state.pos_world[0]),
                                                  0.f,
                                                  static_cast<float>(2.0 * O * vw[0] + O * O * m_state.pos_world[2])};
        const auto aFict_body = quatRotate(q_conj, aFict_world.data());
        forces[0] += eff_mass * aFict_body[0];
        forces[1] += eff_mass * aFict_body[1];
        forces[2] += eff_mass * aFict_body[2];
    }

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
    // moments = {roll, pitch, yaw}; omega = {roll(X), yaw(Y), pitch(Z)}. Inertia names are
    // aero-convention: ixx=roll, iyy=pitch, izz=yaw.
    const float Ixx = m_data->geometry.ixx_kg_m2;
    const float Iyy = m_data->geometry.iyy_kg_m2;
    const float Izz = m_data->geometry.izz_kg_m2;

    // Applied moments about the body axes, before the inertial solve. L=roll(about X),
    // M=pitch(about Z=right), N=yaw(about Y=up, engine convention).
    float L = moments[0];
    float M = moments[1];
    float N = moments[2];

    // Engine angular momentum He (#899): a spinning rotor about +X is a gyroscope, so M_gyro = −ω×H
    // with H=(He,0,0). In the engine frame (ω_x=omega[0], ω_y=omega[1]=yaw, ω_z=omega[2]=pitch) that
    // is a pitch moment +He·(yaw rate) and a yaw moment −He·(pitch rate); no roll term. He=0 ⇒ no
    // change. (The [prop] block keeps its own prop-specific gyro model — this is the general term.)
    const float He = m_data->geometry.engine_ang_momentum;
    if (He != 0.f) {
        M += He * m_state.omega[1]; // pitch (about Z) from yaw rate
        N -= He * m_state.omega[2]; // yaw   (about Y) from pitch rate
    }

    m_state.omega[2] += (M / Iyy) * dt; // pitch (omega[2] = around Z=right) — uncoupled by Ixz

    // Ixz roll↔yaw coupling (#899). With a product of inertia between the roll (x) and yaw (z) axes,
    // a roll moment produces a yaw acceleration and vice versa. Solving the coupled pair
    //   Ixx·ṗ − Ixz·ṙ = L,  −Ixz·ṗ + Izz·ṙ = N   (aero convention; ṙ_engine = −ṙ_aero, N_engine = −N)
    // gives, in the engine's yaw sign: ṗ = (Izz·L − Ixz·N)/det, ṙ = (Ixx·N − Ixz·L)/det. At Ixz=0 this
    // reduces to the decoupled L/Ixx, N/Izz — byte-identical to every pre-#899 model. Only the Ixz
    // İ·ω̇ coupling is added, not the ω×H rate-product Euler terms (they are non-zero at Ixz=0 and
    // would change all existing content); the term the issue calls out as "matter most" is this one.
    const float Ixz = m_data->geometry.ixz_kg_m2;
    const float det = Ixx * Izz - Ixz * Ixz;
    if (Ixz != 0.f && det > 0.f) {
        m_state.omega[0] += ((Izz * L - Ixz * N) / det) * dt; // roll (about X)
        m_state.omega[1] += ((Ixx * N - Ixz * L) / det) * dt; // yaw  (about Y=up)
    } else {
        m_state.omega[0] += (L / Ixx) * dt; // roll  (omega[0] = around X=fwd)
        m_state.omega[1] += (N / Izz) * dt; // yaw   (omega[1] = around Y=up)
    }

    // Clamp angular rates: prevents float overflow when aerodynamic moments are
    // extreme (e.g. 90° AoA freefall).  50 rad/s ≈ 2865°/s — well above any
    // physically reachable rate for the builtin model.
    constexpr float kMaxOmega = 50.f;
    m_state.omega[0] = std::clamp(m_state.omega[0], -kMaxOmega, kMaxOmega);
    m_state.omega[1] = std::clamp(m_state.omega[1], -kMaxOmega, kMaxOmega);
    m_state.omega[2] = std::clamp(m_state.omega[2], -kMaxOmega, kMaxOmega);

    // 12. Semi-implicit Euler: translational velocity, split into the force term and the transport
    // term of the body-frame equation of motion
    //
    //     v̇_body = F/m − ω × v_body.
    //
    // 12a. FORCE TERM. Thrust, drag, lift, gravity and turbulence change the SPEED; this is the only
    // part that adds or removes kinetic energy, and it is bounded by thrust-vs-drag physics.
    m_state.vel_body[0] += (forces[0] / eff_mass) * dt;
    m_state.vel_body[1] += (forces[1] / eff_mass) * dt;
    m_state.vel_body[2] += (forces[2] / eff_mass) * dt;

    // 12b. TRANSPORT TERM (−ω × v), applied as an EXACT ROTATION rather than the explicit tangent.
    //
    // The transport term is NOT optional: without it the velocity vector rotates WITH the airframe,
    // so pitching the nose develops no angle of attack, no extra lift, and cannot pull g at all (a
    // test model at full aft stick spun up 400 deg/s of pitch rate and peaked at 0.6 g). Everything
    // that reads AoA — turn rate, sustained g, stall entry, load factor (#816) — depends on it.
    //
    // But it must be INTEGRATED as a rotation, not as  v -= (ω × v)·dt. In the rotating body frame
    // an inertially-fixed velocity obeys v̇ = −ω × v, whose exact solution for constant ω is a pure
    // rotation of v by −ω·dt: |v| is conserved and the term does no work. The explicit tangent is
    // the first-order Taylor term of that rotation and OVERSHOOTS it, lengthening v by a factor
    // √(1 + (ω·dt)²) every tick. In cruise (ω ≈ 0) that is invisible, but under any sustained
    // rotation — a departure or incipient spin building several rad/s — it compounds into an
    // exponential airspeed runaway (creating energy at hundreds of times the thrust bound) until the
    // aircraft is flung out of the world at the speed clamp. That was #891: a statically stable pack
    // deck diverging from level flight, hands-off, in ~10 s, with the entity silently reaped.
    //
    // Rotating instead is exact for constant ω and unconditionally speed-conserving. The increment
    // is the CONJUGATE of the +ω·dt attitude increment integrateRotation() applies below (the body
    // axes turn by +ω·dt, so a fixed vector re-expressed in them turns by −ω·dt); using the same
    // normalized {0.5·ω·dt, 1} quaternion keeps the velocity and attitude rotations identical, so a
    // force-free rotation preserves alpha/beta exactly.
    float dqTransport[4] = {0.5f * m_state.omega[0] * dt, 0.5f * m_state.omega[1] * dt, 0.5f * m_state.omega[2] * dt,
                            1.f};
    quatNorm(dqTransport);
    dqTransport[0] = -dqTransport[0]; // conjugate → rotate v_body by −ω·dt
    dqTransport[1] = -dqTransport[1];
    dqTransport[2] = -dqTransport[2];
    const auto vel_rot = quatRotateD(dqTransport, m_state.vel_body);
    m_state.vel_body[0] = vel_rot[0];
    m_state.vel_body[1] = vel_rot[1];
    m_state.vel_body[2] = vel_rot[2];

    // NUMERICAL GUARD, NOT FLIGHT PHYSICS (#816). This exists to stop a NaN position or an absurd
    // aero force from overflowing the quaternion integration -- nothing more. It was doing double
    // duty as a top-speed limiter because `max_mach` was dead and a cd_wave table clamps flat above
    // its last breakpoint, so an over-thrusted model would simply accelerate into this wall.
    //
    // An aircraft's real top speed is set by its drag rising to meet its thrust. If a model can
    // exceed its declared max_mach in level flight, the MODEL is wrong, and fm-trim (#817) fails it
    // in CI. The engine does not paper over that with an artificial wall, so this is raised well
    // clear of any flyable regime and left as the overflow backstop it always was.
    // Per-instance since #354 (setSpeedGuard): ballistic vehicles legitimately pass Mach 6, so
    // their guard sits at ~8000 m/s — still a NaN backstop, never a top-speed limiter.
    // ONE-SHOT envelope-departure flag (#891): raised when the guard actually bites, so a divergence
    // is logged serially by WorldBroadcaster rather than silently flung out of the world and reaped.
    m_state.speed_guard_clamped = std::abs(m_state.vel_body[0]) > m_speedGuardMps ||
                                  std::abs(m_state.vel_body[1]) > m_speedGuardMps ||
                                  std::abs(m_state.vel_body[2]) > m_speedGuardMps;
    m_state.vel_body[0] = std::clamp(m_state.vel_body[0], -m_speedGuardMps, m_speedGuardMps);
    m_state.vel_body[1] = std::clamp(m_state.vel_body[1], -m_speedGuardMps, m_speedGuardMps);
    m_state.vel_body[2] = std::clamp(m_state.vel_body[2], -m_speedGuardMps, m_speedGuardMps);

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
            // does not act as a continuous brake during ground roll. A VESSEL (#38) rides this
            // floor as buoyancy, not wheels-on-dirt: its hull drag lives in VesselForceModel, so
            // the contact response must not skim speed off it every tick.
            const float kSlide =
                m_data->isVessel() ? 1.f : kSlideRoll + (kSlideImpact - kSlideRoll) * std::min(impactSpd / 10.f, 1.f);
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
            // Attenuate angular rates on impact to prevent post-contact spinning. Not for a
            // VESSEL (#38): a ship is permanently settling onto its water floor, and halving its
            // yaw rate every tick would crush the turn its rudder is commanding.
            if (!m_data->isVessel()) {
                m_state.omega[0] *= 0.5f;
                m_state.omega[1] *= 0.5f;
                m_state.omega[2] *= 0.5f;
            }
            // Rotate corrected world velocity back to body frame.
            float q_c[4] = {-m_state.quat[0], -m_state.quat[1], -m_state.quat[2], m_state.quat[3]};
            auto vb = quatRotate(q_c, vw.data());
            m_state.vel_body[0] = vb[0];
            m_state.vel_body[1] = vb[1];
            m_state.vel_body[2] = vb[2];
        }
    }

    // 14b-surface. Per-surface rolling resistance (#487). During ground contact, an unpaved surface
    // (grass, gravel) sheds horizontal ground speed faster than a hard runway, so the rollout differs
    // by surface. A hard paved surface (the default GroundFriction{}) adds nothing on top of the
    // baseline ground roll above — bit-identical to before this feature.
    if (ground.extraRollingPerSec > 0.f &&
        (m_gravity->geodeticAltitude(m_state.pos_world) - static_cast<double>(groundElev)) <= kGroundContactMarginM) {
        const float decay = std::max(0.f, 1.f - ground.extraRollingPerSec * dt);
        m_state.vel_body[0] *= decay; // forward
        m_state.vel_body[2] *= decay; // right (vertical vel_body[1] is the impact clamp's)
    }

    // 14b-brake. Wheel brakes, baseline rolling resistance, and nosewheel steering (#700). Only while
    // in ground contact — wheels down on the surface. Rolling resistance (~0.02 g) is always present so
    // a rollout decays instead of coasting forever; the pilot's wheelBrake adds up to ~0.35 g on top,
    // which is what lets a lander stop on the runway rather than run off the far end. Both act on the
    // HORIZONTAL body velocity only, capped so they can never reverse it. Nosewheel steering turns
    // rudder into a yaw RATE with authority that fades from full at taxi speed to nil by ~50 m/s, so
    // the aero rudder owns yaw during the takeoff roll and this never fights it. Placed before the
    // static parking hold (14c) so a truly stopped, near-idle aircraft still latches fully static.
    // A VESSEL (#38) is permanently "in contact" with its water floor and has neither wheels nor a
    // nosewheel — its drag and steering live in VesselForceModel, so this whole block is skipped.
    if (!m_data->isVessel() &&
        (m_gravity->geodeticAltitude(m_state.pos_world) - static_cast<double>(groundElev)) <= kGroundContactMarginM) {
        constexpr float kRollingResistG = 0.02f; // baseline tyre rolling resistance
        constexpr float kBrakeMaxG = 0.35f;      // full-pedal wheel-brake deceleration
        const float decel = (kRollingResistG + kBrakeMaxG * std::clamp(ctrl.wheelBrake, 0.f, 1.f)) * kG0;
        const float horizSpd =
            float(std::sqrt(m_state.vel_body[0] * m_state.vel_body[0] + m_state.vel_body[2] * m_state.vel_body[2]));
        if (horizSpd > 1e-4f) {
            const float newSpd = std::max(0.f, horizSpd - decel * dt);
            const float scale = newSpd / horizSpd;
            m_state.vel_body[0] *= scale; // forward
            m_state.vel_body[2] *= scale; // right (vertical vel_body[1] belongs to the impact clamp)
        }

        // Tire cornering grip: the wheels resist sliding sideways, so the ground velocity tracks the
        // heading. This is what turns the velocity vector when the nosewheel yaws the airframe below —
        // without it the transport term (12b) keeps the world velocity inertially fixed and the nose
        // just crabs off the direction of travel instead of the aircraft actually turning.
        constexpr float kTireGripPerSec = 4.f; // lateral (body-z) skid decay rate on the ground
        m_state.vel_body[2] *= std::max(0.f, 1.f - kTireGripPerSec * dt);

        // Nosewheel steering: omega[1] is yaw about +Y with positive = nose LEFT, and rudder +1 = right
        // yaw, so the full-authority target is -rudder * kSteerRate. Blend the current yaw rate toward
        // it with weight = authority: at taxi speed the nosewheel owns yaw entirely (rudder turns the
        // aircraft), by kSteerFadeMax it contributes nothing and the aero rudder is untouched.
        constexpr float kSteerFadeMinMps = 15.f; // full nosewheel authority at/below this ground speed
        constexpr float kSteerFadeMaxMps = 50.f; // no nosewheel authority at/above this ground speed
        constexpr float kSteerRateRadS = 0.35f;  // yaw rate at full rudder + full authority (~20 deg/s)
        const float authority =
            std::clamp((kSteerFadeMaxMps - horizSpd) / (kSteerFadeMaxMps - kSteerFadeMinMps), 0.f, 1.f);
        if (authority > 0.f) {
            const float targetYaw = -ctrl.rudder * kSteerRateRadS;
            m_state.omega[1] += authority * (targetYaw - m_state.omega[1]);
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

    // A ruptured fuel tank (#675) drains on top of the burn — a leak the pilot cannot throttle away.
    float burned = flow * dt + m_fuelLeakKgS * dt;
    m_state.fuel_kg = std::max(0.f, m_state.fuel_kg - burned);
    m_state.mass_kg = m_data->geometry.mass_kg + m_state.fuel_kg;
}

} // namespace fl
