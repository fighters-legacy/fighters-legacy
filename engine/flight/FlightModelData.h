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
    Ballistic,  // #354: an unwinged boost/coast vehicle — flown by BallisticForceModel, not wings
    Multirotor, // #349: a quad/hex/octo rotor frame — flown by MultirotorForceModel (thrust mixing)
    Helicopter, // #350: a single-main-rotor helicopter — flown by HelicopterForceModel (rotor disc)
    Vessel      // #38: a surface ship (carrier, escort) — flown by VesselForceModel on the water floor
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

    // Optional product of inertia between the roll (x) and yaw (z) body axes (#899). A real fighter's
    // mass is not symmetric about its principal planes, so a rolling moment produces a yawing
    // acceleration and vice versa — the inertial roll/yaw coupling TP-1538's own EOM carries (F-16 =
    // 1,331 kg·m²). 0 = the symmetric, decoupled rotational update every existing model uses; a
    // non-zero value opts the airframe into the Ixz-coupled solve (byte-identical at 0). See
    // FlightIntegrator's rotational update.
    float ixz_kg_m2{0.f};

    // Optional engine rotor angular momentum He about +X forward, N·m·s (#899). A spinning spool/rotor
    // is a gyroscope: pitching the airframe yaws it and yawing it pitches it (F-16 = 216.9 kg·m²/s).
    // Signed — the sign follows the rotor's spin direction. 0 = no gyroscopic reaction (the default;
    // the [prop] block carries a separate, prop-specific gyro model for piston/turboprop types).
    float engine_ang_momentum{0.f};
};

struct AeroDragPolar {
    float cd0{0.018f};
    float k{0.14f};
    float speedbrake_cd{0.07f};
    float gear_cd{0.03f};

    // Optional speed-brake normal-force (lift) increment per unit deployment (#899, ΔCZ,sb). A real
    // airbrake changes lift as well as drag; TP-1538 publishes both. Added as q·S·(cl · speedbrake) to
    // the lift when the brake is out. 0 = drag only (the default; every existing model).
    float speedbrake_cl{0.f};
};

struct AeroMoments {
    // Pitch (reference length: mac_m)
    float cm0{0.f}; // zero-alpha pitching moment (#899): a cambered wing's Cm at alpha=0 is non-zero,
                    // which sets the zero-elevator trim-alpha offset (≈ −3.3° for the F-16A). 0 =
                    // symmetric section, trim alpha at 0 — every pre-#899 model.
    float cm_alpha{-0.7f};
    float cm_q{-10.f};
    float cm_de{-1.f};
    float cm_speedbrake{0.f}; // pitch increment per unit speed-brake deployment (#899, ΔCm,sb)
    // Roll (reference length: wingspan_m)
    float cl_beta{-0.08f};
    float cl_p{-0.40f};
    float cl_da{0.07f};
    float cl_dr{0.f}; // rudder-induced roll (#899): rudder deflection rolls the aircraft. 0 = none.
    // Yaw (reference length: wingspan_m)
    float cn_beta{0.10f};
    float cn_r{-0.12f};
    float cn_dr{-0.05f};
    float cn_da{0.f}; // adverse yaw (#899): aileron deflection yaws AGAINST the roll. 0 = none.

    // Optional alpha-dependent dynamic dampers (#899). TP-1538 publishes cm_q/cl_p/cn_r as tables over
    // alpha (Cmq ranges −3.4 to −6.8 across the sweep), and the post-stall damping story is exactly
    // what a deep-stall study is about. When present, the table REPLACES the scalar in computeMoments
    // (lookup over alpha_deg); absent, the scalar above is used — bit-identical to before.
    std::optional<Table1D> cm_q_table;
    std::optional<Table1D> cl_p_table;
    std::optional<Table1D> cn_r_table;
};

struct AeroLimits {
    float alpha_stall_deg{18.f};
    float max_g_structural{8.f};
    float min_g_structural{-3.f};
    float max_mach{1.6f};

    // Optional FLCS angle-of-attack cap (#900), distinct from alpha_stall_deg (the AERODYNAMIC table
    // peak). A fly-by-wire jet's computer holds alpha below a limit its aero can exceed — the F-16's
    // FLCS holds 25.5° while the wing stalls at ~35°. When set (>0) and has_fbw, the limiter also
    // holds |alpha| ≤ this cap, tighter than the g-limit at low q. 0 = unset (structural-g only).
    float alpha_limit_deg{0.f};
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

