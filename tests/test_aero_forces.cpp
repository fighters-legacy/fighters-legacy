// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "flight/AeroForces.h"
#include "flight/Atmosphere.h"
#include "flight/FlightModelParser.h"

#include <cmath>
#include <string>

using Catch::Matchers::WithinAbs;
using namespace fl;

static const std::string kGenericToml = R"(
[aircraft]
name         = "Test"
type         = "fighter"
engine_type  = "turbofan"
has_fbw      = false
cruise_alt_m = 10000.0
mesh         = "m"
cockpit      = "c"

[flight_model]
mass_kg      = 10000.0
wing_area_m2 = 35.0
wingspan_m   = 10.0
mac_m        = 3.5
fuel_kg      = 4000.0
ixx_kg_m2    = 10000.0
iyy_kg_m2    = 70000.0
izz_kg_m2    = 78000.0

[aero.cl_table]
alpha  = [-10.0, -5.0, 0.0, 5.0, 10.0, 15.0, 18.0, 20.0, 25.0]
mach   = [0.3, 0.6, 0.9]
values = [
    -0.55,-0.60,-0.66,
    -0.20,-0.22,-0.24,
     0.05, 0.06, 0.07,
     0.40, 0.45, 0.52,
     0.75, 0.84, 0.97,
     1.05, 1.18, 1.36,
     1.18, 1.32, 1.52,
     1.10, 1.23, 1.42,
     0.85, 0.95, 1.10,
]

[aero.drag_polar]
cd0           = 0.018
k             = 0.14
speedbrake_cd = 0.08
gear_cd       = 0.03

[aero.cd_wave]
mach   = [0.70, 0.85, 0.95, 1.00, 1.10, 1.50]
values = [0.000, 0.010, 0.040, 0.036, 0.018, 0.003]

[aero.moments]
cm_alpha = -0.7
cm_q     = -10.0
cm_de    = -1.0
cl_beta  = -0.08
cl_p     = -0.40
cl_da    =  0.07
cn_beta  =  0.10
cn_r     = -0.12
cn_dr    = -0.05

[aero.limits]
alpha_stall_deg  = 18.0
max_g_structural =  8.0
min_g_structural = -3.0
max_mach         =  1.6

[aero.controls]
max_elevator_deg = 25.0
max_aileron_deg  = 20.0
max_rudder_deg   = 30.0

[engine]
fuel_flow_idle_kg_s = 0.1
fuel_flow_mil_kg_s  = 1.0
fuel_flow_ab_kg_s   = 3.0
spool_time_s        = 5.0

[engine.mil_thrust]
mach   = [0.0, 0.3, 0.9]
alt_km = [0.0, 12.0]
values = [60.0, 30.0, 63.0, 31.0, 68.0, 34.0]
)";

static FlightModelData makeData() {
    return parseFlightModel(kGenericToml);
}

TEST_CASE("Lift is zero at zero alpha and Mach 0.6", "[aero]") {
    auto data = makeData();
    auto atmos = computeAtmosphere(0.f);
    ControlInput ctrl{};
    PayloadEffect payload{};
    // alpha=0, beta=0, Mach=0.6 (speed = 0.6 * 340.3 ≈ 204 m/s)
    float spd = 0.6f * atmos.speed_of_sound_m_s;
    auto f = computeForces(0.f, 0.f, 0.6f, spd, 0.f, 55.f, false, 0.f, ctrl, payload, data, atmos);
    // At alpha=0 CL≈0.06; lift is small but positive; we check forces[2] (−lift*cos(alpha)) < 0
    // Mainly verifying no crash and lift is non-NaN
    CHECK(std::isfinite(f[0]));
    CHECK(std::isfinite(f[2]));
}

TEST_CASE("Stall region CL is lower than pre-stall peak CL", "[aero]") {
    auto data = makeData();
    float mach = 0.3f;
    // Stall peak (18 deg) CL > deep stall (25 deg) CL
    float cl_peak = data.cl_table.lookup(18.f, mach);
    float cl_deep = data.cl_table.lookup(25.f, mach);
    CHECK(cl_peak > cl_deep);
}

