// SPDX-License-Identifier: GPL-3.0-or-later
#include "flight_model_validator.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

// Generic fighter quick-start template from docs/modding/flight-model.md
static const char* kValidFighter = R"toml(
[aircraft]
name         = "Generic Fighter"
type         = "fighter"
engine_type  = "turbofan"
has_fbw      = false
cruise_alt_m = 10000
mesh         = "generic"
cockpit      = "generic_hud"

[flight_model]
mass_kg      = 12000.0
wing_area_m2 = 35.0
wingspan_m   = 10.0
mac_m        = 3.5
fuel_kg      = 4000.0
ixx_kg_m2    = 10000.0
iyy_kg_m2    = 70000.0
izz_kg_m2    = 78000.0

[aero.cl_table]
alpha  = [-5, 0, 5, 10, 15, 18, 20, 25]
mach   = [0.3, 0.6, 0.9, 1.2, 1.8]
values = [
    -0.20,-0.22,-0.24,-0.18,-0.12,
     0.05, 0.06, 0.07, 0.05, 0.03,
     0.40, 0.45, 0.52, 0.40, 0.28,
     0.75, 0.84, 0.97, 0.75, 0.52,
     1.05, 1.18, 1.36, 1.05, 0.73,
     1.18, 1.32, 1.52, 1.18, 0.82,
     1.10, 1.23, 1.42, 1.10, 0.76,
     0.85, 0.95, 1.10, 0.85, 0.59,
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
alpha_stall_deg  = 18.0
max_g_structural =  8.0
min_g_structural = -3.0
max_mach         =  1.6

[aero.controls]
max_elevator_deg = 25.0
max_aileron_deg  = 20.0
max_rudder_deg   = 30.0

[engine]
fuel_flow_idle_kg_s = 0.10
fuel_flow_mil_kg_s  = 0.90
fuel_flow_ab_kg_s   = 3.20
spool_time_s        = 5.0

[engine.mil_thrust]
mach   = [0.0, 0.3, 0.6, 0.9, 1.2, 1.5, 1.8]
alt_km = [0, 3, 6, 9, 12, 15]
values = [
    60.0, 51.0, 42.0, 33.0, 24.0, 15.0,
    63.0, 54.0, 44.0, 35.0, 25.0, 16.0,
    66.0, 56.0, 47.0, 37.0, 27.0, 17.0,
    68.0, 58.0, 48.0, 38.0, 28.0, 18.0,
    66.0, 56.0, 47.0, 37.0, 26.0, 17.0,
    62.0, 53.0, 44.0, 35.0, 25.0, 16.0,
    56.0, 48.0, 40.0, 32.0, 23.0, 15.0,
]
)toml";

// Replaces one key=value line in kValidFighter for mutation testing
static std::string patch(const char* base, const char* find, const char* replace) {
    std::string s(base);
    auto pos = s.find(find);
    if (pos != std::string::npos) {
        auto end = s.find('\n', pos);
        s.replace(pos, end - pos, replace);
    }
    return s;
}

TEST_CASE("valid generic fighter TOML passes", "[flight-model-validator]") {
    auto r = validateFlightModel(kValidFighter);
    CHECK(r.ok);
    CHECK(r.errors.empty());
}

TEST_CASE("malformed TOML fails with parse error", "[flight-model-validator]") {
    auto r = validateFlightModel("not valid toml {{{{");
    CHECK_FALSE(r.ok);
    REQUIRE(!r.errors.empty());
    CHECK(r.errors[0].find("parse error") != std::string::npos);
}

TEST_CASE("missing [aircraft] table fails", "[flight-model-validator]") {
    auto r = validateFlightModel("[flight_model]\nmass_kg = 10000.0\n");
    CHECK_FALSE(r.ok);
    REQUIRE(!r.errors.empty());
    CHECK(r.errors[0].find("aircraft") != std::string::npos);
}

TEST_CASE("invalid aircraft.type fails", "[flight-model-validator]") {
    auto r = validateFlightModel(patch(kValidFighter, "type         = \"fighter\"", "type         = \"helicopter\""));
    CHECK_FALSE(r.ok);
    bool found = false;
    for (const auto& e : r.errors)
        if (e.find("aircraft.type") != std::string::npos) {
            found = true;
            break;
        }
    CHECK(found);
}

TEST_CASE("invalid aircraft.engine_type fails", "[flight-model-validator]") {
    auto r = validateFlightModel(patch(kValidFighter, "engine_type  = \"turbofan\"", "engine_type  = \"rocket\""));
    CHECK_FALSE(r.ok);
}

TEST_CASE("mass_kg = 0 fails", "[flight-model-validator]") {
    auto r = validateFlightModel(patch(kValidFighter, "mass_kg      = 12000.0", "mass_kg      = 0.0"));
    CHECK_FALSE(r.ok);
    bool found = false;
    for (const auto& e : r.errors)
        if (e.find("mass_kg") != std::string::npos) {
            found = true;
            break;
        }
    CHECK(found);
}

