// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "flight/FlightModelParser.h"

#include <stdexcept>
#include <string>

using Catch::Matchers::WithinAbs;
using namespace fl;

// Minimal valid TOML that satisfies every required field.
//
// Note what is NOT here: `mesh` and `cockpit` (#813). A flight model is aerodynamics and does not
// know what it looks like -- asset wiring lives on the entity def. They used to be required by the
// parser and read by nothing.
static const std::string kMinimalToml = R"(
[aircraft]
name         = "Test Fighter"
type         = "fighter"
engine_type  = "turbofan"
has_fbw      = false
cruise_alt_m = 10000.0

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
alpha  = [-5.0, 0.0, 5.0, 10.0, 15.0]
mach   = [0.3, 0.9]
values = [
    -0.2, -0.2,
     0.05, 0.05,
     0.4,  0.4,
     0.75, 0.75,
     1.05, 1.05,
]

[aero.drag_polar]
cd0           = 0.018
k             = 0.14
speedbrake_cd = 0.07
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
alpha_stall_deg  = 15.0
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
mach   = [0.0, 0.9]
alt_km = [0.0, 12.0]
values = [60.0, 30.0,
          65.0, 33.0]
)";

TEST_CASE("Parser accepts minimal valid TOML", "[parser]") {
    REQUIRE_NOTHROW(parseFlightModel(kMinimalToml));
}

TEST_CASE("Parser optional blocks are absent by default", "[parser]") {
    auto d = parseFlightModel(kMinimalToml);
    CHECK_FALSE(d.tvc.has_value());
    CHECK_FALSE(d.wing_sweep.has_value());
    CHECK_FALSE(d.prop.has_value());
    CHECK_FALSE(d.carrier.has_value());
    CHECK_FALSE(d.refueling.has_value());
    CHECK_FALSE(d.tanker.has_value());
    CHECK_FALSE(d.cd_wave.has_value());
    CHECK_FALSE(d.engine.ab_thrust.has_value());
}

TEST_CASE("Parser reads aircraft metadata correctly", "[parser]") {
    auto d = parseFlightModel(kMinimalToml);
    CHECK(d.meta.name == "Test Fighter");
    CHECK(d.meta.role == AircraftRole::Fighter);
    CHECK(d.meta.engine_type == EngineType::Turbofan);
    CHECK_FALSE(d.meta.has_fbw);
    CHECK_THAT(d.geometry.mass_kg, WithinAbs(10000.f, 0.1f));
}

TEST_CASE("Parser accepts a flight model with no mesh and no cockpit", "[parser]") {
    // kMinimalToml declares neither -- the parse succeeding IS the assertion (#813). Before this,
    // both were required and the parser threw without them, while nothing read either one.
    CHECK_NOTHROW(parseFlightModel(kMinimalToml));
}

TEST_CASE("Parser ignores a legacy aircraft.mesh / aircraft.cockpit", "[parser]") {
    // Existing packs that still carry the dead keys keep parsing; the values just go nowhere.
    std::string toml = kMinimalToml;
    auto pos = toml.find("cruise_alt_m = 10000.0");
    toml.insert(pos, "mesh    = \"legacy_mesh\"\ncockpit = \"legacy_hud\"\n");
    CHECK_NOTHROW(parseFlightModel(toml));
}

TEST_CASE("Parser: cd_table is absent by default and parses when present", "[parser]") {
    // Optional by design (#820): the parabolic [aero.drag_polar] stays the simple path, which is what
    // most community content will use. A cd_table is for aircraft with real tabulated data.
    auto plain = parseFlightModel(kMinimalToml);
    CHECK_FALSE(plain.cd_table.has_value());

    std::string toml = kMinimalToml;
    toml += "\n[aero.cd_table]\n"
            "alpha  = [-10.0, 0.0, 10.0, 20.0]\n"
            "mach   = [0.3, 0.9]\n"
            "values = [0.10, 0.12,\n"
            "          0.02, 0.03,\n"
            "          0.08, 0.10,\n"
            "          0.30, 0.34]\n";
    auto d = parseFlightModel(toml);
    REQUIRE(d.cd_table.has_value());
    CHECK(d.cd_table->rows.size() == 4);
    CHECK(d.cd_table->cols.size() == 2);
    CHECK_THAT(d.cd_table->lookup(0.f, 0.3f), WithinAbs(0.02f, 1e-5f));
    CHECK_THAT(d.cd_table->lookup(20.f, 0.9f), WithinAbs(0.34f, 1e-5f));
}

