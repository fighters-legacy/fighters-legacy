// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The variable-geometry flight-model fixture the #1195 wing-sweep tests share.
//
// It lives in its own header because three suites need it — the snapshot pipeline (does sweep reach
// the wire), the broadcaster (does it reach the world-state aggregate) and client prediction (does
// the own aircraft keep it across a reconcile) — and a fourth hand-written copy of a swing-wing
// model is exactly the kind of duplication that let this defect exist in the first place.

#include "flight/FlightModelParser.h"

#include <string>

// A variable-geometry flight model for the #1195 wing-sweep cases, with the sweep SCHEDULE pinned
// flat at `refSweepDeg`. That is deliberate: a pilot peer spawns parked, so a Mach-driven schedule
// would leave every test at min_deg and assert nothing. Following the schedule as Mach changes is
// the integrator's job and is pinned in test_flight_integrator.cpp; what these cases test is that
// the angle leaves the integrator at all, which for two releases it did not.
//
// min_deg is 15 rather than 0 on purpose — a normalization that forgot to subtract it would still
// produce 1.0 at max_deg and pass a sloppier test.
// `scheduleSweepDeg` defaults to the reference, i.e. a wing that sits still. Passing a different
// value gives a wing that SLEWS from where it spawned toward where the schedule wants it, which is
// what the client-prediction case needs to prove the angle survives a reconcile.
inline std::string makeVgFlightModelToml(float refSweepDeg, float scheduleSweepDeg = -1.f) {
    const std::string ref = std::to_string(refSweepDeg);
    const std::string sched = std::to_string(scheduleSweepDeg < 0.f ? refSweepDeg : scheduleSweepDeg);
    return R"(
[aircraft]
name         = "Test Swing Wing"
type         = "bomber"
engine_type  = "turbofan"
has_fbw      = false
cruise_alt_m = 12000.0

[flight_model]
mass_kg      = 87090.0
wing_area_m2 = 181.16
wingspan_m   = 41.758
mac_m        = 4.758
fuel_kg      = 120326.0
ixx_kg_m2    = 7451784.0
iyy_kg_m2    = 10571333.0
izz_kg_m2    = 13313039.0

[aero.cl_table]
alpha  = [-4.0, 0.0, 8.0, 13.0]
mach   = [0.30, 0.90]
values = [
    -0.28, -0.33,
     0.06,  0.06,
     0.75,  0.84,
     1.18,  1.33,
]

[aero.drag_polar]
cd0           = 0.0175
k             = 0.0413
speedbrake_cd = 0.0400
gear_cd       = 0.0250

[aero.moments]
cm_alpha = -2.0631
cm_q     = -22.6828
cm_de    = -1.2696
cl_beta  = -0.2857
cl_p     = -0.6834
cl_da    =  0.3740
cn_beta  =  0.1543
cn_r     = -0.1565
cn_dr    = -0.0919

[aero.limits]
alpha_stall_deg  = 13.0
max_g_structural =  2.50
min_g_structural = -1.00
max_mach         =  1.25

[aero.controls]
max_elevator_deg = 25.0
max_aileron_deg  = 20.0
max_rudder_deg   = 25.0

[engine]
fuel_flow_idle_kg_s = 0.900
fuel_flow_mil_kg_s  = 4.910
fuel_flow_ab_kg_s   = 29.500
spool_time_s        = 5.500

[engine.mil_thrust]
mach   = [0.00, 0.90]
alt_km = [0.00, 11.00]
values = [
    309.40, 110.27,
    366.48, 130.61,
]

[wing_sweep]
ref_sweep_deg   = )" +
           ref + R"(
min_deg         = 15.0
max_deg         = 67.5
slew_rate_deg_s = 90.0

[wing_sweep.schedule]
mach  = [0.00, 1.25]
sweep = [)" +
           sched + ", " + sched + R"(]

[wing_sweep.spread]
cl_scale  = 1.000
k_scale   = 1.000
cd0_delta = +0.0000

[wing_sweep.swept]
cl_scale  = 0.666
k_scale   = 1.715
cd0_delta = -0.0020
)";
}
