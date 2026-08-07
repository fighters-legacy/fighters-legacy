// SPDX-License-Identifier: GPL-3.0-or-later
//
// FlightModelParser rejections (#1145). The parser throws on any validation failure, and it has
// five distinct schemas behind one entry point — fixed-wing, ballistic, multirotor, helicopter and
// vessel — each with its own required tables. test_flight_model_parser.cpp covers the fixed-wing
// happy path; this file is what happens when an author gets it wrong, and the four other schemas.
//
// A flight model that loads with a missing table is an aircraft that flies wrong rather than one
// that fails to load, which is far more expensive to notice.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "flight/FlightModelParser.h"

#include <stdexcept>
#include <string>

using Catch::Matchers::ContainsSubstring;
using namespace fl;

namespace {

// A complete, valid fixed-wing model. Every negative case removes or corrupts one piece of it.
constexpr const char* kFixedWing = R"(
[aircraft]
name         = "Test"
type         = "fighter"
engine_type  = "turbofan"
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
alpha  = [-5.0, 0.0, 5.0, 10.0]
mach   = [0.3, 0.9]
values = [-0.20,-0.24, 0.05, 0.07, 0.40, 0.52, 0.75, 0.97]

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
mach   = [0.0, 0.9]
alt_km = [0.0, 12.0]
values = [60.0, 30.0, 68.0, 34.0]
)";

// Drop a whole TOML table (from its header to the next header or end).
std::string withoutTable(std::string_view table) {
    std::string s(kFixedWing);
    const auto pos = s.find(table);
    REQUIRE(pos != std::string::npos);
    auto end = s.find("\n[", pos + 1);
    if (end == std::string::npos)
        end = s.size();
    s.erase(pos, end - pos);
    return s;
}

std::string replaced(std::string_view from, std::string_view to) {
    std::string s(kFixedWing);
    const auto pos = s.find(from);
    REQUIRE(pos != std::string::npos);
    s.replace(pos, from.size(), to);
    return s;
}

void rejects(const std::string& toml, std::string_view diagnosis) {
    INFO("expected diagnosis: " << diagnosis);
    REQUIRE_THROWS_WITH(parseFlightModel(toml), ContainsSubstring(std::string(diagnosis)));
}

} // namespace

TEST_CASE("parseFlightModel: the base fixed-wing model is valid (#1145)", "[flightmodel][parser]") {
    const auto d = parseFlightModel(kFixedWing);
    CHECK(d.geometry.mass_kg == 10000.f);
    CHECK(d.geometry.wing_area_m2 == 35.f);
}

// ---------------------------------------------------------------------------
// Document and required tables
// ---------------------------------------------------------------------------

TEST_CASE("parseFlightModel: malformed TOML throws with a parse diagnosis (#1145)", "[flightmodel][parser]") {
    rejects("[aircraft\nname = ", "TOML parse error");
}

TEST_CASE("parseFlightModel: the required tables are required (#1145)", "[flightmodel][parser]") {
    rejects(withoutTable("[aircraft]"), "missing [aircraft] table");
    rejects(withoutTable("[flight_model]"), "missing [flight_model] table");
    rejects(withoutTable("[aero.cl_table]"), "missing [aero.cl_table] table");
    rejects(withoutTable("[aero.drag_polar]"), "missing [aero.drag_polar] table");
    rejects(withoutTable("[aero.moments]"), "missing [aero.moments] table");
    rejects(withoutTable("[aero.limits]"), "missing [aero.limits] table");
    rejects(withoutTable("[aero.controls]"), "missing [aero.controls] table");
    rejects(withoutTable("[engine.mil_thrust]"), "missing [engine.mil_thrust] table");
    // [engine.mil_thrust] itself creates the `engine` table, so removing only [engine] leaves it
    // present-but-empty and the parser names the missing FIELD instead. Both have to go.
    {
        std::string s(kFixedWing);
        const auto pos = s.find("[engine]");
        REQUIRE(pos != std::string::npos);
        s.erase(pos);
        rejects(s, "missing [engine] table");
    }
}

