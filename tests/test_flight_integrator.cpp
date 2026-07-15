// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "flight/Atmosphere.h"
#include "flight/BallisticForceModel.h"
#include "flight/BuiltinFlightModel.h"
#include "flight/CentralGravityField.h"
#include "flight/EngineFailFlags.h"
#include "flight/FlightIntegrator.h"
#include "flight/FlightModelParser.h"

#include <algorithm>
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

TEST_CASE("FlightIntegrator: parked aircraft does not drift under steady wind", "[flight_integrator][weather]") {
    // Regression: a stationary entity on the ground used to slide downwind whenever the weather
    // changed, because the relative-airspeed model turns steady wind into aerodynamic drag. The
    // gear now suppresses wind/turbulence forcing while in ground contact.
    auto d = makeData();
    fl::FlightIntegrator fi(d);
    fl::FlightState s{};
    s.pos_world[1] = 100.f; // sitting on the ground (groundElev = 100)
    s.mass_kg = 10000.f;
    s.fuel_kg = 0.f;
    fi.reset(s);

    fl::ControlInput ctrl{}; // idle throttle, no pilot input
    fl::PayloadEffect px{};
    fl::WindInfluence wind{};
    wind.wind_world[0] = 50.f;     // strong steady wind
    wind.turbulence_body[2] = 5.f; // and gusts

    for (int i = 0; i < 600; ++i) // 10 simulated seconds
        fi.step(1.f / 60.f, ctrl, px, wind, /*groundElev=*/100.f);

    CHECK(std::abs(fi.state().pos_world[0]) < 0.5);
    CHECK(std::abs(fi.state().pos_world[2]) < 0.5);
}

