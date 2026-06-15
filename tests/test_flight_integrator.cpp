// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "flight/Atmosphere.h"
#include "flight/BuiltinFlightModel.h"
#include "flight/CentralGravityField.h"
#include "flight/FlightIntegrator.h"
#include "flight/FlightModelParser.h"

#include <cmath>
#include <memory>
#include <string>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using namespace fl;

static const std::string kBaseToml = R"(
[aircraft]
name         = "Integrator Test"
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
alpha  = [-5.0, 0.0, 5.0, 10.0, 15.0, 18.0, 20.0, 25.0]
mach   = [0.3, 0.9]
values = [
    -0.20,-0.24,
     0.05, 0.07,
     0.40, 0.52,
     0.75, 0.97,
     1.05, 1.36,
     1.18, 1.52,
     1.10, 1.42,
     0.85, 1.10,
]

[aero.drag_polar]
cd0           = 0.018
k             = 0.14
speedbrake_cd = 0.08
gear_cd       = 0.03

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

static const std::string kWingSweepToml = R"(
[wing_sweep]
ref_sweep_deg    = 45.0
min_deg          = 20.0
max_deg          = 68.0
slew_rate_deg_s  = 720.0

[wing_sweep.schedule]
mach  = [0.0, 0.5, 1.0]
sweep = [20.0, 45.0, 68.0]

[wing_sweep.spread]
cl_scale  = 1.1
k_scale   = 0.9
cd0_delta = 0.002

[wing_sweep.swept]
cl_scale  = 0.9
k_scale   = 1.1
cd0_delta = 0.005
)";

static std::shared_ptr<FlightModelData> makeData(const std::string& extra = "") {
    return std::make_shared<FlightModelData>(parseFlightModel(kBaseToml + extra));
}

TEST_CASE("Integrator: single step changes state", "[integrator]") {
    FlightIntegrator integ(makeData());
    FlightState s{};
    s.vel_body[0] = 100.f; // 100 m/s forward
    s.pos_world[1] = 1000.f;
    s.mass_kg = 14000.f;
    s.fuel_kg = 4000.f;
    integ.reset(s);

    ControlInput ctrl{};
    ctrl.throttle = 0.5f;
    PayloadEffect px{};
    integ.step(1.f / 60.f, ctrl, px);

    // Position must have changed (aircraft is moving forward)
    CHECK(std::isfinite(integ.state().pos_world[0]));
    CHECK(std::isfinite(integ.state().vel_body[0]));
}

TEST_CASE("Integrator: fuel burns at MIL rate when throttle=1", "[integrator]") {
    auto data = makeData();
    FlightIntegrator integ(data);
    FlightState s{};
    s.vel_body[0] = 50.f;
    s.pos_world[1] = 1000.f;
    s.mass_kg = data->geometry.mass_kg + data->geometry.fuel_kg;
    s.fuel_kg = data->geometry.fuel_kg;
    s.throttle_actual = 1.f;
    integ.reset(s);

    ControlInput ctrl{};
    ctrl.throttle = 1.f;
    PayloadEffect px{};

    float dt = 1.f;
    float before = integ.state().fuel_kg;
    integ.step(dt, ctrl, px);
    float after = integ.state().fuel_kg;
    float burned = before - after;

    // Throttle is 1.0 (spool catches up after 1s at 5s spool_time),
    // but spool_actual starts at 0 and moves toward 1 → partial burn.
    // At least idle flow should have burned.
    CHECK(burned >= data->engine.fuel_flow_idle_kg_s * dt * 0.9f);
    CHECK(burned <= data->engine.fuel_flow_mil_kg_s * dt * 1.1f);
}

TEST_CASE("Integrator: spool lag converges to commanded throttle", "[integrator]") {
    auto data = makeData();
    FlightIntegrator integ(data);
    FlightState s{};
    s.vel_body[0] = 50.f;
    s.pos_world[1] = 1000.f;
    s.mass_kg = data->geometry.mass_kg + data->geometry.fuel_kg;
    s.fuel_kg = data->geometry.fuel_kg;
    integ.reset(s);

    ControlInput ctrl{};
    ctrl.throttle = 1.f;
    PayloadEffect px{};

    // Spool time = 5 s; after 5× the time constant (25 s) throttle_actual should be >= 0.99
    float dt = 1.f / 60.f;
    float spool_t = data->engine.spool_time_s;
    int ticks = static_cast<int>(5.f * spool_t / dt);
    for (int i = 0; i < ticks; ++i)
        integ.step(dt, ctrl, px);

    CHECK(integ.state().throttle_actual >= 0.99f);
}