TEST_CASE("Parser rejects a cd_table with too few breakpoints", "[parser]") {
    std::string toml = kMinimalToml;
    toml += "\n[aero.cd_table]\n"
            "alpha  = [-10.0, 0.0, 10.0]\n"
            "mach   = [0.3, 0.9]\n"
            "values = [0.10, 0.12, 0.02, 0.03, 0.08, 0.10]\n";
    CHECK_THROWS_AS(parseFlightModel(toml), std::runtime_error);
}

TEST_CASE("Parser reads drag polar fields", "[parser]") {
    auto d = parseFlightModel(kMinimalToml);
    CHECK_THAT(d.drag_polar.cd0, WithinAbs(0.018f, 1e-5f));
    CHECK_THAT(d.drag_polar.speedbrake_cd, WithinAbs(0.07f, 1e-5f));
    CHECK_THAT(d.drag_polar.gear_cd, WithinAbs(0.03f, 1e-5f));
}

TEST_CASE("Parser reads limits correctly", "[parser]") {
    auto d = parseFlightModel(kMinimalToml);
    CHECK_THAT(d.limits.min_g_structural, WithinAbs(-3.f, 1e-5f));
    CHECK_THAT(d.limits.max_mach, WithinAbs(1.6f, 1e-5f));
}

TEST_CASE("Parser rejects missing required field", "[parser]") {
    std::string toml = kMinimalToml;
    // Remove mass_kg line
    auto pos = toml.find("mass_kg");
    toml.erase(pos, toml.find('\n', pos) - pos + 1);
    CHECK_THROWS_AS(parseFlightModel(toml), std::runtime_error);
}

TEST_CASE("Parser rejects CL table dimension mismatch", "[parser]") {
    std::string toml = kMinimalToml;
    // Replace values with wrong size (5 instead of 10)
    auto vpos = toml.rfind("values = [");
    auto vend = toml.find(']', vpos);
    toml.replace(vpos, vend - vpos + 1, "values = [-0.2, 0.05, 0.4, 0.75, 1.05]");
    CHECK_THROWS_AS(parseFlightModel(toml), std::runtime_error);
}

TEST_CASE("Parser rejects too few CL table alpha breakpoints", "[parser]") {
    // Only 3 alpha breakpoints (minimum is 4)
    const std::string toml = R"(
[aircraft]
name         = "T"
type         = "fighter"
engine_type  = "turbofan"
has_fbw      = false
cruise_alt_m = 0.0
mesh         = "m"
cockpit      = "c"

[flight_model]
mass_kg      = 1.0
wing_area_m2 = 1.0
wingspan_m   = 1.0
mac_m        = 1.0
fuel_kg      = 1.0
ixx_kg_m2    = 1.0
iyy_kg_m2    = 1.0
izz_kg_m2    = 1.0

[aero.cl_table]
alpha  = [-5.0, 0.0, 5.0]
mach   = [0.3, 0.9]
values = [-0.2, -0.2, 0.05, 0.05, 0.4, 0.4]

[aero.drag_polar]
cd0           = 0.018
k             = 0.14
speedbrake_cd = 0.07
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
alpha_stall_deg  = 15.0
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
mach   = [0.0, 0.9]
alt_km = [0.0, 12.0]
values = [60.0, 30.0, 65.0, 33.0]
)";
    CHECK_THROWS_AS(parseFlightModel(toml), std::runtime_error);
}

