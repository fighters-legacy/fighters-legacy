// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "math/Table1D.h"
#include "math/Table2D.h"

#include <optional>
#include <string>

namespace fl {

enum class EngineType { Turbojet, Turbofan, Turboprop, Piston };

enum class AircraftRole {
    Fighter,
    Interceptor,
    Attacker,
    Bomber,
    MaritimePatrol,
    Awacs,
    Ew,
    Recon,
    Tanker,
    Transport,
    Trainer,
    Ballistic // #354: an unwinged boost/coast vehicle — flown by BallisticForceModel, not wings
};

enum class PropRotation { CW, CCW, Contra };

// A FLIGHT MODEL IS AERODYNAMICS. IT DOES NOT KNOW WHAT IT LOOKS LIKE (#813).
//
// `mesh` and `cockpit` used to live here, were REQUIRED by the parser, and were read by absolutely
// nothing -- the renderer has always used EntityDef::mesh. Two sources of truth for an aircraft's
// asset wiring, and the one the parser enforced was the dead one. EntityDef is now the sole owner
// (mesh, cockpitMesh, flightModelAsset, aiScriptAsset, classicDamageMesh); the flight model owns
// only physics. The keys are still accepted in TOML and ignored, so existing files parse.
struct AircraftMeta {
    std::string name;
    AircraftRole role{AircraftRole::Fighter};
    EngineType engine_type{EngineType::Turbofan};
    bool has_fbw{false}; // gates the G-limiter, and nothing else (#816)
    float cruise_alt_m{10000.f};
};

struct FlightModelGeometry {
    float mass_kg{10000.f};
    float wing_area_m2{35.f};
    float wingspan_m{10.f};
    float mac_m{3.5f};
    float fuel_kg{4000.f};
    float ixx_kg_m2{10000.f};
    float iyy_kg_m2{70000.f};
    float izz_kg_m2{78000.f};
};

struct AeroDragPolar {
    float cd0{0.018f};
    float k{0.14f};
    float speedbrake_cd{0.07f};
    float gear_cd{0.03f};
};

struct AeroMoments {
    // Pitch (reference length: mac_m)
    float cm_alpha{-0.7f};
    float cm_q{-10.f};
    float cm_de{-1.f};
    // Roll (reference length: wingspan_m)
    float cl_beta{-0.08f};
    float cl_p{-0.40f};
    float cl_da{0.07f};
    // Yaw (reference length: wingspan_m)
    float cn_beta{0.10f};
    float cn_r{-0.12f};
    float cn_dr{-0.05f};
};

struct AeroLimits {
    float alpha_stall_deg{18.f};
    float max_g_structural{8.f};
    float min_g_structural{-3.f};
    float max_mach{1.6f};
};

// Control-surface travel, in degrees of deflection at full stick.
//
// ASYMMETRY IS THE NORM ON THE PITCH AXIS (#822). A fighter needs far more nose-up authority than
// nose-down: the F-5E's all-moving stabilator travels 17° nose-up but only 5° nose-down (T.O.
// 1F-5E-1), a 3.4:1 ratio, and the F-16's and T-38's are asymmetric too. With one scalar per axis
// the author must pick a number, and picking the nose-up figure (which governs pitch authority and
// the ability to reach the G limit) models the aircraft's nose-down authority 3.4x too generous: a
// bunt or a negative-G push is far more effective than the real aeroplane's.
//
// So the elevator gets an optional negative-side travel. It defaults to the positive value, which
// makes a model that does not use it bit-identical to before.
//
// ROLL AND YAW DELIBERATELY DO NOT GET ONE. Left and right are mirror images: `aileron = -1` is the
// same deflection as `aileron = +1` seen from the other side, so a sign-dependent travel would mean
// an aircraft that rolls harder one way than the other. (The F-5E's 35°-up/25°-down aileron is a
// per-SURFACE differential, not a per-command asymmetry, and its gear-driven aileron spring stop is
// a control-system behaviour — a soft stop the pilot can overpower at the cost of a structural limit
// — not a travel limit. Neither belongs here.)
struct AeroControls {
    float max_elevator_deg{25.f};     // nose-up authority at full aft stick
    float max_elevator_neg_deg{25.f}; // nose-down authority at full forward stick; = max_elevator_deg when unset
    float max_aileron_deg{20.f};
    float max_rudder_deg{30.f};
};

struct TvcData {
    float min_angle_deg{-20.f};
    float max_angle_deg{20.f};
    float slew_rate_deg_s{5.f};
};

struct WingSweepConfig {
    float cl_scale{1.f};
    float k_scale{1.f};
    float cd0_delta{0.f};
};

struct WingSweepData {
    float ref_sweep_deg{55.f};
    float min_deg{20.f};
    float max_deg{68.f};
    float slew_rate_deg_s{7.5f};
    Table1D schedule;       // Mach -> commanded sweep deg
    WingSweepConfig spread; // at min_deg
    WingSweepConfig swept;  // at max_deg
};

struct PropData {
    PropRotation rotation{PropRotation::CW};
    float torque_factor{0.f};
    float gyro_factor{0.f};
};

struct EngineData {
    EngineType type{EngineType::Turbofan};
    Table2D mil_thrust; // (Mach, alt_km) -> kN
    std::optional<Table2D> ab_thrust;