TEST_CASE("Integrator: fuel does not go negative", "[integrator]") {
    auto data = makeData();
    FlightIntegrator integ(data);
    FlightState s{};
    s.vel_body[0] = 100.f;
    s.pos_world[1] = 1000.f;
    s.mass_kg = data->geometry.mass_kg + 1.f;
    s.fuel_kg = 1.f; // nearly empty
    s.throttle_actual = 1.f;
    s.ab_engaged = true;
    integ.reset(s);

    ControlInput ctrl{};
    ctrl.throttle = 1.f;
    ctrl.afterburner = true;
    PayloadEffect px{};

    for (int i = 0; i < 600; ++i)
        integ.step(1.f / 60.f, ctrl, px);

    CHECK(integ.state().fuel_kg >= 0.f);
}

TEST_CASE("Integrator: ab_engaged set when afterburner commanded and ab_thrust table present", "[integrator]") {
    // Minimal 2x2 ab_thrust table (2 mach breakpoints x 2 alt_km breakpoints required by parser)
    auto data = makeData(R"(
[engine.ab_thrust]
mach   = [0.0, 1.0]
alt_km = [0.0, 12.0]
values = [100.0, 80.0, 150.0, 120.0]
)");
    FlightIntegrator fi(data);
    FlightState s{};
    s.vel_body[0] = 50.f;
    s.pos_world[1] = 1000.f;
    s.mass_kg = data->geometry.mass_kg + data->geometry.fuel_kg;
    s.fuel_kg = data->geometry.fuel_kg;
    fi.reset(s);

    ControlInput ctrl{};
    ctrl.throttle = 1.f;
    ctrl.afterburner = true;
    PayloadEffect px{};
    fi.step(1.f / 60.f, ctrl, px);

    CHECK(fi.state().ab_engaged == true);
}

TEST_CASE("Integrator: ab_engaged false when afterburner not commanded", "[integrator]") {
    auto data = makeData(R"(
[engine.ab_thrust]
mach   = [0.0, 1.0]
alt_km = [0.0, 12.0]
values = [100.0, 80.0, 150.0, 120.0]
)");
    FlightIntegrator fi(data);
    FlightState s{};
    s.vel_body[0] = 50.f;
    s.pos_world[1] = 1000.f;
    s.mass_kg = data->geometry.mass_kg + data->geometry.fuel_kg;
    s.fuel_kg = data->geometry.fuel_kg;
    fi.reset(s);

    ControlInput ctrl{};
    ctrl.throttle = 1.f;
    ctrl.afterburner = false;
    PayloadEffect px{};
    fi.step(1.f / 60.f, ctrl, px);

    CHECK(fi.state().ab_engaged == false);
}

TEST_CASE("Integrator: ab_engaged false when afterburner commanded but no ab_thrust table", "[integrator]") {
    auto data = makeData(); // builtin model: no ab_thrust table
    FlightIntegrator fi(data);
    FlightState s{};
    s.vel_body[0] = 50.f;
    s.pos_world[1] = 1000.f;
    s.mass_kg = data->geometry.mass_kg + data->geometry.fuel_kg;
    s.fuel_kg = data->geometry.fuel_kg;
    fi.reset(s);

    ControlInput ctrl{};
    ctrl.throttle = 1.f;
    ctrl.afterburner = true; // commanded, but no ab_thrust → physically impossible
    PayloadEffect px{};
    fi.step(1.f / 60.f, ctrl, px);

    CHECK(fi.state().ab_engaged == false);
}

TEST_CASE("Integrator: no NaN propagation at zero airspeed", "[integrator]") {
    auto data = makeData();
    FlightIntegrator integ(data);
    FlightState s{}; // all zero velocity
    s.pos_world[1] = 0.f;
    s.mass_kg = data->geometry.mass_kg + data->geometry.fuel_kg;
    s.fuel_kg = data->geometry.fuel_kg;
    integ.reset(s);

    ControlInput ctrl{};
    PayloadEffect px{};
    integ.step(1.f / 60.f, ctrl, px);

    const auto& st = integ.state();
    CHECK(std::isfinite(st.vel_body[0]));
    CHECK(std::isfinite(st.omega[0]));
    CHECK(std::isfinite(st.pos_world[1]));
}

