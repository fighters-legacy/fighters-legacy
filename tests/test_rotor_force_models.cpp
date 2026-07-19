// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "flight/EngineFailFlags.h"
#include "flight/FlightIntegrator.h"
#include "flight/FlightModelParser.h"
#include "flight/ForceModelSelect.h"
#include "flight/Trim.h"

#include <cmath>
#include <memory>
#include <string>

using namespace fl;

// ── multirotor (#349) ────────────────────────────────────────────────────────

static const std::string kQuadToml = R"(
[aircraft]
name = "Test Quad"
type = "multirotor"

[flight_model]
mass_kg   = 12.0
fuel_kg   = 2.0
ixx_kg_m2 = 0.6
iyy_kg_m2 = 0.6
izz_kg_m2 = 1.0

[multirotor]
rotor_count        = 4
rotor_thrust_max_n = 60.0
rotor_arm_m        = 0.35
yaw_torque_nm      = 8.0
frame_cd           = 1.1
frame_area_m2      = 0.12
flight_time_min    = 25.0
)";

static std::shared_ptr<FlightModelData> makeQuad() {
    return std::make_shared<FlightModelData>(parseFlightModel(kQuadToml));
}

// A hovering-ish integrator: level attitude, given throttle, well clear of the ground.
static FlightIntegrator makeQuadAt(std::shared_ptr<FlightModelData> data, float throttle, float altM = 500.f) {
    FlightIntegrator fi(std::move(data));
    applyForceModelFor(fi, fi.flightModel());
    FlightState s{};
    s.pos_world[1] = altM;
    s.mass_kg = fi.flightModel().geometry.mass_kg + fi.flightModel().geometry.fuel_kg;
    s.fuel_kg = fi.flightModel().geometry.fuel_kg;
    s.throttle_actual = throttle;
    fi.reset(s);
    return fi;
}

TEST_CASE("Multirotor: [multirotor] parses into the reduced schema", "[multirotor]") {
    auto d = makeQuad();
    REQUIRE(d->multirotor.has_value());
    CHECK(d->isMultirotor());
    CHECK(d->isRotorcraft());
    CHECK_FALSE(d->isFixedWing());
    CHECK(d->multirotor->rotor_count == 4);
    CHECK(d->multirotor->rotor_thrust_max_n == Catch::Approx(60.f));
    // Endurance became a constant fuel drain: 2 kg over 25 min.
    CHECK(d->engine.fuel_flow_mil_kg_s == Catch::Approx(2.f / (25.f * 60.f)));
    // Electric motor response, not a turbine spool.
    CHECK(d->engine.spool_time_s == Catch::Approx(0.2f));
}

TEST_CASE("Multirotor: missing [multirotor] table is rejected", "[multirotor]") {
    const auto pos = kQuadToml.find("[multirotor]");
    REQUIRE(pos != std::string::npos);
    CHECK_THROWS(parseFlightModel(kQuadToml.substr(0, pos)));
}

TEST_CASE("Multirotor: full throttle climbs, idle falls", "[multirotor]") {
    ControlInput full{};
    full.throttle = 1.f;
    auto climb = makeQuadAt(makeQuad(), 1.f);
    for (int i = 0; i < 60; ++i)
        climb.step(1.f / 60.f, full, {});
    CHECK(climb.state().vel_body[1] > 1.0); // T/W ~1.7: strong climb

    ControlInput idle{};
    idle.throttle = 0.1f; // above the parking-hold gate, far below hover
    auto fall = makeQuadAt(makeQuad(), 0.1f);
    for (int i = 0; i < 60; ++i)
        fall.step(1.f / 60.f, idle, {});
    CHECK(fall.state().vel_body[1] < -1.0);
}

TEST_CASE("Multirotor: hover throttle balances weight", "[multirotor]") {
    auto data = makeQuad();
    // Density-scaled hover throttle at the test altitude.
    const float weight = (12.f + 2.f) * 9.80665f;
    const float rho = computeAtmosphere(500.f).density_kg_m3;
    const float hover = weight / (4.f * 60.f * (rho / 1.225f));
    ControlInput ctrl{};
    ctrl.throttle = hover;
    auto fi = makeQuadAt(data, hover);
    for (int i = 0; i < 60; ++i)
        fi.step(1.f / 60.f, ctrl, {});
    // One second at hover throttle: residual vertical velocity stays small (fuel burn + density
    // drift over a few metres are the only unbalanced terms).
    CHECK(std::abs(fi.state().vel_body[1]) < 0.5);
}

