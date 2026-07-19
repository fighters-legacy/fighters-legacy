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