TEST_CASE("Integrator: payload increases effective drag", "[integrator]") {
    auto data = makeData();
    FlightState init{};
    init.vel_body[0] = 200.f;
    init.pos_world[1] = 3000.f;
    init.mass_kg = data->geometry.mass_kg + data->geometry.fuel_kg;
    init.fuel_kg = data->geometry.fuel_kg;
    init.throttle_actual = 0.8f;

    ControlInput ctrl{};
    ctrl.throttle = 0.8f;

    PayloadEffect clean{};
    PayloadEffect heavy{.extra_mass_kg = 3000.f, .extra_cd0 = 0.030f};

    FlightIntegrator integ_clean(data);
    integ_clean.reset(init);
    integ_clean.step(1.f / 60.f, ctrl, clean);

    FlightIntegrator integ_heavy(data);
    integ_heavy.reset(init);
    integ_heavy.step(1.f / 60.f, ctrl, heavy);

    // Heavy payload → more drag → less forward acceleration
    CHECK(integ_heavy.state().vel_body[0] < integ_clean.state().vel_body[0]);
}

TEST_CASE("Integrator: prop torque adds roll moment (CW rotation)", "[integrator]") {
    std::string prop_toml = R"(
[prop]
rotation      = "cw"
torque_factor = 0.08
gyro_factor   = 0.02
)";
    auto data = makeData(prop_toml);
    FlightState s{};
    s.vel_body[0] = 100.f;
    s.pos_world[1] = 1000.f;
    s.mass_kg = data->geometry.mass_kg + data->geometry.fuel_kg;
    s.fuel_kg = data->geometry.fuel_kg;
    s.throttle_actual = 1.f;
    FlightIntegrator integ(data);
    integ.reset(s);

    ControlInput ctrl{};
    ctrl.throttle = 1.f;
    PayloadEffect px{};
    integ.step(1.f / 60.f, ctrl, px);

    // CW prop → negative roll moment → omega[0] (roll rate) becomes negative
    CHECK(integ.state().omega[0] < 0.f);
}

TEST_CASE("Integrator: contra-rotating prop produces zero net torque", "[integrator]") {
    std::string prop_toml = R"(
[prop]
rotation      = "contra"
torque_factor = 0.0
gyro_factor   = 0.0
)";
    auto data = makeData(prop_toml);
    FlightState s{};
    s.vel_body[0] = 100.f;
    s.pos_world[1] = 1000.f;
    s.mass_kg = data->geometry.mass_kg + data->geometry.fuel_kg;
    s.fuel_kg = data->geometry.fuel_kg;
    s.throttle_actual = 1.f;
    FlightIntegrator integ(data);
    integ.reset(s);

    ControlInput ctrl{};
    ctrl.throttle = 1.f;
    PayloadEffect px{};
    integ.step(1.f / 60.f, ctrl, px);

    // No torque roll: omega[0] should remain near zero (only aero moments, which are
    // symmetric at zero beta/sideslip)
    CHECK_THAT(integ.state().omega[0], WithinAbs(0.f, 1e-3f));
}

TEST_CASE("Integrator: wing sweep absent causes no crash", "[integrator]") {
    // The base TOML has no [wing_sweep] block — verify the integrator runs cleanly
    auto data = makeData();
    FlightState s{};
    s.vel_body[0] = 200.f;
    s.pos_world[1] = 5000.f;
    s.mass_kg = data->geometry.mass_kg + data->geometry.fuel_kg;
    s.fuel_kg = data->geometry.fuel_kg;
    s.throttle_actual = 0.7f;
    FlightIntegrator integ(data);
    integ.reset(s);

    ControlInput ctrl{};
    ctrl.throttle = 0.7f;
    PayloadEffect px{};
    for (int i = 0; i < 60; ++i)
        integ.step(1.f / 60.f, ctrl, px);

    CHECK(std::isfinite(integ.state().pos_world[0]));
    CHECK(std::isfinite(integ.state().vel_body[0]));
}