    // Number of independent engines (#308). Drives the engine-out asymmetry: losing ONE of
    // `engine_count` engines (a kEngineFailLeft/Right event) removes 1/engine_count of total thrust
    // and yaws the nose toward the dead side. Defaults to 2 — bit-identical to the previous hardcoded
    // "one engine out = half thrust", which silently assumed a twin. A four-holer now sheds a quarter,
    // not a half. A single-engine airframe (engine_count 1) fails via the centreline path
    // (kEngineFailCenter = total loss, no yaw), so this fraction never applies to it.
    int engine_count{2};

    // Fraction of the wingspan used as the moment arm for the engine-out yaw (#308). Defaults to 0.15
    // — bit-identical to the previous hardcoded arm — because true engine lateral offsets are not
    // modelled; an author who knows the real spacing can widen it (podded, wing-mounted engines yaw
    // harder than fuselage-packed ones).
    float engine_yaw_arm_frac{0.15f};

    // Afterburner envelope (#309), both optional with permissive defaults so a model that omits them
    // is bit-identical to before. AB lights only inside the window; outside it (below the ram-limit
    // Mach, or above the altitude ceiling) the augmentor extinguishes even with the throttle in zone.
    // An absent limit is simply not applied — so a model with neither field behaves exactly as before
    // (AB available whenever commanded and an ab_thrust deck exists). Gated in FlightIntegrator::step
    // with a small hysteresis band so a model riding the boundary does not chatter.
    std::optional<float> ab_min_mach;   // AB unavailable below this Mach (too little ram to sustain it)
    std::optional<float> ab_max_alt_km; // AB extinguishes above this altitude in km (too little oxygen)

    // ── engine failure dynamics (#308) ───────────────────────────────────────
    // The integrator RAISES kEngineFlameout / kEngineCompStall from these; until #308 those bits were
    // defined, carried on the wire, and consumed by haptics — but never set by anything.

    // Optional combustion ceiling in km: above it the burner cannot sustain light-off and the engine
    // flames out (kEngineFlameout = total thrust loss) until a windmill relight — back below the
    // ceiling with airspeed >= relight_min_mps. Absent = no altitude flameout (bit-identical to
    // before). Distinct from ab_max_alt_km, which only extinguishes the augmentor.
    std::optional<float> flameout_alt_km;

    // Minimum airspeed for a windmill relight after a flameout (m/s). Only read while recovering
    // from a flameout, so the default costs a model that never flames out nothing. Fuel-starvation
    // flameouts also relight through this gate once fuel is available again (tanker/base ops).
    float relight_min_mps{60.f};

    // Opt-in compressor-surge model: at high alpha the intake blanks and a hard-working compressor
    // surges — a transient total thrust loss (kEngineCompStall) that clears a fixed recovery time
    // after the disturbed flow condition ends. Default off, so existing content is bit-identical;
    // a deep-stall-honest model turns it on.
    bool compressor_stall{false};

    // Alpha margin PAST alpha_stall_deg where surge risk begins (deg), at high commanded power.
    // Only read when compressor_stall is on.
    float surge_alpha_margin_deg{5.f};
};

// ── [drone_limits] (#351) ────────────────────────────────────────────────────
// The ONBOARD AUTOPILOT's command envelope for a fixed-wing UAV — distinct from [aero.limits]
// (what the airframe can survive) and from has_fbw (a manned jet's FLCS). A Predator-class
// airframe is aerodynamically capable of far more than its autopilot will ever command: the
// autopilot holds bank shallow, load factor low, and airspeed inside a narrow band, and that
// command shaping — not the structure — is what defines how the vehicle flies. Enforced by
// FlightIntegrator on the COMMANDS (never a silent physics clamp), reusing the FBW AoA-limiting
// path for g so the two limiters cannot drift. Each field 0 = that gate off; the block absent =
// bit-identical to before.
struct DroneLimits {
    float max_bank_deg{0.f};     // autopilot bank-angle limit (aileron command shaping); 0 = off
    float max_g{0.f};            // autopilot load-factor limit, tighter than structural; 0 = off
    float min_airspeed_mps{0.f}; // stall protection: throttle floors up below this; 0 = off
    float max_airspeed_mps{0.f}; // overspeed protection: throttle shed above this; 0 = off
};