TEST_CASE("Parser rejects wing_sweep ref outside min/max range", "[parser]") {
    std::string toml = kMinimalToml + R"(
[wing_sweep]
ref_sweep_deg   = 10.0
min_deg         = 20.0
max_deg         = 68.0
slew_rate_deg_s = 7.5

[wing_sweep.schedule]
mach  = [0.0, 1.0]
sweep = [20.0, 68.0]

[wing_sweep.spread]
cl_scale=1.2 k_scale=0.8 cd0_delta=0.004

[wing_sweep.swept]
cl_scale=0.82 k_scale=1.3 cd0_delta=-0.003
)";
    CHECK_THROWS_AS(parseFlightModel(toml), std::runtime_error);
}

TEST_CASE("Parser accepts optional [aero.tvc] block", "[parser]") {
    std::string toml = kMinimalToml + R"(
[aero.tvc]
min_angle_deg   = -20.0
max_angle_deg   =  20.0
slew_rate_deg_s =   5.0
)";
    auto d = parseFlightModel(toml);
    REQUIRE(d.tvc.has_value());
    CHECK_THAT(d.tvc->max_angle_deg, WithinAbs(20.f, 1e-5f));
}

TEST_CASE("Parser accepts optional [carrier] block", "[parser]") {
    std::string toml = kMinimalToml + R"(
[carrier]
approach_m_s     = 69.4
approach_aoa_deg =  8.1
cat_min_m_s      = 66.9
hook_length_m    =  5.33
)";
    auto d = parseFlightModel(toml);
    REQUIRE(d.carrier.has_value());
    CHECK_THAT(d.carrier->approach_aoa_deg, WithinAbs(8.1f, 1e-4f));
}

TEST_CASE("Parser accepts optional [tanker] block with type both", "[parser]") {
    std::string toml = kMinimalToml + R"(
[tanker]
type            = "both"
stations        = 3
max_rate_kg_s   = 4.5
offload_reserve = 0.20
)";
    auto d = parseFlightModel(toml);
    REQUIRE(d.tanker.has_value());
    CHECK(d.tanker->boom);
    CHECK(d.tanker->drogue);
    CHECK(d.tanker->stations == 3);
}

TEST_CASE("Parser accepts [engine.ab_thrust] optional block", "[parser]") {
    std::string toml = kMinimalToml + R"(
[engine.ab_thrust]
mach   = [0.0, 0.9]
alt_km = [0.0, 12.0]
values = [100.0, 50.0,
          105.0, 53.0]
)";
    auto d = parseFlightModel(toml);
    REQUIRE(d.engine.ab_thrust.has_value());
}

TEST_CASE("Parser accepts [engine.idle_thrust] optional block", "[parser]") {
    // #898: absent by default; present it parses on the same (mach, alt_km) grid as mil_thrust,
    // idle values may be negative (ram drag exceeds idle gross thrust at speed/altitude).
    auto plain = parseFlightModel(kMinimalToml);
    CHECK_FALSE(plain.engine.idle_thrust.has_value());

    std::string toml = kMinimalToml + R"(
[engine.idle_thrust]
mach   = [0.0, 0.9]
alt_km = [0.0, 12.0]
values = [ 2.8,  1.0,
         -16.0, -10.0]
)";
    auto d = parseFlightModel(toml);
    REQUIRE(d.engine.idle_thrust.has_value());
    CHECK_THAT(d.engine.idle_thrust->lookup(0.0f, 0.0f), WithinAbs(2.8f, 1e-5f));
    CHECK(d.engine.idle_thrust->lookup(0.9f, 0.0f) < 0.f);
}

TEST_CASE("Parser rejects [engine.idle_thrust] with too few breakpoints", "[parser]") {
    std::string toml = kMinimalToml + R"(
[engine.idle_thrust]
mach   = [0.0]
alt_km = [0.0, 12.0]
values = [2.8, 1.0]
)";
    CHECK_THROWS_AS(parseFlightModel(toml), std::runtime_error);
}

TEST_CASE("Parser rejects unknown engine_type", "[parser]") {
    std::string toml = kMinimalToml;
    auto pos = toml.find("\"turbofan\"");
    toml.replace(pos, 10, "\"warpdriv\"");
    CHECK_THROWS_AS(parseFlightModel(toml), std::runtime_error);
}