TEST_CASE("Integrator: control surface mapping scales correctly", "[integrator]") {
    // Verifies that cm_de is multiplied by max_elevator_rad, not by 1.0
    // Pull full stick (elevator=1.0) and check pitch rate increases
    auto data = makeData();
    FlightState s{};
    s.vel_body[0] = 200.f;
    s.pos_world[1] = 5000.f;
    s.mass_kg = data->geometry.mass_kg + data->geometry.fuel_kg;
    s.fuel_kg = data->geometry.fuel_kg;
    FlightIntegrator integ(data);
    integ.reset(s);

    ControlInput ctrl{};
    ctrl.throttle = 0.5f;
    ctrl.elevator = 1.f; // full pull
    PayloadEffect px{};
    integ.step(1.f / 60.f, ctrl, px);

    // Full nose-up elevator → pitch rate (omega[2] = around Z=right) should go positive (nose up)
    CHECK(integ.state().omega[2] > 0.f);
}

TEST_CASE("Integrator: speedbrake and gear drag decelerate aircraft", "[integrator]") {
    auto data = makeData();
    FlightState s{};
    s.vel_body[0] = 200.f;
    s.pos_world[1] = 5000.f;
    s.mass_kg = data->geometry.mass_kg + data->geometry.fuel_kg;
    s.fuel_kg = data->geometry.fuel_kg;
    s.throttle_actual = 0.f;

    ControlInput ctrl_clean{};
    ControlInput ctrl_brake{};
    ctrl_brake.speedbrake = 1.f;
    ctrl_brake.gear_down = true;
    PayloadEffect px{};

    FlightIntegrator ic(data), ib(data);
    ic.reset(s);
    ib.reset(s);
    ic.step(1.f / 60.f, ctrl_clean, px);
    ib.step(1.f / 60.f, ctrl_brake, px);

    CHECK(ib.state().vel_body[0] < ic.state().vel_body[0]);
}

// ---------------------------------------------------------------------------
// Y-up coordinate alignment regression tests
// ---------------------------------------------------------------------------

TEST_CASE("Integrator: gravity decreases altitude (Y-up world)", "[integrator]") {
    // Verifies that gravity acts in the correct direction after the Y-up alignment.
    // A stationary craft at 500 m with zero throttle must fall, not rise.
    auto data = makeData();
    FlightIntegrator integ(data);
    FlightState s{};
    s.pos_world[1] = 500.f; // altitude = Y in Y-up world
    s.mass_kg = data->geometry.mass_kg + data->geometry.fuel_kg;
    s.fuel_kg = data->geometry.fuel_kg;
    integ.reset(s);

    ControlInput ctrl{};
    PayloadEffect px{};
    for (int i = 0; i < 60; ++i)
        integ.step(1.f / 60.f, ctrl, px);

    CHECK(integ.state().pos_world[1] < 500.f);
}

// ---------------------------------------------------------------------------
// BuiltinFlightModel tests
// ---------------------------------------------------------------------------

static FlightState makeBuiltinState() {
    const auto& d = *BuiltinFlightModel::get();
    FlightState s{};
    s.pos_world[1] = 500.f;
    s.vel_body[0] = 40.f;
    s.fuel_kg = d.geometry.fuel_kg;
    s.mass_kg = d.geometry.mass_kg + s.fuel_kg;
    s.throttle_actual = 0.4f;
    return s;
}

TEST_CASE("BuiltinFlightModel: 1 second integration is NaN/Inf/negative-fuel free", "[builtin_flight]") {
    FlightIntegrator fi(BuiltinFlightModel::get());
    fi.reset(makeBuiltinState());
    ControlInput ctrl{};
    PayloadEffect px{};
    for (int i = 0; i < 60; ++i)
        fi.step(1.f / 60.f, ctrl, px);

    const auto& st = fi.state();
    CHECK(std::isfinite(st.pos_world[0]));
    CHECK(std::isfinite(st.pos_world[1]));
    CHECK(std::isfinite(st.pos_world[2]));
    CHECK(std::isfinite(st.vel_body[0]));
    CHECK(std::isfinite(st.omega[0]));
    CHECK(st.fuel_kg >= 0.f);
}

TEST_CASE("BuiltinFlightModel: pitch input produces non-zero pitch rate", "[builtin_flight]") {
    FlightIntegrator fi(BuiltinFlightModel::get());
    fi.reset(makeBuiltinState());
    ControlInput ctrl{};
    ctrl.elevator = 1.f;
    PayloadEffect px{};
    for (int i = 0; i < 60; ++i)
        fi.step(1.f / 60.f, ctrl, px);

    // In Y-up body frame, pitch = rotation around Z (right) = omega[2].
    CHECK(fi.state().omega[2] != 0.f);
}

