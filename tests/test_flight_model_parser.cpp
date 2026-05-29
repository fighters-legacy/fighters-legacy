// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "flight/FlightModelParser.h"

#include <stdexcept>
#include <string>

using Catch::Matchers::WithinAbs;
using namespace fl;

// Minimal valid TOML that satisfies every required field.
static const std::string kMinimalToml = R"(
[aircraft]
name         = "Test Fighter"
type         = "fighter"
engine_type  = "turbofan"
has_fbw      = false
cruise_alt_m = 10000.0
mesh         = "test_mesh"
cockpit      = "test_hud"

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

TEST_CASE("Parser rejects unknown engine_type", "[parser]") {
    std::string toml = kMinimalToml;
    auto pos = toml.find("\"turbofan\"");
    toml.replace(pos, 10, "\"warpdriv\"");
    CHECK_THROWS_AS(parseFlightModel(toml), std::runtime_error);
}
