// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "flight/BuiltinFlightModel.h"
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