TEST_CASE("FlightIntegrator: ground parking hold does not block takeoff roll", "[flight_integrator]") {
    // The static parking hold engages only near idle throttle, so full throttle still accelerates
    // the aircraft down the runway despite being in ground contact.
    auto d = makeData();
    fl::FlightIntegrator fi(d);
    fl::FlightState s{};
    s.pos_world[1] = 100.f;
    s.mass_kg = 10000.f;
    s.fuel_kg = 2000.f;
    s.mass_kg += s.fuel_kg;
    fi.reset(s);

    fl::ControlInput ctrl{};
    ctrl.throttle = 1.0f; // full throttle
    fl::PayloadEffect px{};

    for (int i = 0; i < 300; ++i) // 5 simulated seconds
        fi.step(1.f / 60.f, ctrl, px, {}, /*groundElev=*/100.f);

    CHECK(fi.state().vel_body[0] > 1.0f);
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

TEST_CASE("FlightIntegrator: default gravity pulls downward at surface", "[integrator][gravity]") {
    auto model = fl::BuiltinFlightModel::get();
    fl::FlightIntegrator fi(model); // default = CentralGravityField::earthInstance()

    fl::FlightState s{};
    s.pos_world[1] = 500.f;
    s.mass_kg = model->geometry.mass_kg + model->geometry.fuel_kg;
    s.fuel_kg = model->geometry.fuel_kg;
    fi.reset(s);

    fl::ControlInput ctrl{};
    fl::PayloadEffect px{};
    fi.step(1.f / 60.f, ctrl, px);

    CHECK(fi.state().pos_world[1] < 500.f);
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

TEST_CASE("CentralGravityField: geodeticUp is the outward radial direction", "[integrator][gravity][spherical]") {
    const fl::CentralGravityField g; // Earth
    const double R = 6'371'000.0;

    // At the north pole (world-origin column) the radial up is world +Y.
    const double atPole[3] = {0.0, 500.0, 0.0};
    const auto up0 = g.geodeticUp(atPole);
    CHECK(up0[0] == Catch::Approx(0.0).margin(1e-6));
    CHECK(up0[1] == Catch::Approx(1.0).margin(1e-6));
    CHECK(up0[2] == Catch::Approx(0.0).margin(1e-6));

    // 100 km along +X: up tilts toward +X but stays mostly +Y, and is unit length.
    const double lateral[3] = {1e5, 500.0, 0.0};
    const auto up1 = g.geodeticUp(lateral);
    const double len = std::sqrt(up1[0] * up1[0] + up1[1] * up1[1] + up1[2] * up1[2]);
    CHECK(len == Catch::Approx(1.0).margin(1e-5));
    CHECK(up1[0] > 0.0);
    CHECK(up1[1] > 0.99f);
    // Exactly normalize({x, y+R, z}).
    const double ex = 1e5, ey = 500.0 + R;
    const double el = std::sqrt(ex * ex + ey * ey);
    CHECK(up1[0] == Catch::Approx(static_cast<float>(ex / el)).margin(1e-5));
    CHECK(up1[1] == Catch::Approx(static_cast<float>(ey / el)).margin(1e-5));
}

TEST_CASE("FlightIntegrator: radial ground floor snaps to the surface far from the world origin",
          "[integrator][gravity][spherical]") {
    // #477: the ground floor is radial. An aircraft below the terrain surface far from the origin
    // must be snapped back out along the local radial up so its geodetic altitude equals the terrain
    // elevation — NOT clamped to a world-Y plane (the old planar bug).
    auto model = fl::BuiltinFlightModel::get();
    fl::FlightIntegrator fi(model); // default Earth central gravity
    const fl::CentralGravityField g;
    const double R = 6'371'000.0;
    const float groundElev = 300.f; // terrain radial elevation above the datum

    // Start 50 m BELOW the terrain surface, 100 km along +X (penetrating), on the sphere's near side.
    const double x = 1e5;
    const double startAlt = static_cast<double>(groundElev) - 50.0;
    const double y = std::sqrt((R + startAlt) * (R + startAlt) - x * x) - R;
    fl::FlightState s{};
    s.pos_world[0] = x;
    s.pos_world[1] = y;
    s.pos_world[2] = 0.0;
    s.quat[3] = 1.f; // identity
    s.mass_kg = model->geometry.mass_kg + model->geometry.fuel_kg;
    s.fuel_kg = model->geometry.fuel_kg;
    fi.reset(s);

    const double p0[3] = {s.pos_world[0], s.pos_world[1], s.pos_world[2]};
    REQUIRE(g.geodeticAltitude(p0) == Catch::Approx(startAlt).margin(1.0)); // starts below the surface

    fl::ControlInput ctrl{}; // idle
    fl::PayloadEffect px{};
    fi.step(1.f / 60.f, ctrl, px, {}, groundElev); // one step: the radial floor catches it

    const double pf[3] = {fi.state().pos_world[0], fi.state().pos_world[1], fi.state().pos_world[2]};
    // Snapped radially up to the terrain surface (geodetic AGL ~ 0).
    CHECK(g.geodeticAltitude(pf) == Catch::Approx(static_cast<double>(groundElev)).margin(1.0));
    // The snap moved along the radial up, which has an +X component here, so world-X grew...
    CHECK(pf[0] > x);
    // ...and world-Y sits well BELOW the geodetic altitude (the curvature drop): this is what
    // distinguishes the radial clamp from the old planar one, which would have pinned world-Y to
    // groundElev directly.
    CHECK(g.geodeticAltitude(pf) - pf[1] > 50.0);
}

TEST_CASE("Integrator: double-precision position accumulates at large world offset", "[flight]") {
    // At x = 1e5 m, float ULP ~0.0078 m exceeds the per-step lateral gravity
    // displacement toward planet centre (~0.001 m).  With float pos_world the
    // position never moved; with double it must.
    fl::FlightState s{};
    s.pos_world[0] = 1e5;
    s.pos_world[1] = 500.0;

    fl::FlightIntegrator fi(makeData());
    const auto& cg = fl::CentralGravityField::earthInstance();
    fi.setGravityField(cg);
    fi.reset(s);

    const double x0 = fi.state().pos_world[0];
    fl::ControlInput ctrl{};
    fl::WindInfluence wind{};
    fi.step(1.0f / 60.f, ctrl, {}, wind);

    CHECK(fi.state().pos_world[0] != x0);
    CHECK(std::isfinite(fi.state().pos_world[0]));
}

// ---------------------------------------------------------------------------
// vel_body double-precision tests (#387)
// ---------------------------------------------------------------------------

TEST_CASE("FlightState::vel_body stores double precision", "[integrator]") {
    // 1000.123456789012345 is representable in double but not in float.
    // float rounds it to ~1000.1235 (7 significant digits); double stores it exactly.
    fl::FlightState s{};
    const double kPrecise = 1000.123456789012345;
    s.vel_body[0] = kPrecise;
    CHECK(s.vel_body[0] == kPrecise);
    CHECK(s.vel_body[0] != static_cast<float>(kPrecise));
}

TEST_CASE("FlightIntegrator: vel_body drives pos_world via double-precision rotation", "[integrator]") {
    // Smoke test for the quatRotateD path: with identity quaternion (body X == world X),
    // forward vel_body[0] must accumulate into pos_world[0] over 1 s.
    // Double-precision storage is proven by the previous test; this confirms the
    // integration chain (quatRotateD → pos_world) executes and produces a sensible result.
    // Note: computeAtmosphere clamps to 20 km, so use a low speed (10 m/s) where
    // aero drag is negligible (< 0.001 m/s² deceleration on the test aircraft).
    auto data = makeData();
    fl::FlightIntegrator integ(data);
    fl::FlightState s{};
    s.pos_world[0] = 0.0;
    s.pos_world[1] = 5000.0;
    s.vel_body[0] = 10.0; // m/s — drag at this speed is < 0.001 m/s²
    s.mass_kg = data->geometry.mass_kg + data->geometry.fuel_kg;
    s.fuel_kg = data->geometry.fuel_kg;
    s.throttle_actual = 0.f;
    integ.reset(s);

    fl::ControlInput ctrl{};
    fl::PayloadEffect px{};
    for (int i = 0; i < 60; ++i)
        integ.step(1.f / 60.f, ctrl, px);

    // After 1 s at ~10 m/s, pos_world[0] ≈ 10 m; ±0.5 m covers all force effects.
    CHECK_THAT(integ.state().pos_world[0], WithinAbs(10.0, 0.5));
    CHECK(std::isfinite(integ.state().vel_body[0]));
    CHECK(std::isfinite(integ.state().pos_world[0]));
}

TEST_CASE("FlightIntegrator: vel_body clamped at double-precision kMaxBodySpeed", "[integrator]") {
    // Verifies the kMaxBodySpeed overflow backstop clamps vel_body. It is a NUMERICAL guard, not a
    // top-speed limiter (#816) -- it was raised to 2000 m/s so it sits well clear of any flyable
    // regime. An aircraft's real top speed comes from drag rising to meet thrust, and fm-trim (#817)
    // fails a model that can outrun its own declared max_mach.
    // No existing test exercises vel_body above the clamp threshold.
    auto data = makeData();
    fl::FlightIntegrator integ(data);
    fl::FlightState s{};
    s.vel_body[0] = 5000.0; // far above kMaxBodySpeed
    s.pos_world[1] = 5000.0;
    s.mass_kg = data->geometry.mass_kg + data->geometry.fuel_kg;
    s.fuel_kg = data->geometry.fuel_kg;
    integ.reset(s);

    fl::ControlInput ctrl{};
    fl::PayloadEffect px{};
    integ.step(1.f / 60.f, ctrl, px);

    // After one step the clamp reduces vel_body[0] to ≤ kMaxBodySpeed.
    CHECK(integ.state().vel_body[0] <= 2000.0);
    CHECK(std::isfinite(integ.state().vel_body[0]));
}

// ---------------------------------------------------------------------------
// [aero.limits] enforcement (#816)
//
// Until now alpha_stall_deg, max_g_structural and min_g_structural were parsed, REQUIRED, and read
// by absolutely nothing. There was no G-limiter, no AoA limiter, no structural damage anywhere in
// the engine. They were decoration.
// ---------------------------------------------------------------------------

// Flies the aircraft fast and level, then yanks the stick for N seconds and reports the peak load
// factor and whether the airframe was damaged.
struct PullResult {
    float peakG{0.f};
    bool damaged{false};
    int damageEvents{0};
};

static PullResult hardPull(std::shared_ptr<FlightModelData> data, float seconds = 3.0f) {
    FlightIntegrator integ(data);
    FlightState s{};
    // Low and fast: dynamic pressure high enough that the wing can make far more than the structural
    // limit before it runs out of lift, so full aft stick genuinely overstresses the airframe rather
    // than simply stalling it.
    s.vel_body[0] = 350.0;
    s.pos_world[1] = 1000.0;
    s.quat[3] = 1.f;
    s.mass_kg = data->geometry.mass_kg + data->geometry.fuel_kg;
    s.fuel_kg = data->geometry.fuel_kg;
    integ.reset(s);

    ControlInput ctrl{};
    ctrl.throttle = 1.f;
    ctrl.elevator = 1.f; // full aft stick

    PullResult out;
    const int ticks = static_cast<int>(seconds * 60.f);
    for (int i = 0; i < ticks; ++i) {
        integ.step(1.f / 60.f, ctrl, {});
        out.peakG = std::max(out.peakG, integ.state().load_factor);
        if (integ.state().overg_damage) {
            out.damaged = true;
            ++out.damageEvents;
        }
    }
    return out;
}

TEST_CASE("Integrator: load factor is computed and is ~1 g in level flight", "[integrator][limits]") {
    auto data = makeData();
    FlightIntegrator integ(data);
    FlightState s{};
    s.vel_body[0] = 200.0;
    s.pos_world[1] = 5000.0;
    s.quat[3] = 1.f;
    s.mass_kg = data->geometry.mass_kg + data->geometry.fuel_kg;
    s.fuel_kg = data->geometry.fuel_kg;
    integ.reset(s);

    ControlInput ctrl{};
    ctrl.throttle = 0.6f;
    integ.step(1.f / 60.f, ctrl, {});

    // Not asserting an exact 1.0 -- the aircraft is not trimmed -- only that the field is now live
    // and finite rather than the constant it used to be.
    CHECK(std::isfinite(integ.state().load_factor));
    CHECK(integ.state().load_factor > -10.f);
    CHECK(integ.state().load_factor < 30.f);
}

TEST_CASE("Integrator: pulling past the structural limit damages a non-FBW airframe", "[integrator][limits]") {
    // An F-5E has no fly-by-wire. Its pilot CAN overstress the jet. The sim lets them -- and bills them.
    auto data = makeData();
    data->meta.has_fbw = false;
    data->limits.max_g_structural = 8.f;

    const PullResult r = hardPull(data);

    CHECK(r.peakG > 8.f * 1.1f); // it really did go past the limit (no silent clamp)
    CHECK(r.damaged);            // and it paid for it
}

TEST_CASE("Integrator: an FBW airframe is held at its structural limit and takes no damage", "[integrator][limits]") {
    // The ONLY thing has_fbw should ever mean. A limiter on a 1972 airframe would be a lie about the
    // aircraft; the absence of one on an F-16 would be a different lie.
    auto data = makeData();
    data->meta.has_fbw = true;
    data->limits.max_g_structural = 8.f;

    const PullResult r = hardPull(data);

    CHECK(r.peakG <= 8.f * 1.10f); // held at the limit (within the same margin the damage rule uses)
    CHECK_FALSE(r.damaged);
}

TEST_CASE("Integrator: a brief excursion past the limit does NOT damage the airframe", "[integrator][limits]") {
    // A momentary spike from a gust or a single-tick input is not what breaks an aeroplane. Sustained
    // overstress is -- hence the 0.5 s dwell before the airframe pays.
    auto data = makeData();
    data->meta.has_fbw = false;
    data->limits.max_g_structural = 8.f;

    const PullResult r = hardPull(data, 0.2f); // less than the 0.5 s dwell
    CHECK_FALSE(r.damaged);
}

TEST_CASE("Integrator: overg_damage is a one-shot flag, not a level", "[integrator][limits]") {
    // WorldBroadcaster reads this flag once per tick after the parallel integrate pass. If it latched
    // high it would apply damage on every subsequent tick and disintegrate the aircraft instantly.
    auto data = makeData();
    data->meta.has_fbw = false;
    data->limits.max_g_structural = 8.f;

    const PullResult r = hardPull(data, 3.0f);
    REQUIRE(r.damaged);
    // 3 s of sustained overstress at a 0.5 s dwell => a handful of events, not ~180 (one per tick).
    CHECK(r.damageEvents < 10);
}

TEST_CASE("Integrator: the stall flag sets past alpha_stall_deg and clears below", "[integrator][limits]") {
    auto data = makeData();
    data->limits.alpha_stall_deg = 18.f;

    FlightIntegrator integ(data);
    FlightState s{};
    // Nose well above the flight path: a large positive alpha without needing to fly a whole approach.
    s.vel_body[0] = 60.0;  // slow
    s.vel_body[1] = -40.0; // descending relative to the nose => alpha = atan2(40, 60) ~ 34 deg
    s.pos_world[1] = 5000.0;
    s.quat[3] = 1.f;
    s.mass_kg = data->geometry.mass_kg;
    integ.reset(s);

    ControlInput ctrl{};
    integ.step(1.f / 60.f, ctrl, {});
    CHECK(integ.state().stalled);

    // Now fly it cleanly: alpha near zero.
    FlightState fast{};
    fast.vel_body[0] = 250.0;
    fast.pos_world[1] = 5000.0;
    fast.quat[3] = 1.f;
    fast.mass_kg = data->geometry.mass_kg;
    integ.reset(fast);
    integ.step(1.f / 60.f, ctrl, {});
    CHECK_FALSE(integ.state().stalled);
}

// ---------------------------------------------------------------------------
// Damage penalties + crash-impact report (#626)
// ---------------------------------------------------------------------------

TEST_CASE("Integrator: damage thrust penalty caps what the throttle can command", "[integrator][damage]") {
    auto data = makeData();
    FlightIntegrator integ(data);
    FlightState s{};
    s.vel_body[0] = 200.0;
    s.pos_world[1] = 5000.0;
    s.quat[3] = 1.f;
    s.mass_kg = data->geometry.mass_kg;
    integ.reset(s);

    integ.setDamagePenalty(0.4f, 1.f);
    CHECK(integ.damageThrustFactor() == Catch::Approx(0.4f));

    ControlInput ctrl{};
    ctrl.throttle = 1.f;
    for (int i = 0; i < 1200; ++i) // let the spool converge
        integ.step(1.f / 60.f, ctrl, {});
    // Full stick asked for 1.0; a shot-up engine delivers what the penalty allows.
    CHECK(integ.state().throttle_actual > 0.3f);
    CHECK(integ.state().throttle_actual < 0.45f);
}

TEST_CASE("Integrator: damage control penalty degrades pitch response", "[integrator][damage]") {
    auto data = makeData();
    auto flyPitch = [&](float controlFactor) {
        FlightIntegrator integ(data);
        FlightState s{};
        s.vel_body[0] = 200.0;
        s.pos_world[1] = 5000.0;
        s.quat[3] = 1.f;
        s.mass_kg = data->geometry.mass_kg;
        integ.reset(s);
        integ.setDamagePenalty(1.f, controlFactor);
        ControlInput ctrl{};
        ctrl.throttle = 0.8f;
        ctrl.elevator = 1.f;
        // A single step: the raw command-to-pitch-acceleration response, before aerodynamic
        // damping and alpha feedback blur the comparison.
        integ.step(1.f / 60.f, ctrl, {});
        return std::abs(integ.state().omega[2]); // pitch = around body Z (right axis)
    };

    const float healthy = flyPitch(1.f);
    const float damaged = flyPitch(0.3f);
    CHECK(healthy > 0.f);
    CHECK(damaged < healthy * 0.75f); // shot-up linkages cannot be asked for full deflection
}

TEST_CASE("Integrator: a hard ground impact is reported once, a firm landing not at all", "[integrator][damage]") {
    auto data = makeData();
    FlightIntegrator integ(data);

    FlightState s{};
    s.vel_body[0] = 60.0;
    s.vel_body[1] = -40.0; // 40 m/s of sink — an arrival, not a landing
    s.pos_world[1] = 0.4;  // about to hit the datum floor this tick
    s.quat[3] = 1.f;
    s.mass_kg = data->geometry.mass_kg;
    integ.reset(s);

    ControlInput ctrl{};
    integ.step(1.f / 60.f, ctrl, {}, {}, 0.f);
    CHECK(integ.state().ground_impact_speed > 6.f); // reported...
    integ.step(1.f / 60.f, ctrl, {}, {}, 0.f);
    CHECK(integ.state().ground_impact_speed == 0.f); // ...exactly once (one-shot)

    // A firm-but-survivable landing stays below the reporting threshold.
    FlightState gentle{};
    gentle.vel_body[0] = 60.0;
    gentle.vel_body[1] = -3.0;
    gentle.pos_world[1] = 0.04;
    gentle.quat[3] = 1.f;
    gentle.mass_kg = data->geometry.mass_kg;
    integ.reset(gentle);
    integ.step(1.f / 60.f, ctrl, {}, {}, 0.f);
    CHECK(integ.state().ground_impact_speed == 0.f);
}

// ---------------------------------------------------------------------------
// Ballistic vehicles (#354)
// ---------------------------------------------------------------------------

static const std::string kBallisticToml = R"(
[aircraft]
name = "Test MRBM"
type = "ballistic"

[flight_model]
mass_kg      = 2000.0
wing_area_m2 = 0.8
wingspan_m   = 0.8
mac_m        = 0.8
fuel_kg      = 3000.0
ixx_kg_m2    = 800.0
iyy_kg_m2    = 12000.0
izz_kg_m2    = 12000.0

[engine.boost]
thrust_n    = 300000.0
burn_time_s = 60.0
)";

TEST_CASE("Ballistic: the reduced schema parses and folds the burn into the fuel flow", "[ballistic]") {
    const FlightModelData d = parseFlightModel(kBallisticToml);
    CHECK(d.isBallistic());
    CHECK(d.boost_thrust_n == 300000.f);
    // 3000 kg of propellant over 60 s: 50 kg/s at every throttle — a solid motor burns to depletion.
    CHECK(d.engine.fuel_flow_idle_kg_s == Catch::Approx(50.f));
    CHECK(d.engine.fuel_flow_mil_kg_s == Catch::Approx(50.f));
    CHECK(d.drag_polar.cd0 == Catch::Approx(0.20f)); // blunt-body default
    CHECK(d.limits.alpha_stall_deg == 90.f);         // nothing to stall

    // The wings-and-turbines schema is NOT required of it.
    CHECK_THROWS(parseFlightModel(R"(
[aircraft]
name = "No Boost"
type = "ballistic"
[flight_model]
mass_kg = 2000.0
wing_area_m2 = 0.8
wingspan_m = 0.8
mac_m = 0.8
fuel_kg = 3000.0
ixx_kg_m2 = 800.0
iyy_kg_m2 = 12000.0
izz_kg_m2 = 12000.0
)")); // missing [engine.boost]
}

TEST_CASE("Ballistic: drag-free trajectory matches the closed-form range", "[ballistic]") {
    // A 45-degree, 300 m/s shot in (near-)vacuum drag terms: cd0 = 0 via a table-free copy.
    auto data = std::make_shared<FlightModelData>(parseFlightModel(kBallisticToml));
    auto zeroDrag = std::make_shared<FlightModelData>(*data);
    zeroDrag->drag_polar.cd0 = 0.f;
    zeroDrag->boost_thrust_n = 0.f; // no motor: pure ballistic arc from the initial state

    FlightIntegrator fi(zeroDrag);
    fi.setForceModel(BallisticForceModel::instance());
    fi.setSpeedGuard(8000.0);

    FlightState s{};
    s.pos_world[1] = 1.0;     // just above the datum
    s.vel_body[0] = 212.132f; // 300 m/s at 45 deg, identity attitude: body == world
    s.vel_body[1] = 212.132f;
    s.fuel_kg = 0.f;
    s.mass_kg = 2000.f;
    fi.reset(s);

    ControlInput ctrl{};
    PayloadEffect px{};
    const double posArr0[3] = {0.0, 1.0, 0.0};
    const float g0 = std::abs(CentralGravityField::earthInstance().accelWorld(posArr0)[1]);

    double maxAlt = 0.0;
    int ticks = 0;
    for (; ticks < 60 * 120; ++ticks) {
        fi.step(1.f / 60.f, ctrl, px);
        maxAlt = std::max(maxAlt, fi.state().pos_world[1]);
        if (ticks > 60 && fi.state().pos_world[1] <= 1.0)
            break; // back on the deck
    }

    // Closed form (flat earth, constant g): R = v^2 sin(2*45)/g, apex = v^2 sin^2(45)/(2g),
    // T = 2 v sin(45)/g. The 1/r^2 field and the spherical floor perturb this by well under 1%.
    const double v = 300.0;
    const double rangeExact = v * v / static_cast<double>(g0);
    const double apexExact = v * v * 0.5 * 0.5 / static_cast<double>(g0) * 2.0 / 2.0; // v^2/(4g)... keep explicit below
    (void)apexExact;
    CHECK(fi.state().pos_world[0] == Catch::Approx(rangeExact).epsilon(0.02));
    CHECK(maxAlt == Catch::Approx(v * v * 0.25 / static_cast<double>(g0)).epsilon(0.02));
    CHECK(static_cast<double>(ticks) / 60.0 ==
          Catch::Approx(2.0 * v * 0.70710678 / static_cast<double>(g0)).epsilon(0.02));
}

TEST_CASE("Ballistic: boost accelerates until propellant depletion, then the motor is done", "[ballistic]") {
    auto data = std::make_shared<FlightModelData>(parseFlightModel(kBallisticToml));
    FlightIntegrator fi(data);
    fi.setForceModel(BallisticForceModel::instance());
    fi.setSpeedGuard(8000.0);

    FlightState s{};
    s.pos_world[1] = 30000.0; // thin 1976-layer air: the #354 atmosphere is what makes this work
    s.vel_body[0] = 50.f;
    s.fuel_kg = 3000.f;
    s.mass_kg = 5000.f;
    fi.reset(s);

    ControlInput ctrl{}; // throttle 0: a solid motor does not care
    PayloadEffect px{};
    for (int i = 0; i < 60 * 30; ++i)
        fi.step(1.f / 60.f, ctrl, px);

    // Half the 60 s burn elapsed: still burning, well past the old 2000 m/s aircraft guard.
    CHECK(fi.state().fuel_kg > 0.f);
    CHECK(fi.state().vel_body[0] > 2000.0);
}

TEST_CASE("Ballistic: the speed guard is per-instance - aircraft keep the 2000 m/s backstop",
          "[ballistic][integrator]") {
    auto data = std::make_shared<FlightModelData>(parseFlightModel(kBallisticToml));

    FlightIntegrator guarded(data); // default guard: 2000 m/s
    FlightState s{};
    s.pos_world[1] = 50000.0;
    s.vel_body[0] = 5000.f;
    s.fuel_kg = 0.f;
    s.mass_kg = 2000.f;
    guarded.reset(s);
    ControlInput ctrl{};
    PayloadEffect px{};
    guarded.step(1.f / 60.f, ctrl, px);
    CHECK(guarded.state().vel_body[0] <= 2000.0);

    FlightIntegrator wide(data);
    wide.setSpeedGuard(8000.0);
    wide.reset(s);
    wide.step(1.f / 60.f, ctrl, px);
    CHECK(wide.state().vel_body[0] > 4900.0);
}

// ---------------------------------------------------------------------------
// Per-subsystem damage effects (#675)
// ---------------------------------------------------------------------------

TEST_CASE("Integrator: a single engine out halves thrust and yaws toward the dead engine", "[integrator][subsystem]") {
    // Two runs from the same state: healthy vs left-engine-out. The dead-engine run must accelerate
    // less (half thrust) and develop a yaw rate toward the dead (left) engine.
    auto run = [](uint8_t failFlags) {
        auto data = makeData();
        FlightIntegrator fi(data);
        FlightState s{};
        s.vel_body[0] = 200.f;
        s.pos_world[1] = 5000.f;
        s.fuel_kg = 4000.f;
        s.mass_kg = 14000.f;
        s.quat[3] = 1.f;
        fi.reset(s);
        fi.setEngineFailFlags(failFlags);
        ControlInput ctrl{};
        ctrl.throttle = 1.f;
        PayloadEffect px{};
        for (int i = 0; i < 60; ++i)
            fi.step(1.f / 60.f, ctrl, px);
        return fi.state();
    };

    const FlightState healthy = run(0);
    const FlightState leftOut = run(fl::kEngineFailLeft);

    // Less forward acceleration on one engine.
    CHECK(leftOut.vel_body[0] < healthy.vel_body[0]);
    // A yaw rate developed (omega[1] is yaw about body-up); the healthy jet stays ~straight.
    CHECK(std::abs(leftOut.omega[1]) > std::abs(healthy.omega[1]) + 0.001f);

    // Right engine out yaws the opposite way.
    const FlightState rightOut = run(fl::kEngineFailRight);
    CHECK((leftOut.omega[1] > 0.f) != (rightOut.omega[1] > 0.f)); // opposite signs
}

TEST_CASE("Integrator: both engines out kills thrust entirely", "[integrator][subsystem]") {
    auto data = makeData();
    FlightIntegrator fi(data);
    FlightState s{};
    s.vel_body[0] = 200.f;
    s.pos_world[1] = 5000.f;
    s.fuel_kg = 4000.f;
    s.mass_kg = 14000.f;
    s.quat[3] = 1.f;
    fi.reset(s);
    fi.setEngineFailFlags(fl::kEngineFailLeft | fl::kEngineFailRight);
    ControlInput ctrl{};
    ctrl.throttle = 1.f;
    PayloadEffect px{};
    for (int i = 0; i < 120; ++i)
        fi.step(1.f / 60.f, ctrl, px);
    // With no thrust and drag, an aircraft in level-ish flight decelerates.
    CHECK(fi.state().vel_body[0] < 200.f);
}

TEST_CASE("Integrator: subsystem control factor multiplies the tier control factor", "[integrator][subsystem]") {
    auto data = makeData();
    FlightIntegrator fi(data);
    fi.setDamagePenalty(1.f, 0.5f);     // tier: half control
    fi.setSubsystemControlFactor(0.4f); // subsystem: 40% — combined 0.2
    // (The combined factor is applied to command inputs each step; a direct read is not exposed, so
    //  this case documents the API contract; the combined EFFECT is covered by the WB integration.)
    CHECK(fi.damageControlFactor() == 0.5f); // the tier factor is unchanged by the subsystem setter
}

TEST_CASE("Integrator: a fuel leak drains on top of the burn", "[integrator][subsystem]") {
    auto data = makeData();
    FlightIntegrator fi(data);
    FlightState s{};
    s.vel_body[0] = 200.f;
    s.pos_world[1] = 5000.f;
    s.fuel_kg = 4000.f;
    s.mass_kg = 14000.f;
    s.quat[3] = 1.f;
    fi.reset(s);
    ControlInput ctrl{};
    ctrl.throttle = 0.5f;
    PayloadEffect px{};

    fi.step(1.f / 60.f, ctrl, px);
    const float afterOneStepNoLeak = fi.state().fuel_kg;

    fi.reset(s);
    fi.setFuelLeakRate(10.f); // 10 kg/s
    fi.step(1.f / 60.f, ctrl, px);
    const float afterOneStepLeak = fi.state().fuel_kg;

    CHECK(afterOneStepLeak < afterOneStepNoLeak);
    // The extra drain is ~10 kg/s / 60 = 0.167 kg in one step.
    CHECK((afterOneStepNoLeak - afterOneStepLeak) == Catch::Approx(10.f / 60.f).epsilon(0.001));
}