TEST_CASE("parseFlightModel: aircraft identity fields are required (#1145)", "[flightmodel][parser]") {
    rejects(replaced("name         = \"Test\"", ""), "missing aircraft.name");
    rejects(replaced("type         = \"fighter\"", ""), "missing aircraft.type");
    rejects(replaced("engine_type  = \"turbofan\"", ""), "missing aircraft.engine_type");
}

TEST_CASE("parseFlightModel: the type and engine_type vocabularies are closed (#1145)", "[flightmodel][parser]") {
    rejects(replaced("\"fighter\"", "\"spaceship\""), "unknown aircraft type");
    rejects(replaced("\"turbofan\"", "\"warp core\""), "unknown engine_type");
}

TEST_CASE("parseFlightModel: a missing flight_model field names itself (#1145)", "[flightmodel][parser]") {
    rejects(replaced("mass_kg      = 10000.0", ""), "missing required field");
    rejects(replaced("wing_area_m2 = 35.0", ""), "missing required field");
    rejects(replaced("iyy_kg_m2    = 70000.0", ""), "missing required field");
}

// ---------------------------------------------------------------------------
// Table shape rules
// ---------------------------------------------------------------------------

TEST_CASE("parseFlightModel: aero tables need enough breakpoints (#1145)", "[flightmodel][parser]") {
    // values must shrink with alpha: the dimension check runs first, and would otherwise be the
    // error reported rather than the breakpoint-count rule under test.
    rejects(replaced("alpha  = [-5.0, 0.0, 5.0, 10.0]\nmach   = [0.3, 0.9]\n"
                     "values = [-0.20,-0.24, 0.05, 0.07, 0.40, 0.52, 0.75, 0.97]",
                     "alpha  = [-5.0, 0.0, 5.0]\nmach   = [0.3, 0.9]\n"
                     "values = [-0.20,-0.24, 0.05, 0.07, 0.40, 0.52]"),
            "alpha must have at least 4 breakpoints");
    rejects(replaced("alpha  = [-5.0, 0.0, 5.0, 10.0]\nmach   = [0.3, 0.9]\n"
                     "values = [-0.20,-0.24, 0.05, 0.07, 0.40, 0.52, 0.75, 0.97]",
                     "alpha  = [-5.0, 0.0, 5.0, 10.0]\nmach   = [0.3]\n"
                     "values = [-0.20, 0.05, 0.40, 0.75]"),
            "mach must have at least 2 breakpoints");
    // engine.mil_thrust: values must shrink with the axis under test, same dimension-first ordering.
    rejects(replaced("mach   = [0.0, 0.9]\nalt_km = [0.0, 12.0]\nvalues = [60.0, 30.0, 68.0, 34.0]",
                     "mach   = [0.0]\nalt_km = [0.0, 12.0]\nvalues = [60.0, 30.0]"),
            "mach must have at least 2 breakpoints");
    rejects(replaced("mach   = [0.0, 0.9]\nalt_km = [0.0, 12.0]\nvalues = [60.0, 30.0, 68.0, 34.0]",
                     "mach   = [0.0, 0.9]\nalt_km = [0.0]\nvalues = [60.0, 68.0]"),
            "alt_km must have at least 2 breakpoints");
}

TEST_CASE("parseFlightModel: an empty or non-numeric array is rejected (#1145)", "[flightmodel][parser]") {
    rejects(replaced("mach   = [0.3, 0.9]", "mach   = []"), "missing or empty required array");
    rejects(replaced("mach   = [0.3, 0.9]", "mach   = [\"fast\", \"slow\"]"), "non-numeric value in array");
}