TEST_CASE("mass_kg below fighter range produces warning, not error", "[flight-model-validator]") {
    auto r = validateFlightModel(patch(kValidFighter, "mass_kg      = 12000.0", "mass_kg      = 1000.0"));
    CHECK(r.ok);
    REQUIRE(!r.warnings.empty());
    CHECK(r.warnings[0].find("mass_kg") != std::string::npos);
}

TEST_CASE("cl_table with 3 alpha breakpoints fails", "[flight-model-validator]") {
    std::string s(kValidFighter);
    auto pos = s.find("alpha  = [-5, 0, 5, 10, 15, 18, 20, 25]");
    REQUIRE(pos != std::string::npos);
    s.replace(pos, std::string("alpha  = [-5, 0, 5, 10, 15, 18, 20, 25]").size(), "alpha  = [-5, 0, 5]");
    auto r = validateFlightModel(s);
    CHECK_FALSE(r.ok);
    bool found = false;
    for (const auto& e : r.errors)
        if (e.find("alpha") != std::string::npos) {
            found = true;
            break;
        }
    CHECK(found);
}

TEST_CASE("cl_table values size mismatch fails", "[flight-model-validator]") {
    // 8 alpha x 5 mach = 40 values needed; remove one to get 39
    std::string s(kValidFighter);
    auto pos = s.find("     0.85, 0.95, 1.10, 0.85, 0.59,\n]");
    REQUIRE(pos != std::string::npos);
    s.replace(pos, std::string("     0.85, 0.95, 1.10, 0.85, 0.59,\n]").size(), "     0.85, 0.95, 1.10, 0.85,\n]");
    auto r = validateFlightModel(s);
    CHECK_FALSE(r.ok);
}

TEST_CASE("positive cm_alpha fails sign check", "[flight-model-validator]") {
    auto r = validateFlightModel(patch(kValidFighter, "cm_alpha = -0.7", "cm_alpha =  0.7"));
    CHECK_FALSE(r.ok);
    bool found = false;
    for (const auto& e : r.errors)
        if (e.find("cm_alpha") != std::string::npos) {
            found = true;
            break;
        }
    CHECK(found);
}

TEST_CASE("positive cl_p fails sign check", "[flight-model-validator]") {
    auto r = validateFlightModel(patch(kValidFighter, "cl_p     = -0.40", "cl_p     =  0.40"));
    CHECK_FALSE(r.ok);
}

TEST_CASE("negative cl_da fails sign check", "[flight-model-validator]") {
    auto r = validateFlightModel(patch(kValidFighter, "cl_da    =  0.07", "cl_da    = -0.07"));
    CHECK_FALSE(r.ok);
}

TEST_CASE("negative cn_beta fails sign check", "[flight-model-validator]") {
    auto r = validateFlightModel(patch(kValidFighter, "cn_beta  =  0.10", "cn_beta  = -0.10"));
    CHECK_FALSE(r.ok);
}

TEST_CASE("min_g_structural >= 0 fails", "[flight-model-validator]") {
    auto r = validateFlightModel(patch(kValidFighter, "min_g_structural = -3.0", "min_g_structural =  1.0"));
    CHECK_FALSE(r.ok);
    bool found = false;
    for (const auto& e : r.errors)
        if (e.find("min_g_structural") != std::string::npos) {
            found = true;
            break;
        }
    CHECK(found);
}

TEST_CASE("missing [engine] table fails", "[flight-model-validator]") {
    // Remove the [engine] section
    std::string s(kValidFighter);
    auto pos = s.find("\n[engine]\n");
    REQUIRE(pos != std::string::npos);
    s = s.substr(0, pos);
    auto r = validateFlightModel(s);
    CHECK_FALSE(r.ok);
    bool found = false;
    for (const auto& e : r.errors)
        if (e.find("engine") != std::string::npos) {
            found = true;
            break;
        }
    CHECK(found);
}

TEST_CASE("engine.mil_thrust values dimension mismatch fails", "[flight-model-validator]") {
    // 7 mach x 6 alt = 42; remove last row to get 36
    std::string s(kValidFighter);
    auto pos = s.find("    56.0, 48.0, 40.0, 32.0, 23.0, 15.0,\n]");
    REQUIRE(pos != std::string::npos);
    s.replace(pos, std::string("    56.0, 48.0, 40.0, 32.0, 23.0, 15.0,\n]").size(), "]");
    auto r = validateFlightModel(s);
    CHECK_FALSE(r.ok);
}

TEST_CASE("optional [aero.tvc] with negative slew_rate fails", "[flight-model-validator]") {
    std::string s(kValidFighter);
    s += "\n[aero.tvc]\nmin_angle_deg   = -20\nmax_angle_deg   =  20\nslew_rate_deg_s = -5\n";
    auto r = validateFlightModel(s);
    CHECK_FALSE(r.ok);
    bool found = false;
    for (const auto& e : r.errors)
        if (e.find("slew_rate_deg_s") != std::string::npos) {
            found = true;
            break;
        }
    CHECK(found);
}

