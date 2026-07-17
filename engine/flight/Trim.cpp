// SPDX-License-Identifier: GPL-3.0-or-later
#include "flight/Trim.h"

#include "flight/Atmosphere.h"

#include <algorithm>
#include <cmath>
#include <limits>
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
    const float mach = machNumber(speed, c.atmos.speed_of_sound_m_s);
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
    // THE SENTINEL IS NaN, NOT -1. A cambered table (the F-16's LEF-scheduled deck carries
    // CL = +0.10 at alpha 0) trims 1 g at a LEGITIMATELY NEGATIVE alpha once q is high enough --
    // at sea level that crossing sits near M 0.65, squarely inside the envelope. The old -1
    // sentinel made every caller read "trim alpha is -0.4 deg" as "the wing cannot hold it",
    // and fm-trim reported an F-16 whose top speed FELL when fuel burned off. A symmetric
    // table (every aircraft before the F-16A) never trims negative at 1 g, which is why this
    // survived until published cambered data arrived -- fl-base-pack#19's inversion working
    // exactly as intended.
    if (liftAt(d, payload, c, alphaMax, speed) < needed)
        return std::numeric_limits<float>::quiet_NaN(); // even at max lift the wing cannot hold it

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
    // The manual's turn and Ps numbers are quoted at max thrust, so the point's `afterburner` flag
    // selects the thrust setting for every thrust-dependent metric below (#826).
    const bool hasAb = d.engine.ab_thrust.has_value() && pt.afterburner;
    // THE LEVEL-FLIGHT DRAG CURVE IS U-SHAPED. DO NOT BREAK ON THE FIRST NEGATIVE (#825).
    //
    // The original loop stopped at the first speed where drag exceeded thrust, on the assumption that
    // "everything faster is unreachable". That assumption is false, and it is false for every
    // aeroplane ever built. At the stall speed the wing is at CL_max, where induced drag is enormous:
    // this is the region of reversed command -- the back side of the power curve -- and it is a real
    // place a real aircraft can be. As it accelerates, CL falls, induced drag collapses, total drag
    // reaches a minimum at best L/D, and only THEN climbs again toward the true max speed.
    //
    // So excess thrust is routinely negative AT the stall, positive across the whole usable envelope,
    // and negative again past max speed. Breaking on the first negative saw the back side, concluded
    // the aircraft could not fly, and reported `converged = false` -- for an F-5E with FOURTEEN
    // kilonewtons of excess thrust in the middle of its envelope. It read like a content bug and
    // would have sent the next author hunting through a drag table for a fault that was not there.
    //
    // Scan the whole range instead. The cannot-hold break stays, and is a different thing entirely:
    // if no alpha holds 1 g at this speed, the wing genuinely cannot carry the weight, and nothing
    // faster changes that (CL_max only falls with Mach). "Cannot hold" is NaN -- a negative trim
    // alpha is a real flight condition on a cambered wing, not a failure (see alphaForLoad).
    float maxLevel = 0.f;
    float minLevel = 0.f;
    for (float v = r.stall_speed_1g_mps; v <= kSpeedMax; v += kSpeedStep) {
        const float a = alphaForLoad(d, payload, c, v, 1.f);
        if (std::isnan(a))
            break;
        if (excessThrustAt(d, payload, c, a, v, hasAb) >= 0.f) {
            if (minLevel <= 0.f)
                minLevel = v; // the slowest speed the engine can actually sustain
            maxLevel = v;     // keep going — drag is U-shaped, not monotonic
        }
    }
    r.max_level_mach = machNumber(maxLevel, c.atmos.speed_of_sound_m_s);

    // The honest answer to "how slow can this thing actually go". Above the stall it is genuinely a
    // different number: on the back side of the curve the wing can still carry the weight, but the
    // engine cannot pay for the drag, so the aircraft sinks. If NO speed in the range has non-negative
    // excess thrust, minLevel stays 0 and `converged` below is false — which is then the true "cannot
    // hold level flight here", rather than an artefact of where the scan happened to stop.
    r.min_level_speed_mps = minLevel;

    // ── rate of climb: max over V of (T − D) · V / W, at MIL and at AB. ─────────────────────────
    auto bestRoc = [&](bool ab) {
        float best = -1e30f;
        for (float v = r.stall_speed_1g_mps; v <= kSpeedMax; v += kSpeedStep) {
            const float a = alphaForLoad(d, payload, c, v, 1.f);
            if (std::isnan(a))
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
        const float a1g = alphaForLoad(d, payload, c, v, lo);
        if (std::isnan(a1g) || excessThrustAt(d, payload, c, a1g, v, hasAb) < 0.f)
            continue; // cannot even sustain 1 g here
        for (int i = 0; i < 30; ++i) {
            const float mid = 0.5f * (lo + hi);
            const float a = alphaForLoad(d, payload, c, v, mid);
            if (!std::isnan(a) && excessThrustAt(d, payload, c, a, v, hasAb) >= 0.f)
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

    // ── AT A PINNED MACH (#826) ─────────────────────────────────────────────────────────────────
    //
    // Everything above answers "what is the best this aircraft can do, at any speed". The flight
    // manual does not ask that question. It publishes numbers AT a Mach, and the model can only be
    // gated against them if the solver can be asked the same question. So when a Mach is pinned, the
    // turn figures are recomputed AT it, and Ps and the lift limit become available.
    if (pt.mach > 0.f && c.atmos.speed_of_sound_m_s > 0.f) {
        const float v = pt.mach * c.atmos.speed_of_sound_m_s;

        // The lift limit at this Mach: what pins CL_max, and it is published (5.2 g at 15 000 ft /
        // M 0.60 for the F-5E). NOT capped by the structure — this is what the WING can make.
        r.max_lift_g = maxAeroLoad(d, payload, c, v);

        // Sustained: the largest n whose drag the engine can still pay for AT THIS SPEED.
        const float nCeiling = std::min(r.max_lift_g, nStruct);
        const float aPin = alphaForLoad(d, payload, c, v, 1.f);
        if (nCeiling > 1.f && !std::isnan(aPin) && excessThrustAt(d, payload, c, aPin, v, hasAb) >= 0.f) {
            float lo = 1.f, hi = nCeiling;
            for (int i = 0; i < 30; ++i) {
                const float mid = 0.5f * (lo + hi);
                const float a = alphaForLoad(d, payload, c, v, mid);
                if (!std::isnan(a) && excessThrustAt(d, payload, c, a, v, hasAb) >= 0.f)
                    lo = mid;
                else
                    hi = mid;
            }
            r.sustained_g = lo;
            r.sustained_turn_deg_s = turnRateDegS(lo, v);
        } else {
            r.sustained_g = 0.f;
            r.sustained_turn_deg_s = 0.f;
        }

        // Instantaneous at this Mach: the wing's limit, then the structure's.
        r.instant_g = nCeiling;
        r.instant_turn_deg_s = turnRateDegS(nCeiling, v);
        r.corner_speed_mps = v;

        // Specific excess power at this Mach and load factor: Ps = V·(T − D)/W. This is the ladder the
        // drag table is fitted to, and it is plumbing, not new physics — every term already existed.
        if (pt.load_factor > 0.f) {
            const float a = alphaForLoad(d, payload, c, v, pt.load_factor);
            if (!std::isnan(a))
                r.ps_mps = excessThrustAt(d, payload, c, a, v, hasAb) * v / c.weight_n;
            else
                r.ps_mps = 0.f; // the wing cannot hold this n here: there is no such flight condition
        }
    }

    // ── specific range: metres per kg of fuel, at the speed that maximises it. ──────────────────
    float bestSr = 0.f;
    for (float v = r.stall_speed_1g_mps; v <= kSpeedMax; v += kSpeedStep) {
        const float a = alphaForLoad(d, payload, c, v, 1.f);
        if (std::isnan(a))
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