TEST_CASE("Multirotor: stick commands body rates with the right signs", "[multirotor]") {
    ControlInput ctrl{};
    ctrl.throttle = 0.6f;

    SECTION("aileron right rolls right (positive omega[0])") {
        auto fi = makeQuadAt(makeQuad(), 0.6f);
        ctrl.aileron = 1.f;
        for (int i = 0; i < 30; ++i)
            fi.step(1.f / 60.f, ctrl, {});
        CHECK(fi.state().omega[0] > 0.1f);
    }
    SECTION("elevator aft pitches up (positive omega[2])") {
        auto fi = makeQuadAt(makeQuad(), 0.6f);
        ctrl.elevator = 1.f;
        for (int i = 0; i < 30; ++i)
            fi.step(1.f / 60.f, ctrl, {});
        CHECK(fi.state().omega[2] > 0.1f);
    }
    SECTION("right pedal yaws nose right (negative omega[1])") {
        auto fi = makeQuadAt(makeQuad(), 0.6f);
        ctrl.rudder = 1.f;
        for (int i = 0; i < 30; ++i)
            fi.step(1.f / 60.f, ctrl, {});
        CHECK(fi.state().omega[1] < -0.1f);
    }
    SECTION("rate feedback damps: rates converge, not diverge") {
        auto fi = makeQuadAt(makeQuad(), 0.6f);
        ctrl.aileron = 1.f;
        for (int i = 0; i < 300; ++i)
            fi.step(1.f / 60.f, ctrl, {});
        // Full stick settles near 1/rate_damping_s = 1 rad/s, never runs away.
        CHECK(fi.state().omega[0] < 3.f);
    }
}

TEST_CASE("Multirotor: a dead battery is a flameout and the aircraft falls (#308)", "[multirotor]") {
    auto fi = makeQuadAt(makeQuad(), 1.f);
    FlightState s = fi.state();
    s.fuel_kg = 0.f;
    s.mass_kg = 12.f;
    fi.reset(s);
    ControlInput ctrl{};
    ctrl.throttle = 1.f;
    for (int i = 0; i < 60; ++i)
        fi.step(1.f / 60.f, ctrl, {});
    CHECK((fi.state().engineFailFlags & kEngineFlameout) != 0);
    CHECK(fi.state().vel_body[1] < -1.0);
}

TEST_CASE("Multirotor: losing a left rotor rolls left", "[multirotor]") {
    auto fi = makeQuadAt(makeQuad(), 0.6f);
    fi.setEngineFailFlags(kEngineFailLeft);
    ControlInput ctrl{};
    ctrl.throttle = 0.6f;
    for (int i = 0; i < 30; ++i)
        fi.step(1.f / 60.f, ctrl, {});
    CHECK(fi.state().omega[0] < 0.f); // left wing (rotor) down
}

TEST_CASE("Multirotor: trim() declines honestly (non-converged)", "[multirotor]") {
    auto d = makeQuad();
    TrimPoint pt;
    pt.altitude_m = 0.f;
    const TrimResult r = trim(*d, pt, {});
    CHECK_FALSE(r.converged);
}

// ── helicopter (#350) ────────────────────────────────────────────────────────

static const std::string kHeloToml = R"(
[aircraft]
name = "Test Helo"
type = "helicopter"

[flight_model]
mass_kg   = 5000.0
fuel_kg   = 1000.0
ixx_kg_m2 = 6000.0
iyy_kg_m2 = 40000.0
izz_kg_m2 = 40000.0

[helicopter]
main_rotor_radius_m     = 8.2
main_rotor_max_thrust_n = 90000.0
yaw_moment_max_nm       = 40000.0
cyclic_moment_nm        = 60000.0

[engine]
fuel_flow_idle_kg_s = 0.05
fuel_flow_mil_kg_s  = 0.30
)";

static std::shared_ptr<FlightModelData> makeHelo(const std::string& toml = kHeloToml) {
    return std::make_shared<FlightModelData>(parseFlightModel(toml));
}

static FlightIntegrator makeHeloAt(std::shared_ptr<FlightModelData> data, float collective, float altM) {
    FlightIntegrator fi(std::move(data));
    applyForceModelFor(fi, fi.flightModel());
    FlightState s{};
    s.pos_world[1] = altM;
    s.mass_kg = fi.flightModel().geometry.mass_kg + fi.flightModel().geometry.fuel_kg;
    s.fuel_kg = fi.flightModel().geometry.fuel_kg;
    s.throttle_actual = collective;
    fi.reset(s);
    return fi;
}

TEST_CASE("Helicopter: [helicopter] parses into the reduced schema", "[helicopter]") {
    auto d = makeHelo();
    REQUIRE(d->helicopter.has_value());
    CHECK(d->isHelicopter());
    CHECK(d->isRotorcraft());
    CHECK(d->helicopter->main_rotor_radius_m == Catch::Approx(8.2f));
    CHECK(d->engine.fuel_flow_mil_kg_s == Catch::Approx(0.30f));
    CHECK(d->engine.spool_time_s == Catch::Approx(1.0f)); // turboshaft default
}

TEST_CASE("Helicopter: missing [helicopter] table is rejected", "[helicopter]") {
    std::string s = kHeloToml;
    auto pos = s.find("[helicopter]");
    auto end = s.find("[engine]");
    REQUIRE(pos != std::string::npos);
    s.erase(pos, end - pos);
    CHECK_THROWS(parseFlightModel(s));
}