    // Optional idle deck (#898): (Mach, alt_km) -> kN, same shape as mil_thrust. Real turbofan idle
    // thrust is neither zero nor a linear scaling of MIL: at altitude and speed it goes NEGATIVE
    // because ram drag exceeds idle gross thrust (NASA TP-1538 Table VI gives the F-16 +2.824 kN
    // static but −16.013 kN at M 1.0). When present, computeForces blends idle → mil across throttle
    // [0, 1] instead of 0 → mil, so part-throttle behaviour (descents, approach, energy management)
    // is modelled. Absent, the straight throttle × mil line stays the default — bit-identical to
    // before. Values are kN like mil_thrust, so a deck published in newtons is /1000 on authoring.
    std::optional<Table2D> idle_thrust;

    float fuel_flow_idle_kg_s{0.1f};
    float fuel_flow_mil_kg_s{1.f};
    float fuel_flow_ab_kg_s{3.f};
    float spool_time_s{5.f};
};

struct CarrierData {
    float approach_m_s{69.f};
    float approach_aoa_deg{8.f};
    float cat_min_m_s{67.f};
    float hook_length_m{5.f};
};

struct RefuelingData {
    bool boom{true}; // true = boom, false = drogue
    float max_rate_kg_s{2.f};
};

struct TankerData {
    bool boom{true};
    bool drogue{false};
    int stations{1};
    float max_rate_kg_s{4.f};
    float offload_reserve{0.2f};
};

// Aggregate: everything the flight integrator needs for one aircraft type.
struct FlightModelData {
    AircraftMeta meta;
    FlightModelGeometry geometry;
    Table2D cl_table; // (alpha_deg, Mach) -> CL
    AeroDragPolar drag_polar;

    // Tabulated TOTAL clean drag (alpha_deg, Mach) -> CD (#820).
    //
    // WHY THIS EXISTS. A strictly parabolic polar (cd0 + k*CL^2) forces specific excess power to be
    // exactly quadratic in load factor, which means the implied induced-drag coefficient must be
    // CONSTANT across a Ps chart. Real fighters do not behave that way: against T.O. 1F-5E-1's worked
    // examples the implied coefficient grows 3.5x from the 1-2 g region to the 4-5 g region, because
    // a real wing's drag rises far faster than CL^2 as it approaches max lift. Fit k to cruise and
    // the F-5E sustains 3.92 g where the manual says 3.30; fit k to the hard-turn end and cruise drag
    // is overstated, wrecking range and acceleration. THERE IS NO VALUE OF k THAT GIVES BOTH.
    //
    // It is also the form real published data arrives in: NASA TP-1538 gives the F-16's CD as a table
    // against alpha and Mach, and it cannot be transcribed into a parabolic polar at all.
    //
    // WHEN PRESENT, THIS REPLACES cd0 + k*CL^2 ENTIRELY -- the table is total clean drag and already
    // includes the induced term. cd_wave, speedbrake_cd, gear_cd and payload.extra_cd0 still add on
    // top (an author whose table already spans Mach may fold wave drag into it and omit cd_wave; the
    // terms are additive and independent, so that is their call). When absent, nothing changes and
    // [aero.drag_polar] remains the simple path -- which is what most community content will use.
    std::optional<Table2D> cd_table;

    std::optional<Table1D> cd_wave; // Mach -> delta-CD
    AeroMoments moments;
    AeroLimits limits;
    AeroControls controls;
    std::optional<TvcData> tvc;
    std::optional<WingSweepData> wing_sweep;
    std::optional<PropData> prop;
    EngineData engine;
    std::optional<CarrierData> carrier;
    std::optional<RefuelingData> refueling;
    std::optional<TankerData> tanker;

    // Ballistic boost (#354): constant motor thrust while propellant (fuel_kg) remains. The burn
    // TIME lives in the fuel flow — the parser sets fuel_flow_idle/mil = fuel_kg / burn_time_s, so
    // a solid motor burns to depletion through the integrator's existing fuel path with no special
    // case, throttle be damned. 0 = not a ballistic model.
    float boost_thrust_n{0.f};

    [[nodiscard]] bool isBallistic() const noexcept {
        return meta.role == AircraftRole::Ballistic;
    }
};

} // namespace fl