TEST_CASE("parseFlightModel: a wing-sweep block validates its own consistency (#1145)", "[flightmodel][parser]") {
    const std::string base = std::string(kFixedWing) + R"(
[wing_sweep]
ref_sweep_deg   = 45.0
min_deg         = 20.0
max_deg         = 68.0
slew_rate_deg_s = 720.0

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
    CHECK_NOTHROW(parseFlightModel(base));

    auto perturb = [&base](std::string_view from, std::string_view to) {
        std::string s(base);
        const auto pos = s.find(from);
        REQUIRE(pos != std::string::npos);
        s.replace(pos, from.size(), to);
        return s;
    };
    REQUIRE_THROWS_WITH(parseFlightModel(perturb("ref_sweep_deg   = 45.0", "ref_sweep_deg   = 80.0")),
                        ContainsSubstring("must be within [min_deg, max_deg]"));
    REQUIRE_THROWS_WITH(parseFlightModel(perturb("sweep = [20.0, 45.0, 68.0]", "sweep = [20.0, 45.0]")),
                        ContainsSubstring("must have equal length"));
}

// ---------------------------------------------------------------------------
// The other four vehicle schemas
// ---------------------------------------------------------------------------

TEST_CASE("parseFlightModel: a ballistic model needs its boost table (#1145)", "[flightmodel][parser]") {
    constexpr const char* kBallisticHead = R"(
[aircraft]
name        = "AIM-120"
type        = "ballistic"
engine_type = "turbojet"

[flight_model]
mass_kg      = 150.0
wing_area_m2 = 0.05
wingspan_m   = 0.5
mac_m        = 0.2
fuel_kg      = 0.0
ixx_kg_m2    = 1.0
iyy_kg_m2    = 10.0
izz_kg_m2    = 10.0
)";
    rejects(kBallisticHead, "ballistic model: missing [engine.boost] table");
    rejects(std::string(kBallisticHead) + "\n[engine.boost]\nthrust_n = 0\nburn_time_s = 5\n", "thrust_n must be > 0");
    rejects(std::string(kBallisticHead) + "\n[engine.boost]\nthrust_n = 5000\nburn_time_s = 0\n",
            "burn_time_s must be > 0");

    CHECK_NOTHROW(
        parseFlightModel(std::string(kBallisticHead) + "\n[engine.boost]\nthrust_n = 5000\nburn_time_s = 5\n"));
}

TEST_CASE("parseFlightModel: a multirotor model validates its rotor geometry (#1145)", "[flightmodel][parser]") {
    constexpr const char* kHead = R"(
[aircraft]
name        = "Quad"
type        = "multirotor"
engine_type = "piston"

[flight_model]
mass_kg      = 2.0
wing_area_m2 = 0.1
wingspan_m   = 0.5
mac_m        = 0.1
fuel_kg      = 0.0
ixx_kg_m2    = 0.1
iyy_kg_m2    = 0.1
izz_kg_m2    = 0.2
)";
    rejects(kHead, "multirotor model: missing [multirotor] table");
    rejects(std::string(kHead) + "\n[multirotor]\nrotor_thrust_max_n = 10\n", "missing multirotor.rotor_count");
    rejects(std::string(kHead) + "\n[multirotor]\nrotor_count = 2\nrotor_thrust_max_n = 10\n",
            "rotor_count must be in [3, 16]");
    rejects(std::string(kHead) + "\n[multirotor]\nrotor_count = 20\nrotor_thrust_max_n = 10\n",
            "rotor_count must be in [3, 16]");
    rejects(std::string(kHead) + "\n[multirotor]\nrotor_count = 4\nrotor_thrust_max_n = 0\nrotor_arm_m = 0.2\n"
                                 "yaw_torque_nm = 0.5\n",
            "must be > 0");
    rejects(std::string(kHead) + "\n[multirotor]\nrotor_count = 4\nrotor_thrust_max_n = 10\nrotor_arm_m = 0.2\n"
                                 "yaw_torque_nm = 0.5\nflight_time_min = 0\n",
            "flight_time_min must be > 0");
}

