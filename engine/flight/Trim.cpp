// SPDX-License-Identifier: GPL-3.0-or-later
#include "flight/Trim.h"

#include "flight/Atmosphere.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace fl {
namespace {

constexpr float kG0 = 9.80665f;
constexpr float kDegToRad = std::numbers::pi_v<float> / 180.f;

// Every number below comes out of computeForces, so the tool and the sim cannot disagree about what
// the aircraft's aerodynamics ARE. If someone changes the drag model, these numbers move, and the
// expectation gate catches it. That is the point.
struct Condition {
    AtmosphereState atmos;
    float weight_n{0.f};
    float S{0.f};
    float altitude_m{0.f}; // MUST be threaded into computeForces: the thrust deck is a table over
                           // (Mach, alt), so passing 0 here would fly the whole aircraft on sea-level
                           // thrust and hand back a fighter that climbs like a rocket at 11 km.
};

// Body-x and body-y aero+propulsive force at a given alpha, speed and throttle setting.
std::array<float, 3> forcesAt(const FlightModelData& d, const PayloadEffect& payload, const Condition& c,
                              float alpha_deg, float speed, bool ab, float throttle) {
    ControlInput ctrl{};
    ctrl.throttle = throttle;
    ctrl.afterburner = ab;
    const float mach = (c.atmos.speed_of_sound_m_s > 0.f) ? speed / c.atmos.speed_of_sound_m_s : 0.f;
    const float sweep = d.wing_sweep ? d.wing_sweep->ref_sweep_deg : 0.f;
    return computeForces(alpha_deg * kDegToRad, 0.f, mach, speed, c.altitude_m, sweep, ab, throttle, ctrl, payload, d,
                         c.atmos);
}

// WIND AXES, NOT BODY AXES -- and the difference is the whole ballgame.
//
// computeForces returns BODY-frame forces: fx = T − D·cos α + L·sin α, fy = L·cos α + D·sin α. Read
// the body-x force as "excess thrust" and you get nonsense, because at the alpha needed for a hard
// turn the L·sin α term is enormous: on an F-5E pulling 7 g, lift's contribution to body-x is 105 kN
// against 77 kN of drag, so body-x stays positive and the aircraft appears to sustain a turn it
// cannot possibly pay for. (It reported a sustained turn exactly equal to its instantaneous turn --
// physically impossible, and the tell that the projection was wrong.)
//
// What actually costs energy is the force along the FLIGHT PATH. The velocity vector in body axes is
// (cos α, −sin α), so:
//     along-path force  =  fx·cos α − fy·sin α   =  T·cos α − D     (lift, being perpendicular, drops out)
//     normal force      =  fx·sin α + fy·cos α   =  L               (with thrust off)
float liftAt(const FlightModelData& d, const PayloadEffect& payload, const Condition& c, float alpha_deg, float speed) {
    const auto f = forcesAt(d, payload, c, alpha_deg, speed, false, 0.f);
    const float a = alpha_deg * kDegToRad;
    return f[0] * std::sin(a) + f[1] * std::cos(a);
}

// Same projection, at an explicit throttle setting (0 = pure drag).
float excessThrustAtThrottle(const FlightModelData& d, const PayloadEffect& payload, const Condition& c,
                             float alpha_deg, float speed, float throttle) {
    const auto f = forcesAt(d, payload, c, alpha_deg, speed, false, throttle);
    const float a = alpha_deg * kDegToRad;
    return f[0] * std::cos(a) - f[1] * std::sin(a);
}

// Net force along the flight path: thrust component minus drag. Positive means the aircraft can
// accelerate or climb; zero is the sustained condition.
float excessThrustAt(const FlightModelData& d, const PayloadEffect& payload, const Condition& c, float alpha_deg,
                     float speed, bool ab) {
    const auto f = forcesAt(d, payload, c, alpha_deg, speed, ab, 1.f);
    const float a = alpha_deg * kDegToRad;
    return f[0] * std::cos(a) - f[1] * std::sin(a);
}

// The alpha that carries loadFactor x weight at this speed, or -1 if the wing cannot.
float alphaForLoad(const FlightModelData& d, const PayloadEffect& payload, const Condition& c, float speed,
                   float loadFactor) {
    const float needed = loadFactor * c.weight_n;
    const float alphaMax = d.limits.alpha_stall_deg;
    if (liftAt(d, payload, c, alphaMax, speed) < needed)
        return -1.f; // even at max lift the wing cannot hold it

    float lo = -5.f, hi = alphaMax;
    for (int i = 0; i < 40; ++i) {
        const float mid = 0.5f * (lo + hi);
        if (liftAt(d, payload, c, mid, speed) < needed)
            lo = mid;
        else
            hi = mid;
    }
    return 0.5f * (lo + hi);
}

// Max load factor the WING can make at this speed (structure ignored).
float maxAeroLoad(const FlightModelData& d, const PayloadEffect& payload, const Condition& c, float speed) {
    if (c.weight_n <= 0.f)
        return 0.f;
    return liftAt(d, payload, c, d.limits.alpha_stall_deg, speed) / c.weight_n;
}

constexpr float kSpeedMin = 20.f;
constexpr float kSpeedMax = 1200.f;
constexpr float kSpeedStep = 2.f;

} // namespace