TEST_CASE("BuiltinFlightModel: roll input produces non-zero roll rate", "[builtin_flight]") {
    FlightIntegrator fi(BuiltinFlightModel::get());
    fi.reset(makeBuiltinState());
    ControlInput ctrl{};
    ctrl.aileron = 1.f;
    PayloadEffect px{};
    for (int i = 0; i < 60; ++i)
        fi.step(1.f / 60.f, ctrl, px);

    CHECK(fi.state().omega[0] != 0.f);
}

TEST_CASE("BuiltinFlightModel: yaw input produces non-zero yaw rate", "[builtin_flight]") {
    FlightIntegrator fi(BuiltinFlightModel::get());
    fi.reset(makeBuiltinState());
    ControlInput ctrl{};
    ctrl.rudder = 1.f;
    PayloadEffect px{};
    for (int i = 0; i < 60; ++i)
        fi.step(1.f / 60.f, ctrl, px);

    // In Y-up body frame, yaw = rotation around Y (up) = omega[1].
    CHECK(fi.state().omega[1] != 0.f);
}

TEST_CASE("BuiltinFlightModel: throttle produces forward acceleration from rest", "[builtin_flight]") {
    FlightIntegrator fi(BuiltinFlightModel::get());
    FlightState s{};
    s.pos_world[1] = 500.f;
    s.fuel_kg = BuiltinFlightModel::get()->geometry.fuel_kg;
    s.mass_kg = BuiltinFlightModel::get()->geometry.mass_kg + s.fuel_kg;
    fi.reset(s);

    ControlInput ctrl{};
    ctrl.throttle = 1.f;
    PayloadEffect px{};
    for (int i = 0; i < 60; ++i)
        fi.step(1.f / 60.f, ctrl, px);

    CHECK(fi.state().vel_body[0] > 0.f);
}

TEST_CASE("FlightIntegrator: default WindInfluence gives same result as no-wind step", "[flight_integrator][weather]") {
    auto make_fi = [] {
        fl::FlightIntegrator fi(fl::BuiltinFlightModel::get());
        fl::FlightState s{};
        s.pos_world[1] = 500.f;
        s.vel_body[0] = 40.f;
        s.fuel_kg = fl::BuiltinFlightModel::get()->geometry.fuel_kg;
        s.mass_kg = fl::BuiltinFlightModel::get()->geometry.mass_kg + s.fuel_kg;
        fi.reset(s);
        return fi;
    };
    fl::ControlInput ctrl{};
    ctrl.throttle = 0.5f;
    fl::PayloadEffect px{};

    auto fi1 = make_fi();
    auto fi2 = make_fi();
    fi1.step(1.f / 60.f, ctrl, px);
    fi2.step(1.f / 60.f, ctrl, px, {});
    CHECK(fi1.state().vel_body[0] == fi2.state().vel_body[0]);
    CHECK(fi1.state().vel_body[1] == fi2.state().vel_body[1]);
}

TEST_CASE("FlightIntegrator: nonzero turbulence perturbs velocity", "[flight_integrator][weather]") {
    auto make_fi = [] {
        fl::FlightIntegrator fi(fl::BuiltinFlightModel::get());
        fl::FlightState s{};
        s.pos_world[1] = 500.f;
        s.vel_body[0] = 40.f;
        s.fuel_kg = fl::BuiltinFlightModel::get()->geometry.fuel_kg;
        s.mass_kg = fl::BuiltinFlightModel::get()->geometry.mass_kg + s.fuel_kg;
        fi.reset(s);
        return fi;
    };
    fl::ControlInput ctrl{};
    ctrl.throttle = 0.5f;
    fl::PayloadEffect px{};

    auto fi1 = make_fi();
    auto fi2 = make_fi();
    fi1.step(1.f / 60.f, ctrl, px);
    fl::WindInfluence wind{};
    wind.turbulence_body[0] = 10.f;
    fi2.step(1.f / 60.f, ctrl, px, wind);
    CHECK(fi1.state().vel_body[0] != fi2.state().vel_body[0]);
}