TEST_CASE("parseFlightModel: a helicopter model needs rotor and engine tables (#1145)", "[flightmodel][parser]") {
    constexpr const char* kHead = R"(
[aircraft]
name        = "Huey"
type        = "helicopter"
engine_type = "turboprop"

[flight_model]
mass_kg      = 2500.0
wing_area_m2 = 1.0
wingspan_m   = 2.0
mac_m        = 0.5
fuel_kg      = 500.0
ixx_kg_m2    = 1000.0
iyy_kg_m2    = 4000.0
izz_kg_m2    = 3000.0
)";
    rejects(kHead, "helicopter model: missing [helicopter] table");
    rejects(std::string(kHead) + "\n[helicopter]\n", "missing required field: helicopter.main_rotor_radius_m");
    rejects(std::string(kHead) + "\n[helicopter]\nmain_rotor_radius_m = 0\nmain_rotor_max_thrust_n = 40000\n"
                                 "yaw_moment_max_nm = 8000\ncyclic_moment_nm = 20000\n"
                                 "collective_rate = 1.0\n",
            "must be > 0");
}

TEST_CASE("parseFlightModel: a vessel model needs its thrust and speed (#1145)", "[flightmodel][parser]") {
    constexpr const char* kHead = R"(
[aircraft]
name        = "Frigate"
type        = "vessel"
engine_type = "piston"

[flight_model]
mass_kg      = 4000000.0
wing_area_m2 = 100.0
wingspan_m   = 20.0
mac_m        = 5.0
fuel_kg      = 100000.0
ixx_kg_m2    = 1.0e8
iyy_kg_m2    = 1.0e9
izz_kg_m2    = 1.0e9
)";
    rejects(kHead, "vessel model: missing [vessel] table");
    rejects(std::string(kHead) + "\n[vessel]\nmax_thrust_n = 0\nmax_speed_mps = 15\n", "must be > 0");
    rejects(std::string(kHead) + "\n[vessel]\nmax_thrust_n = 1000000\nmax_speed_mps = 0\n", "must be > 0");
    CHECK_NOTHROW(parseFlightModel(std::string(kHead) + "\n[vessel]\nmax_thrust_n = 1000000\nmax_speed_mps = 15\n"));
}

// ---------------------------------------------------------------------------
// Optional blocks that must still be coherent when present
// ---------------------------------------------------------------------------

TEST_CASE("parseFlightModel: flaps cannot reduce parasite drag (#1145)", "[flightmodel][parser]") {
    rejects(std::string(kFixedWing) + "\n[aero.flaps]\ndcl = 0.4\ndcd = -0.01\n", "dcd must be >= 0");
    CHECK_NOTHROW(parseFlightModel(std::string(kFixedWing) + "\n[aero.flaps]\ndcl = 0.4\ndcd = 0.02\n"));
}

TEST_CASE("parseFlightModel: cd_wave arrays must pair up (#1145)", "[flightmodel][parser]") {
    rejects(std::string(kFixedWing) + "\n[aero.cd_wave]\nmach = [0.8, 1.0, 1.2]\nvalues = [0.01, 0.05]\n",
            "must have equal length");
}

TEST_CASE("parseFlightModel: drone limits must be coherent (#1145)", "[flightmodel][parser]") {
    rejects(std::string(kFixedWing) + "\n[drone_limits]\nmax_airspeed_mps = -1\n", "must be >= 0");
    rejects(std::string(kFixedWing) + "\n[drone_limits]\nmin_airspeed_mps = 90\nmax_airspeed_mps = 40\n",
            "must be below max_airspeed_mps");
}

TEST_CASE("parseFlightModel: refuelling and tanker types are closed vocabularies (#1145)", "[flightmodel][parser]") {
    rejects(std::string(kFixedWing) + "\n[refueling]\n", "missing refueling.type");
    rejects(std::string(kFixedWing) + "\n[refueling]\ntype = \"telepathy\"\n", "unknown refueling.type");
    rejects(std::string(kFixedWing) + "\n[tanker]\n", "missing tanker.type");
    rejects(std::string(kFixedWing) + "\n[tanker]\ntype = \"telepathy\"\n", "unknown tanker.type");
}

TEST_CASE("parseFlightModel: a propeller model needs a known rotation (#1145)", "[flightmodel][parser]") {
    const std::string prop = replaced("\"turbofan\"", "\"turboprop\"");
    rejects(prop + "\n[prop]\ndiameter_m = 3.0\n", "missing prop.rotation");
    rejects(prop + "\n[prop]\ndiameter_m = 3.0\nrotation = \"sideways\"\n", "unknown prop.rotation");
}
