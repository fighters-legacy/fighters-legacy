// SPDX-License-Identifier: GPL-3.0-or-later
#include "flight/BuiltinFlightModel.h"

#include "flight/FlightModelParser.h"

namespace fl {

namespace {

// The builtin trainer (#1334), authored in the same TOML schema as pack content and parsed once on
// first use. Every number below is load-bearing against a consumer the survey in #1334 enumerated:
//
//  - stall: CL peaks 1.35 at alpha 15 = alpha_stall_deg (the validator's "the table IS the stall"
//    rule), giving Vs(1 g, gross) = 54 m/s at sea level — under the AI's fixed 75/70 m/s
//    approach/rotate defaults (AtcBehaviors.h) and well under the 120 m/s default airborne spawn.
//  - thrust: 14 kN sea-level static on 4450 kg gross, T/W = 0.32. Max level speed M0.64 (SL) to
//    M0.71 — under the declared max_mach 0.80, which fm-trim holds the model to. The deck's alt_km
//    decay is the ceiling: ~1 m/s residual climb at 11 km.
//  - pattern work: sustains a 45-degree-bank orbit at the geometry-derived 146-168 m/s
//    (Guidance.h orbit speeds) with about 2x thrust margin at 3 km; 1 g trim alpha at AI speeds
//    stays a few degrees, far under the 0.20 rad guidance AoA bound (#1186).
//  - fuel: 950 kg at real flows — about an hour at MIL, ~1.5 h on patrol throttle. Running dry is a
//    real #308 flameout with the 60 m/s windmill relight.
//  - 60 Hz stability: the per-tick pitch-damping ratio q_dyn*S*mac^2*|cm_q|*dt/(2*V*Iyy) stays
//    ~0.02 at 200 m/s sea level, far inside the semi-implicit-Euler threshold; cn_beta/cn_r damp
//    sideslip per the #891 gate shape.
//  - elevator gearing: cm_alpha/cm_de = 0.43, i.e. reaching 1 rad of alpha costs ~0.75 of the
//    20-deg elevator travel. This is load-bearing for the AI: elevatorForAltitudeHold's P-only
//    elevator loop droops on a statically stable airframe — its equilibrium deflection is capped
//    at ~(2/pi)*(kDefaultMaxAoaRad - trim alpha) ~= 0.11 of travel (#1186) — and at this gearing
//    that still trims ~4 deg of alpha, enough lift for the 45-degree-bank loiter orbit with margin.
//    The pre-#1334 UFO had cm_alpha = 0 (neutral static stability), which made pitch a pure
//    integrator and hid the droop entirely; a steeper gearing here re-creates the #1186 B-1B
//    descent under every default-bound AI controller.
//  - no afterburner deck: ctrl.afterburner is a no-op for this model (FlightIntegrator gates AB on
//    ab_thrust.has_value()); fuel_flow_ab_kg_s is required by the parser and never drawn.
constexpr const char* kBuiltinTrainerToml = R"(
[aircraft]
name         = "builtin:trainer"
type         = "trainer"
engine_type  = "turbofan"
has_fbw      = false
cruise_alt_m = 6000.0

[flight_model]
mass_kg      = 3500.0
wing_area_m2 = 18.8
wingspan_m   = 9.5
mac_m        = 2.1
fuel_kg      = 950.0
ixx_kg_m2    = 4600.0
iyy_kg_m2    = 28000.0
izz_kg_m2    = 31000.0

[aero.cl_table]
alpha  = [-10.0, -5.0, 0.0, 5.0, 10.0, 15.0, 20.0, 30.0]
mach   = [0.0, 0.5, 0.85]
values = [
    -0.75, -0.75, -0.70,
    -0.31, -0.31, -0.28,
     0.12,  0.12,  0.14,
     0.55,  0.56,  0.58,
     0.96,  0.98,  1.00,
     1.35,  1.37,  1.30,
     1.05,  1.02,  0.95,
     0.60,  0.55,  0.50,
]

[aero.drag_polar]
cd0           = 0.021
k             = 0.088
speedbrake_cd = 0.06
gear_cd       = 0.025

[aero.moments]
cm_alpha = -0.65
cm_q     = -7.0
cm_de    = -1.5
cl_beta  = -0.08
cl_p     = -0.42
cl_da    =  0.10
cn_beta  =  0.12
cn_r     = -0.30
cn_dr    = -0.06

[aero.limits]
alpha_stall_deg  = 15.0
max_g_structural =  7.0
min_g_structural = -3.0
max_mach         =  0.80

[aero.controls]
max_elevator_deg = 20.0
max_aileron_deg  = 18.0
max_rudder_deg   = 25.0

[engine]
fuel_flow_idle_kg_s = 0.035
fuel_flow_mil_kg_s  = 0.24
fuel_flow_ab_kg_s   = 0.24
spool_time_s        = 4.0

[engine.mil_thrust]
mach   = [0.0, 0.4, 0.8]
alt_km = [0.0, 6.0, 12.0]
values = [
    14.0, 8.1, 3.9,
    12.8, 7.5, 3.6,
    11.5, 6.8, 3.3,
]
)";

// The builtin helicopter (#1335): light-utility class in round numbers — 3.0 t empty + 600 kg
// fuel, a 7.3 m rotor at 48 kN max collective (hover at ~74% collective at gross, honest margin,
// nothing acrobatic), docile cyclic/pedal authority, and a draggy utility frame that tops out
// near 70 m/s (~135 kt) in level tilt. Fuel flows give a couple of hours; running dry is a real
// #308 flameout (autorotation is the force model's own term).
constexpr const char* kBuiltinHelicopterToml = R"(
[aircraft]
name        = "builtin:helicopter"
type        = "helicopter"
engine_type = "turbofan"

[flight_model]
mass_kg   = 3000.0
fuel_kg   = 600.0
ixx_kg_m2 = 3500.0
iyy_kg_m2 = 14000.0
izz_kg_m2 = 12000.0

[helicopter]
main_rotor_radius_m     = 7.3
main_rotor_max_thrust_n = 48000.0
yaw_moment_max_nm       = 12000.0
cyclic_moment_nm        = 25000.0
flapback_nm_per_mps     = 40.0
torque_factor           = 0.05
frame_cd                = 1.2
frame_area_m2           = 2.5

[engine]
fuel_flow_idle_kg_s = 0.02
fuel_flow_mil_kg_s  = 0.12
)";