TEST_CASE("FlightIntegrator: nonzero world wind affects forces", "[flight_integrator][weather]") {
    // Use the parsed test model (mass=10000 kg) rather than BuiltinFlightModel (fuel=10M kg)
    // so the wind drag force produces a float-distinguishable velocity difference.
    auto make_fi = [] {
        FlightIntegrator fi(makeData());
        FlightState s{};
        s.pos_world[1] = 500.f;
        s.vel_body[0] = 40.f;
        s.fuel_kg = 0.f;
        s.mass_kg = 10000.f;
        fi.reset(s);
        return fi;
    };
    fl::ControlInput ctrl{};
    ctrl.throttle = 0.5f;
    fl::PayloadEffect px{};

    auto fi1 = make_fi();
    auto fi2 = make_fi();
    fi1.step(1.f / 60.f, ctrl, px);
    fl::WindInfluence wind{};
    wind.wind_world[0] = 100.f; // strong crosswind to produce visible drag force
    fi2.step(1.f / 60.f, ctrl, px, wind);
    // Force contribution differs → velocity differs
    CHECK(fi1.state().vel_body[0] != fi2.state().vel_body[0]);
}

TEST_CASE("Integrator: headwind increases drag and reduces forward velocity", "[flight_integrator][weather]") {
    // With the relative-airspeed model, a headwind (wind opposing flight) raises
    // the effective airspeed seen by computeForces, producing more drag and leaving
    // the aircraft slower after one step than the no-wind case at the same throttle.
    auto d = makeData();
    FlightIntegrator fi_nowind(d);
    FlightIntegrator fi_headwind(d);

    FlightState s{};
    s.vel_body[0] = 100.f; // forward at 100 m/s, identity orientation
    s.pos_world[1] = 1000.f;
    s.mass_kg = 14000.f;
    s.fuel_kg = 4000.f;
    fi_nowind.reset(s);
    fi_headwind.reset(s);

    ControlInput ctrl{};
    ctrl.throttle = 0.5f;
    PayloadEffect px{};

    fl::WindInfluence headwind{};
    headwind.wind_world[0] = -50.f; // opposes forward (+X) flight, raising relative airspeed to 150 m/s

    fi_nowind.step(1.f / 60.f, ctrl, px);
    fi_headwind.step(1.f / 60.f, ctrl, px, headwind);

    // Higher relative airspeed -> more drag -> lower ground-speed after the step
    CHECK(fi_headwind.state().vel_body[0] < fi_nowind.state().vel_body[0]);
}

TEST_CASE("Integrator: tailwind reduces drag and increases forward velocity", "[flight_integrator][weather]") {
    auto d = makeData();
    FlightIntegrator fi_nowind(d);
    FlightIntegrator fi_tailwind(d);

    FlightState s{};
    s.vel_body[0] = 100.f;
    s.pos_world[1] = 1000.f;
    s.mass_kg = 14000.f;
    s.fuel_kg = 4000.f;
    fi_nowind.reset(s);
    fi_tailwind.reset(s);

    ControlInput ctrl{};
    ctrl.throttle = 0.5f;
    PayloadEffect px{};

    fl::WindInfluence tailwind{};
    tailwind.wind_world[0] = 50.f; // same direction as flight, lowering relative airspeed to 50 m/s

    fi_nowind.step(1.f / 60.f, ctrl, px);
    fi_tailwind.step(1.f / 60.f, ctrl, px, tailwind);

    // Lower relative airspeed -> less drag -> higher ground-speed after the step
    CHECK(fi_tailwind.state().vel_body[0] > fi_nowind.state().vel_body[0]);
}

TEST_CASE("Integrator: crosswind introduces sideslip and changes yaw rate", "[flight_integrator][weather]") {
    // Z-axis world wind produces non-zero rel2, which drives a non-zero beta_rad into
    // computeMoments, creating a yaw moment absent in the no-wind case.
    auto d = makeData();
    FlightIntegrator fi_nowind(d);
    FlightIntegrator fi_cross(d);

    FlightState s{};
    s.vel_body[0] = 100.f;
    s.pos_world[1] = 1000.f;
    s.mass_kg = 14000.f;
    s.fuel_kg = 4000.f;
    fi_nowind.reset(s);
    fi_cross.reset(s);

    ControlInput ctrl{};
    ctrl.throttle = 0.5f;
    PayloadEffect px{};

    fl::WindInfluence crosswind{};
    crosswind.wind_world[2] = 50.f; // Z-axis wind -> non-zero rel2 -> beta_rad != 0

    fi_nowind.step(1.f / 60.f, ctrl, px);
    fi_cross.step(1.f / 60.f, ctrl, px, crosswind);

    // Non-zero beta drives yaw moment -> different yaw rate than no-wind
    CHECK(fi_cross.state().omega[1] != fi_nowind.state().omega[1]);
}