// ── [multirotor] (#349) ──────────────────────────────────────────────────────
// A multirotor is thrust mixing, not wings: total rotor thrust along body +Y (up), attitude control
// from differential per-rotor thrust, yaw from differential rotor torque. The flight-control inner
// loop (rate feedback) lives IN the force model, because a real multirotor is unflyable without its
// FC and the model without one would be an aerobatics problem, not an aircraft. Authored in the
// reduced `type = "multirotor"` schema — no CL tables, no stability derivatives.
struct MultirotorData {
    int rotor_count{4};             // number of rotors (drives per-rotor loss on an engine-out)
    float rotor_thrust_max_n{60.f}; // max thrust PER ROTOR at sea-level density
    float rotor_arm_m{0.35f};       // CG-to-rotor moment arm
    float yaw_torque_nm{8.f};       // max yaw moment from differential rotor torque at full pedal
    float frame_cd{1.0f};           // flat-plate frame drag coefficient (all axes)
    float frame_area_m2{0.1f};      // frame reference area for the drag term
    float attitude_authority{0.3f}; // fraction of per-rotor max thrust available for pitch/roll mixing
    float rate_damping_s{1.0f};     // FC inner-loop rate feedback (s/rad); sets full-stick rate ≈ 1/k
};

// ── [helicopter] (#350) ──────────────────────────────────────────────────────
// A single-main-rotor helicopter as a rotor DISC, not blade elements: collective drives
// density-scaled disc thrust along body-up (ground effect and translational lift scale it), cyclic
// tilts the disc (pitch/roll moments with rotor-follow rate damping), pedals command the tail
// rotor against the optional main-rotor torque reaction, and an unpowered disc autorotates —
// axial momentum drag through the disc caps the sink rate at a survivable figure, which is the
// gameplay truth of autorotation without carrying rotor-RPM state. Blade flapping appears as the
// classic flapback speed-stability moment (nose rises with forward speed).
struct HelicopterData {
    float main_rotor_radius_m{7.3f};       // disc geometry: ground effect + autorotation drag area
    float main_rotor_max_thrust_n{130e3f}; // max collective thrust at sea-level density
    float yaw_moment_max_nm{40e3f};        // tail-rotor yaw moment at full pedal
    float cyclic_moment_nm{60e3f};         // pitch/roll moment at full cyclic
    float rate_damping_s{1.5f};            // rotor-follow rate feedback (s/rad)
    float flapback_nm_per_mps{0.f};        // nose-up moment per m/s of forward speed (0 = off)
    float torque_factor{0.f};              // main-rotor torque reaction as a fraction of T·R that the
                                           // pedals must hold against (0 = auto-trimmed hover)
    float frame_cd{0.8f};                  // parasite flat-plate drag coefficient
    float frame_area_m2{2.0f};             // parasite reference area
    float ground_effect_frac{0.15f};       // max thrust bonus, fading out by one rotor diameter AGL
    float translational_lift_frac{0.12f};  // max thrust bonus from effective translational lift
    float translational_lift_mps{25.f};    // forward speed where the ETL bonus saturates
    float autorotation_cd{1.2f};           // axial disc drag coefficient (the autorotation term)
};

// ── [vessel] (#38) ───────────────────────────────────────────────────────────
// A surface ship as a controlled entity: propulsion along the keel, quadratic water drag sized so
// the declared top speed is where thrust and drag meet, a rate-commanded rudder that needs
// steerage way, and hard damping of everything a displacement hull does not do (roll, pitch,
// sideslip). The ship rides the integrator's radial floor clamped to sea level — a moving carrier
// is an ordinary ControlledEntity that replicates, takes damage, and is steered by any
// IEntityController (a WaypointController drives a patrol track), not a bespoke platform system.
struct VesselData {
    float max_thrust_n{2.0e6f};  // propulsion at full ahead
    float max_speed_mps{15.f};   // top speed; sizes the water drag (thrust = drag here)
    float turn_rate_deg_s{1.5f}; // steady turn rate at full rudder and steerage way
    float steerage_mps{2.f};     // below this speed the rudder has nothing to bite
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
    std::optional<DroneLimits> drone_limits;  // #351: fixed-wing UAV autopilot command envelope
    std::optional<MultirotorData> multirotor; // #349: present iff type = "multirotor"
    std::optional<HelicopterData> helicopter; // #350: present iff type = "helicopter"
    std::optional<VesselData> vessel;         // #38: present iff type = "vessel"
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
    [[nodiscard]] bool isMultirotor() const noexcept {
        return meta.role == AircraftRole::Multirotor;
    }
    [[nodiscard]] bool isHelicopter() const noexcept {
        return meta.role == AircraftRole::Helicopter;
    }
    // Rotorcraft (#349/#350) hover; fixed-wing performance derivation (fl::trim, the manual, fm-trim)
    // is meaningless for them and gates on this.
    [[nodiscard]] bool isRotorcraft() const noexcept {
        return isMultirotor() || isHelicopter();
    }
    [[nodiscard]] bool isVessel() const noexcept {
        return meta.role == AircraftRole::Vessel;
    }
    [[nodiscard]] bool isFixedWing() const noexcept {
        return !isBallistic() && !isRotorcraft() && !isVessel();
    }
};

} // namespace fl