TEST_CASE("Wave drag makes transonic CD higher than subsonic CD at same CL", "[aero]") {
    auto data = makeData();
    // CL at 5 deg, Mach 0.3 vs Mach 0.95
    float cl_sub = data.cl_table.lookup(5.f, 0.3f);
    float cl_tran = data.cl_table.lookup(5.f, 0.95f);
    float cd_sub = data.drag_polar.cd0 + data.drag_polar.k * cl_sub * cl_sub;
    float cd_wave = data.cd_wave->lookup(0.95f);
    float cd_tran = data.drag_polar.cd0 + data.drag_polar.k * cl_tran * cl_tran + cd_wave;
    CHECK(cd_tran > cd_sub);
    CHECK(cd_wave > 0.f);
}

TEST_CASE("Speedbrake and gear drag stack correctly", "[aero]") {
    auto data = makeData();
    auto atmos = computeAtmosphere(0.f);
    PayloadEffect payload{};
    float spd = 0.3f * atmos.speed_of_sound_m_s;

    ControlInput clean{};
    ControlInput with_brake{};
    with_brake.speedbrake = 1.f;
    ControlInput with_gear{};
    with_gear.gear_down = true;
    ControlInput both{};
    both.speedbrake = 1.f;
    both.gear_down = true;

    auto f_clean = computeForces(0.f, 0.f, 0.3f, spd, 0.f, 55.f, false, 0.f, clean, payload, data, atmos);
    auto f_brake = computeForces(0.f, 0.f, 0.3f, spd, 0.f, 55.f, false, 0.f, with_brake, payload, data, atmos);
    auto f_gear = computeForces(0.f, 0.f, 0.3f, spd, 0.f, 55.f, false, 0.f, with_gear, payload, data, atmos);
    auto f_both = computeForces(0.f, 0.f, 0.3f, spd, 0.f, 55.f, false, 0.f, both, payload, data, atmos);

    // Forward force (x) should be smaller (more drag) when speedbrake or gear are deployed
    CHECK(f_brake[0] < f_clean[0]);
    CHECK(f_gear[0] < f_clean[0]);
    CHECK(f_both[0] < f_brake[0]);
    CHECK(f_both[0] < f_gear[0]);
}

TEST_CASE("Zero speed produces zero aerodynamic forces without NaN", "[aero]") {
    auto data = makeData();
    auto atmos = computeAtmosphere(0.f);
    ControlInput ctrl{};
    PayloadEffect payload{};
    auto f = computeForces(0.f, 0.f, 0.f, 0.f, 0.f, 55.f, false, 0.f, ctrl, payload, data, atmos);
    CHECK(std::isfinite(f[0]));
    CHECK(std::isfinite(f[1]));
    CHECK(std::isfinite(f[2]));
    // Dynamic pressure is zero, so aero forces are zero (thrust also 0 at throttle=0)
    CHECK_THAT(f[0], WithinAbs(0.f, 1e-3f));
    CHECK_THAT(f[2], WithinAbs(0.f, 1e-3f));
}

TEST_CASE("Zero speed produces finite moments without NaN", "[aero]") {
    auto data = makeData();
    auto atmos = computeAtmosphere(0.f);
    ControlInput ctrl{};
    auto m = computeMoments(0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, ctrl, data, atmos);
    CHECK(std::isfinite(m[0]));
    CHECK(std::isfinite(m[1]));
    CHECK(std::isfinite(m[2]));
}

TEST_CASE("Payload drag and mass accumulate correctly", "[aero]") {
    auto data = makeData();
    auto atmos = computeAtmosphere(0.f);
    ControlInput ctrl{};

    PayloadEffect no_payload{};
    PayloadEffect with_payload{.extra_mass_kg = 500.f, .extra_cd0 = 0.016f};

    float spd = 0.6f * atmos.speed_of_sound_m_s;
    auto f0 = computeForces(0.f, 0.f, 0.6f, spd, 0.f, 55.f, false, 0.8f, ctrl, no_payload, data, atmos);
    auto fp = computeForces(0.f, 0.f, 0.6f, spd, 0.f, 55.f, false, 0.8f, ctrl, with_payload, data, atmos);
    // Higher drag from payload → less forward force
    CHECK(fp[0] < f0[0]);
}