TEST_CASE("optional [wing_sweep] with ref outside range fails", "[flight-model-validator]") {
    std::string s(kValidFighter);
    s += "\n[wing_sweep]\nref_sweep_deg = 70.0\nmin_deg = 20.0\nmax_deg = 68.0\n"
         "slew_rate_deg_s = 7.5\n"
         "[wing_sweep.schedule]\nmach = [0.0, 0.9]\nsweep = [20, 68]\n"
         "[wing_sweep.spread]\ncl_scale = 1.2\nk_scale = 0.8\ncd0_delta = 0.004\n"
         "[wing_sweep.swept]\ncl_scale = 0.8\nk_scale = 1.3\ncd0_delta = -0.003\n";
    auto r = validateFlightModel(s);
    CHECK_FALSE(r.ok);
    bool found = false;
    for (const auto& e : r.errors)
        if (e.find("ref_sweep_deg") != std::string::npos) {
            found = true;
            break;
        }
    CHECK(found);
}

TEST_CASE("optional [carrier] with zero approach speed fails", "[flight-model-validator]") {
    std::string s(kValidFighter);
    s += "\n[carrier]\napproach_m_s = 0.0\napproach_aoa_deg = 8.0\n"
         "cat_min_m_s = 67.0\nhook_length_m = 5.0\n";
    auto r = validateFlightModel(s);
    CHECK_FALSE(r.ok);
}

TEST_CASE("[[hardpoints]] with default not in allowed fails", "[flight-model-validator]") {
    std::string s(kValidFighter);
    s += "\n[[hardpoints]]\nslot    = 0\ntype    = \"missile\"\n"
         "allowed = [\"aim120c\"]\ndefault = \"aim9x\"\n";
    auto r = validateFlightModel(s);
    CHECK_FALSE(r.ok);
    bool found = false;
    for (const auto& e : r.errors)
        if (e.find("default") != std::string::npos) {
            found = true;
            break;
        }
    CHECK(found);
}

TEST_CASE("[[hardpoints]] with duplicate slot fails", "[flight-model-validator]") {
    std::string s(kValidFighter);
    s += "\n[[hardpoints]]\nslot = 0\ntype = \"missile\"\n"
         "allowed = [\"aim120c\"]\ndefault = \"aim120c\"\n"
         "[[hardpoints]]\nslot = 0\ntype = \"bomb\"\n"
         "allowed = [\"gbu12\"]\ndefault = \"gbu12\"\n";
    auto r = validateFlightModel(s);
    CHECK_FALSE(r.ok);
    bool found = false;
    for (const auto& e : r.errors)
        if (e.find("duplicated") != std::string::npos) {
            found = true;
            break;
        }
    CHECK(found);
}

TEST_CASE("[[hardpoints]] with invalid type fails", "[flight-model-validator]") {
    std::string s(kValidFighter);
    s += "\n[[hardpoints]]\nslot = 0\ntype = \"torpedo\"\n"
         "allowed = [\"mk46\"]\ndefault = \"mk46\"\n";
    auto r = validateFlightModel(s);
    CHECK_FALSE(r.ok);
}

TEST_CASE("valid [[hardpoints]] passes", "[flight-model-validator]") {
    std::string s(kValidFighter);
    s += "\n[[hardpoints]]\nslot = 0\ntype = \"missile\"\n"
         "allowed = [\"aim120c\", \"aim9x\"]\ndefault = \"aim120c\"\n"
         "[[hardpoints]]\nslot = 1\ntype = \"bomb\"\n"
         "allowed = [\"gbu12\"]\ndefault = \"gbu12\"\n";
    auto r = validateFlightModel(s);
    CHECK(r.ok);
    CHECK(r.errors.empty());
}

TEST_CASE("all errors reported in single pass", "[flight-model-validator]") {
    // Completely empty doc — should produce multiple errors
    auto r = validateFlightModel("# empty\n");
    CHECK_FALSE(r.ok);
    CHECK(r.errors.size() >= 3);
}

TEST_CASE("bomber type does not trigger fighter mass warning", "[flight-model-validator]") {
    // Bomber with mass below fighter range should not warn
    std::string s(kValidFighter);
    // Replace type and mass
    s = patch(s.c_str(), "type         = \"fighter\"", "type         = \"bomber\"");
    s = patch(s.c_str(), "mass_kg      = 12000.0", "mass_kg      = 90000.0");
    s = patch(s.c_str(), "wing_area_m2 = 35.0", "wing_area_m2 = 311.0");
    s = patch(s.c_str(), "wingspan_m   = 10.0", "wingspan_m   = 50.0");
    auto r = validateFlightModel(s);
    CHECK(r.ok);
    CHECK(r.warnings.empty());
}