TrimResult trim(const FlightModelData& d, const TrimPoint& pt, const PayloadEffect& payload) {
    TrimResult r;

    Condition c;
    c.atmos = computeAtmosphere(pt.altitude_m);
    c.altitude_m = pt.altitude_m;
    const float mass = (pt.mass_kg > 0.f) ? pt.mass_kg : (d.geometry.mass_kg + d.geometry.fuel_kg);
    c.weight_n = (mass + payload.extra_mass_kg) * kG0;
    c.S = d.geometry.wing_area_m2;

    r.fuel_flow_mil_kg_s = d.engine.fuel_flow_mil_kg_s;
    r.fuel_flow_ab_kg_s = d.engine.ab_thrust ? d.engine.fuel_flow_ab_kg_s : d.engine.fuel_flow_mil_kg_s;

    // ── 1 g stall speed: the slowest speed at which CL_max still carries the weight. ────────────
    for (float v = kSpeedMin; v <= kSpeedMax; v += kSpeedStep) {
        if (maxAeroLoad(d, payload, c, v) >= 1.f) {
            r.stall_speed_1g_mps = v;
            break;
        }
    }
    if (r.stall_speed_1g_mps <= 0.f)
        return r; // cannot fly level at all here — converged stays false, and we say so

    // ── max level speed: fastest speed where thrust still matches drag at the alpha that holds 1 g.
    const bool hasAb = d.engine.ab_thrust.has_value();
    float maxLevel = 0.f;
    for (float v = r.stall_speed_1g_mps; v <= kSpeedMax; v += kSpeedStep) {
        const float a = alphaForLoad(d, payload, c, v, 1.f);
        if (a < 0.f)
            break;
        if (excessThrustAt(d, payload, c, a, v, hasAb) >= 0.f)
            maxLevel = v;
        else
            break; // drag has overtaken thrust: everything faster is unreachable
    }
    r.max_level_mach = (c.atmos.speed_of_sound_m_s > 0.f) ? maxLevel / c.atmos.speed_of_sound_m_s : 0.f;

    // ── rate of climb: max over V of (T − D) · V / W, at MIL and at AB. ─────────────────────────
    auto bestRoc = [&](bool ab) {
        float best = -1e30f;
        for (float v = r.stall_speed_1g_mps; v <= kSpeedMax; v += kSpeedStep) {
            const float a = alphaForLoad(d, payload, c, v, 1.f);
            if (a < 0.f)
                continue;
            const float ps = excessThrustAt(d, payload, c, a, v, ab) * v / c.weight_n;
            best = std::max(best, ps);
        }
        return std::max(0.f, best);
    };
    r.roc_mps_mil = bestRoc(false);
    r.roc_mps_ab = hasAb ? bestRoc(true) : r.roc_mps_mil;

    // ── turn performance. ───────────────────────────────────────────────────────────────────────
    //
    // SUSTAINED: the hardest turn the engine can pay for -- the largest n at which thrust still
    // equals drag, so the aircraft holds its speed. INSTANT: the hardest turn the WING can make,
    // capped by the structure, which the aircraft can only hold for as long as its energy lasts.
    // CORNER SPEED is where they meet: the speed at which you can pull the airframe's limit AND
    // sustain it, and the number that most defines an aircraft in a fight.
    const float nStruct = (d.limits.max_g_structural > 0.f) ? d.limits.max_g_structural : 9.f;

    auto turnRateDegS = [&](float n, float v) {
        if (n <= 1.f || v <= 0.f)
            return 0.f;
        return (kG0 * std::sqrt(n * n - 1.f) / v) / kDegToRad;
    };

    for (float v = r.stall_speed_1g_mps; v <= kSpeedMax; v += kSpeedStep) {
        // Instantaneous: wing limit, then structural limit.
        const float nAero = std::min(maxAeroLoad(d, payload, c, v), nStruct);
        const float instRate = turnRateDegS(nAero, v);
        if (instRate > r.instant_turn_deg_s) {
            r.instant_turn_deg_s = instRate;
            r.instant_g = nAero;
        }

        // Sustained: the largest n (up to the wing/structure limit) whose drag the engine can still
        // pay for at this speed. Bisect on n.
        float lo = 1.f, hi = nAero;
        if (hi <= lo)
            continue;
        if (excessThrustAt(d, payload, c, alphaForLoad(d, payload, c, v, lo), v, hasAb) < 0.f)
            continue; // cannot even sustain 1 g here
        for (int i = 0; i < 30; ++i) {
            const float mid = 0.5f * (lo + hi);
            const float a = alphaForLoad(d, payload, c, v, mid);
            if (a >= 0.f && excessThrustAt(d, payload, c, a, v, hasAb) >= 0.f)
                lo = mid;
            else
                hi = mid;
        }
        const float susRate = turnRateDegS(lo, v);
        if (susRate > r.sustained_turn_deg_s) {
            r.sustained_turn_deg_s = susRate;
            r.sustained_g = lo;
            r.corner_speed_mps = v; // best sustained turn = the corner
        }
    }

    // ── specific range: metres per kg of fuel, at the speed that maximises it. ──────────────────
    float bestSr = 0.f;
    for (float v = r.stall_speed_1g_mps; v <= kSpeedMax; v += kSpeedStep) {
        const float a = alphaForLoad(d, payload, c, v, 1.f);
        if (a < 0.f)
            continue;
        // Throttle needed to hold level flight = drag / available MIL thrust, both projected along
        // the flight path (see the wind-axes note above).
        const float dragOnly = -excessThrustAtThrottle(d, payload, c, a, v, 0.f); // T=0 ⇒ −D
        const float milThrust = excessThrustAt(d, payload, c, a, v, false) + dragOnly;
        if (milThrust <= 0.f || dragOnly <= 0.f)
            continue;
        const float throttleNeeded = dragOnly / milThrust;
        if (throttleNeeded > 1.f)
            continue; // cannot hold this speed on MIL
        const float burn = std::max(1e-6f, throttleNeeded * d.engine.fuel_flow_mil_kg_s);
        bestSr = std::max(bestSr, v / burn);
    }
    r.specific_range_m_per_kg = bestSr;

    r.converged = (r.stall_speed_1g_mps > 0.f && r.max_level_mach > 0.f);
    return r;
}

} // namespace fl