TEST_CASE("Helicopter: full collective climbs, low collective sinks", "[helicopter]") {
    ControlInput full{};
    full.throttle = 1.f;
    auto climb = makeHeloAt(makeHelo(), 1.f, 500.f);
    for (int i = 0; i < 60; ++i)
        climb.step(1.f / 60.f, full, {});
    CHECK(climb.state().vel_body[1] > 1.0);

    ControlInput low{};
    low.throttle = 0.2f;
    auto sink = makeHeloAt(makeHelo(), 0.2f, 500.f);
    for (int i = 0; i < 60; ++i)
        sink.step(1.f / 60.f, low, {});
    CHECK(sink.state().vel_body[1] < -1.0);
}

TEST_CASE("Helicopter: ground effect adds lift near the surface", "[helicopter]") {
    // Same collective, one disc height off the deck vs well clear of it: the low machine sees the
    // ground-effect thrust bonus and ends the second with more upward velocity.
    auto run = [](float altM) {
        ControlInput ctrl{};
        ctrl.throttle = 0.65f;
        auto fi = makeHeloAt(makeHelo(), 0.65f, altM);
        for (int i = 0; i < 30; ++i)
            fi.step(1.f / 60.f, ctrl, {});
        return fi.state().vel_body[1];
    };
    CHECK(run(5.f) > run(500.f) + 0.05);
}

TEST_CASE("Helicopter: an unpowered disc autorotates to a survivable sink rate", "[helicopter]") {
    // Engine out (fuel starvation flameout, #308): the machine descends, but the axial disc drag
    // caps the sink far below free fall — terminal ~ sqrt(2W / (rho * A * cd)) ~= 19 m/s here.
    auto fi = makeHeloAt(makeHelo(), 0.f, 2000.f);
    FlightState s = fi.state();
    s.fuel_kg = 0.f;
    s.mass_kg = 5000.f;
    fi.reset(s);
    ControlInput ctrl{};
    for (int i = 0; i < 600; ++i) // 10 s
        fi.step(1.f / 60.f, ctrl, {});
    CHECK((fi.state().engineFailFlags & kEngineFlameout) != 0);
    CHECK(fi.state().vel_body[1] < -8.0);  // it IS descending
    CHECK(fi.state().vel_body[1] > -30.0); // ...but nothing like free fall (~98 m/s by now)
}

TEST_CASE("Helicopter: cyclic and pedals command body rates with the right signs", "[helicopter]") {
    ControlInput ctrl{};
    ctrl.throttle = 0.6f;
    SECTION("aft cyclic pitches up") {
        auto fi = makeHeloAt(makeHelo(), 0.6f, 500.f);
        ctrl.elevator = 1.f;
        for (int i = 0; i < 30; ++i)
            fi.step(1.f / 60.f, ctrl, {});
        CHECK(fi.state().omega[2] > 0.05f);
    }
    SECTION("right cyclic rolls right") {
        auto fi = makeHeloAt(makeHelo(), 0.6f, 500.f);
        ctrl.aileron = 1.f;
        for (int i = 0; i < 30; ++i)
            fi.step(1.f / 60.f, ctrl, {});
        CHECK(fi.state().omega[0] > 0.05f);
    }
    SECTION("right pedal yaws nose right") {
        auto fi = makeHeloAt(makeHelo(), 0.6f, 500.f);
        ctrl.rudder = 1.f;
        for (int i = 0; i < 30; ++i)
            fi.step(1.f / 60.f, ctrl, {});
        CHECK(fi.state().omega[1] < -0.05f);
    }
}

TEST_CASE("Helicopter: main-rotor torque reaction yaws the nose without pedal input", "[helicopter]") {
    auto d = makeHelo();
    d->helicopter->torque_factor = 0.05f;
    auto fi = makeHeloAt(d, 0.7f, 500.f);
    ControlInput ctrl{};
    ctrl.throttle = 0.7f;
    for (int i = 0; i < 30; ++i)
        fi.step(1.f / 60.f, ctrl, {});
    CHECK(fi.state().omega[1] < -0.01f); // nose right against the CCW main rotor
}

TEST_CASE("Helicopter: flapback pitches the nose up with forward speed", "[helicopter]") {
    auto d = makeHelo();
    d->helicopter->flapback_nm_per_mps = 400.f;
    auto fi = makeHeloAt(d, 0.6f, 500.f);
    FlightState s = fi.state();
    s.vel_body[0] = 50.0; // fast forward flight
    fi.reset(s);
    ControlInput ctrl{};
    ctrl.throttle = 0.6f;
    for (int i = 0; i < 30; ++i)
        fi.step(1.f / 60.f, ctrl, {});
    CHECK(fi.state().omega[2] > 0.01f);
}