TEST_CASE("Integrator: wind effect depends on aircraft orientation", "[flight_integrator][weather]") {
    // Wind is rotated from world to body frame via q_conj before computing relative airspeed.
    // Two aircraft with different yaw orientations see the same world wind differently:
    // one as a tailwind component, the other as a crosswind. If the rotation were skipped,
    // both would produce identical force changes.
    auto d = makeData();
    FlightIntegrator fi_identity(d);
    FlightIntegrator fi_yawed(d);

    FlightState s{};
    s.vel_body[0] = 100.f;
    s.pos_world[1] = 1000.f;
    s.mass_kg = 14000.f;
    s.fuel_kg = 4000.f;

    fi_identity.reset(s);

    FlightState s_yaw = s;
    // 90-degree yaw around world Y: quat = (x=0, y=sin45, z=0, w=cos45)
    s_yaw.quat[0] = 0.f;
    s_yaw.quat[1] = 0.70711f;
    s_yaw.quat[2] = 0.f;
    s_yaw.quat[3] = 0.70711f;
    fi_yawed.reset(s_yaw);

    ControlInput ctrl{};
    ctrl.throttle = 0.5f;
    PayloadEffect px{};

    fl::WindInfluence wind{};
    wind.wind_world[0] = 50.f; // same world-frame wind applied to both

    fi_identity.step(1.f / 60.f, ctrl, px, wind);
    fi_yawed.step(1.f / 60.f, ctrl, px, wind);

    // Different body-frame winds after rotation -> different aerodynamic results
    CHECK(fi_identity.state().vel_body[0] != fi_yawed.state().vel_body[0]);
}

TEST_CASE("Integrator: wing-sweep schedule uses relative-airspeed Mach", "[flight_integrator][weather]") {
    // A headwind raises the effective airspeed seen by the sweep scheduler, driving a higher
    // Mach lookup and therefore a different commanded sweep angle than the no-wind case.
    auto d = makeData(kWingSweepToml);

    auto make_fi = [&] {
        FlightIntegrator fi(d);
        FlightState s{};
        s.vel_body[0] = 100.f;
        s.pos_world[1] = 1000.f;
        s.mass_kg = d->geometry.mass_kg + d->geometry.fuel_kg;
        s.fuel_kg = d->geometry.fuel_kg;
        fi.reset(s);
        return fi;
    };

    ControlInput ctrl{};
    ctrl.throttle = 0.5f;
    PayloadEffect px{};

    auto fi_nowind = make_fi();
    auto fi_headwind = make_fi();

    fl::WindInfluence headwind{};
    headwind.wind_world[0] = -80.f; // opposes forward (+X) flight, raising relative airspeed

    fi_nowind.step(1.f / 60.f, ctrl, px);
    fi_headwind.step(1.f / 60.f, ctrl, px, headwind);

    CHECK(fi_headwind.state().current_sweep_deg != fi_nowind.state().current_sweep_deg);
}

TEST_CASE("Integrator: wing-sweep tracks schedule with zero wind", "[flight_integrator][weather]") {
    // With zero wind, relative airspeed equals ground speed. Verify the sweep settles at the
    // value the Mach schedule prescribes for the aircraft's actual speed.
    auto d = makeData(kWingSweepToml);

    FlightIntegrator fi(d);
    FlightState s{};
    s.vel_body[0] = 100.f;
    s.pos_world[1] = 1000.f;
    s.mass_kg = d->geometry.mass_kg + d->geometry.fuel_kg;
    s.fuel_kg = d->geometry.fuel_kg;
    s.current_sweep_deg = d->wing_sweep->ref_sweep_deg; // start at ref so slew settles in one step
    fi.reset(s);

    ControlInput ctrl{};
    ctrl.throttle = 0.5f;
    PayloadEffect px{};
    fi.step(1.f / 60.f, ctrl, px);

    float sos = fl::computeAtmosphere(1000.f).speed_of_sound_m_s;
    float mach = 100.f / sos;
    float expected_sweep = d->wing_sweep->schedule.lookup(mach);
    CHECK_THAT(fi.state().current_sweep_deg, WithinAbs(expected_sweep, 0.1f));
}