// The builtin multirotor (#1335): a large camera-drone-class quad — 9 kg empty + 3 kg battery-mass
// budget, 55 N per rotor (hover at ~53% throttle), deliberately slow and stable rather than a
// racer. flight_time_min folds the pack-style endurance into the fuel path.
constexpr const char* kBuiltinMultirotorToml = R"(
[aircraft]
name        = "builtin:multirotor"
type        = "multirotor"
engine_type = "turbofan"

[flight_model]
mass_kg   = 9.0
fuel_kg   = 3.0
ixx_kg_m2 = 0.5
iyy_kg_m2 = 0.5
izz_kg_m2 = 0.9

[multirotor]
rotor_count        = 4
rotor_thrust_max_n = 55.0
rotor_arm_m        = 0.45
yaw_torque_nm      = 6.0
frame_cd           = 1.1
frame_area_m2      = 0.15
attitude_authority = 0.25
rate_damping_s     = 1.4
flight_time_min    = 30.0
)";

} // namespace

std::shared_ptr<const FlightModelData> BuiltinFlightModel::get() {
    static std::shared_ptr<const FlightModelData> kInstance =
        std::make_shared<const FlightModelData>(parseFlightModel(kBuiltinTrainerToml));
    return kInstance;
}

std::shared_ptr<const FlightModelData> BuiltinHelicopterModel::get() {
    static std::shared_ptr<const FlightModelData> kInstance =
        std::make_shared<const FlightModelData>(parseFlightModel(kBuiltinHelicopterToml));
    return kInstance;
}

std::shared_ptr<const FlightModelData> BuiltinMultirotorModel::get() {
    static std::shared_ptr<const FlightModelData> kInstance =
        std::make_shared<const FlightModelData>(parseFlightModel(kBuiltinMultirotorToml));
    return kInstance;
}

std::shared_ptr<const FlightModelData> builtinFlightModel(std::string_view name) {
    if (name == "builtin:trainer")
        return BuiltinFlightModel::get();
    if (name == "builtin:carrier-vessel")
        return BuiltinCarrierVesselModel::get();
    if (name == "builtin:helicopter")
        return BuiltinHelicopterModel::get();
    if (name == "builtin:multirotor")
        return BuiltinMultirotorModel::get();
    return nullptr;
}

std::shared_ptr<const FlightModelData> BuiltinCarrierVesselModel::get() {
    static std::shared_ptr<const FlightModelData> kInstance = [] {
        auto d = std::make_shared<FlightModelData>();
        d->meta.name = "builtin:carrier-vessel";
        d->meta.role = AircraftRole::Vessel;

        d->geometry.mass_kg = 9.0e7f; // ~100 000 t displacement, less fuel below
        d->geometry.fuel_kg = 1.0e6f;
        d->geometry.ixx_kg_m2 = 5.0e10f;
        d->geometry.iyy_kg_m2 = 5.0e11f;
        d->geometry.izz_kg_m2 = 5.0e11f;

        VesselData v;
        v.max_thrust_n = 8.0e6f; // meets the quadratic water drag at flank speed
        v.max_speed_mps = 16.f;  // ~31 kt
        v.turn_rate_deg_s = 1.0f;
        v.steerage_mps = 2.f;
        d->vessel = v;

        // Endurance not modelled: a zero mil flow cannot starve (#308's fuel gate).
        d->engine.fuel_flow_idle_kg_s = 0.f;
        d->engine.fuel_flow_mil_kg_s = 0.f;
        d->engine.fuel_flow_ab_kg_s = 0.f;
        d->engine.spool_time_s = 10.f; // the telegraph answers slowly

        // Benign placeholder aero (the reduced-schema discipline).
        d->drag_polar.cd0 = 1.0f;
        d->drag_polar.k = 0.f;
        d->cl_table.rows = {-90.f, -30.f, 30.f, 90.f};
        d->cl_table.cols = {0.f, 30.f};
        d->cl_table.values.assign(8, 0.f);
        d->engine.mil_thrust.rows = {0.f, 30.f};
        d->engine.mil_thrust.cols = {0.f, 90.f};
        d->engine.mil_thrust.values.assign(4, 0.f);
        d->limits.alpha_stall_deg = 90.f;
        d->limits.max_g_structural = 100.f;
        d->limits.min_g_structural = -100.f;
        return d;
    }();
    return kInstance;
}

} // namespace fl