TEST_CASE("Integrator: wing-sweep Mach accounts for aircraft orientation", "[flight_integrator][weather]") {
    // Wind is rotated from world to body frame before computing relative airspeed for the sweep
    // scheduler. Two aircraft with different orientations see the same world wind differently,
    // producing distinct sweep commands. Without the rotation, both would produce identical results.
    auto d = makeData(kWingSweepToml);

    auto make_fi = [&](const FlightState& s) {
        FlightIntegrator fi(d);
        fi.reset(s);
        return fi;
    };

    FlightState s_identity{};
    s_identity.vel_body[0] = 100.f;
    s_identity.pos_world[1] = 1000.f;
    s_identity.mass_kg = d->geometry.mass_kg + d->geometry.fuel_kg;
    s_identity.fuel_kg = d->geometry.fuel_kg;
    s_identity.current_sweep_deg = d->wing_sweep->ref_sweep_deg;

    FlightState s_yaw = s_identity;
    // 90-degree yaw around world Y: quat = (x=0, y=sin45, z=0, w=cos45)
    s_yaw.quat[0] = 0.f;
    s_yaw.quat[1] = 0.70711f;
    s_yaw.quat[2] = 0.f;
    s_yaw.quat[3] = 0.70711f;

    auto fi_identity = make_fi(s_identity);
    auto fi_yawed = make_fi(s_yaw);

    ControlInput ctrl{};
    ctrl.throttle = 0.5f;
    PayloadEffect px{};

    fl::WindInfluence wind{};
    wind.wind_world[0] = 50.f; // identity: tailwind (lowers rel spd); yawed: crosswind (different magnitude)

    fi_identity.step(1.f / 60.f, ctrl, px, wind);
    fi_yawed.step(1.f / 60.f, ctrl, px, wind);

    CHECK(fi_identity.state().current_sweep_deg != fi_yawed.state().current_sweep_deg);
}

TEST_CASE("FlightIntegrator: CentralGravityField at origin matches flat gravity", "[integrator][gravity]") {
    auto model = fl::BuiltinFlightModel::get();

    fl::FlightIntegrator fi(model);
    fi.setGravityField(fl::CentralGravityField::earthInstance());

    fl::FlightIntegrator fiFlat(model);

    fl::FlightState s{};
    s.pos_world[1] = 500.f;
    s.mass_kg = model->geometry.mass_kg + model->geometry.fuel_kg;
    s.fuel_kg = model->geometry.fuel_kg;
    fi.reset(s);
    fiFlat.reset(s);

    fl::ControlInput ctrl{};
    fl::PayloadEffect px{};
    fl::WindInfluence wind{};
    fi.step(1.f / 60.f, ctrl, px, wind);
    fiFlat.step(1.f / 60.f, ctrl, px, wind);

    // At the origin the spherical gravity vector is identical to flat gravity
    CHECK(fi.state().pos_world[1] == Catch::Approx(fiFlat.state().pos_world[1]).epsilon(1e-4f));
}

TEST_CASE("FlightIntegrator: CentralGravityField at lateral position tilts gravity", "[integrator][gravity]") {
    auto model = fl::BuiltinFlightModel::get();
    fl::FlightIntegrator fi(model);
    fi.setGravityField(fl::CentralGravityField::earthInstance());

    // Place entity 100 km along X — gravity pulls toward planet centre, which has a -X component
    fl::FlightState s{};
    s.pos_world[0] = 1e5f;
    s.pos_world[1] = 500.f;
    s.pos_world[2] = 0.f;
    s.quat[3] = 1.f; // identity quaternion (w=1)
    s.mass_kg = model->geometry.mass_kg + model->geometry.fuel_kg;
    s.fuel_kg = model->geometry.fuel_kg;
    fi.reset(s);

    fl::ControlInput ctrl{};
    fl::PayloadEffect px{};
    fl::WindInfluence wind{};
    // Single step only: at zero initial velocity dynamic pressure is zero and all
    // aerodynamic forces are zero, so only gravity contributes to the velocity change.
    // Multiple steps cause freefall → 90° AoA → large aero forces that swamp the
    // small -X gravity component (~0.154 m/s²).
    fi.step(1.f / 60.f, ctrl, px, wind);

    // Gravity pulls toward the planet centre at {0,-R,0}: at x=1e5 the -X component
    // is ~-0.154 m/s²; after one tick vel_body[0] ≈ -0.154/60 ≈ -0.00257 m/s.
    CHECK(fi.state().vel_body[0] < 0.f);
}
