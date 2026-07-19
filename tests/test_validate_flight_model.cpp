// SPDX-License-Identifier: GPL-3.0-or-later
#include "flight_model_validator.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace fl;

// Generic fighter quick-start template from docs/modding/flight-model.md
static const char* kValidFighter = R"toml(
[aircraft]
name         = "Generic Fighter"
type         = "fighter"
engine_type  = "turbofan"
has_fbw      = false
cruise_alt_m = 10000

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

TEST_CASE("#899: a valid Ixz passes", "[flight-model-validator]") {
    // ixx=10000, izz=78000 -> Ixz^2 < 7.8e8, so 5000 is comfortably valid.
    auto r = validateFlightModel(
        patch(kValidFighter, "izz_kg_m2    = 78000.0", "izz_kg_m2    = 78000.0\nixz_kg_m2    = 5000.0"));
    CHECK(r.ok);
    CHECK(r.errors.empty());
}

TEST_CASE("#899: an Ixz too large for the inertia tensor fails", "[flight-model-validator]") {
    // 30000^2 = 9e8 > Ixx*Izz = 7.8e8 -> non-positive coupled-solve determinant.
    auto r = validateFlightModel(
        patch(kValidFighter, "izz_kg_m2    = 78000.0", "izz_kg_m2    = 78000.0\nixz_kg_m2    = 30000.0"));
    CHECK_FALSE(r.ok);
    bool found = false;
    for (const auto& e : r.errors)
        if (e.find("ixz_kg_m2") != std::string::npos)
            found = true;
    CHECK(found);
}

TEST_CASE("#899: a malformed alpha-damper table fails", "[flight-model-validator]") {
    std::string toml =
        std::string(kValidFighter) + "\n[aero.moments.cm_q_table]\nalpha = [0.0, 15.0]\nvalues = [-3.4]\n";
    auto r = validateFlightModel(toml);
    CHECK_FALSE(r.ok);
    bool found = false;
    for (const auto& e : r.errors)
        if (e.find("cm_q_table") != std::string::npos)
            found = true;
    CHECK(found);
}

TEST_CASE("#900: a plausible alpha_limit_deg below the stall passes", "[flight-model-validator]") {
    auto r = validateFlightModel(
        patch(kValidFighter, "max_mach         =  1.6", "max_mach         =  1.6\nalpha_limit_deg  = 15.0"));
    CHECK(r.ok);
}

TEST_CASE("#900: alpha_limit_deg >= alpha_stall_deg warns", "[flight-model-validator]") {
    // Cap at/above the aero stall never binds — almost certainly a mix-up of the two.
    auto r = validateFlightModel(patch(kValidFighter, "max_mach         =  1.6",
                                       "max_mach         =  1.6\nalpha_limit_deg  = 25.0")); // stall is 18
    CHECK(r.ok);
    bool warned = false;
    for (const auto& w : r.warnings)
        if (w.find("alpha_limit_deg") != std::string::npos)
            warned = true;
    CHECK(warned);
}

TEST_CASE("a flight model with no mesh and no cockpit validates clean", "[flight-model-validator]") {
    // kValidFighter declares neither (#813): asset wiring belongs to the entity def, so the
    // validator has no business demanding it of an aerodynamic model.
    auto r = validateFlightModel(kValidFighter);
    REQUIRE(r.ok);
    for (const auto& e : r.errors)
        CHECK(e.find("mesh") == std::string::npos);
    for (const auto& e : r.errors)
        CHECK(e.find("cockpit") == std::string::npos);
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
    // "helicopter" was the invalid-type example until #350 made it real; a zeppelin still is not.
    auto r = validateFlightModel(patch(kValidFighter, "type         = \"fighter\"", "type         = \"zeppelin\""));
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

// ---------------------------------------------------------------------------
// Plausibility bands (#815) — the light-fighter class must not be excluded.
//
// The old bands (mass [8000, 25000], area [25, 75], span [8, 20]) were calibrated on the
// F-15/F-16/F-18 class. An honest F-5E Tiger II trips two of the three, and so would a MiG-21, a
// Gnat, or a Tejas. The F-5E IS a fighter and must be declared `type = "fighter"` — calling it a
// trainer to dodge a lint would be a lie in the content to work around a bug in the tool.
// ---------------------------------------------------------------------------

// Applies a real aircraft's geometry to the generic-fighter fixture.
static std::string withGeometry(const char* type, double mass, double area, double span) {
    std::string s = patch(kValidFighter, "mass_kg      = 12000.0", ("mass_kg      = " + std::to_string(mass)).c_str());
    s = patch(s.c_str(), "wing_area_m2 = 35.0", ("wing_area_m2 = " + std::to_string(area)).c_str());
    s = patch(s.c_str(), "wingspan_m   = 10.0", ("wingspan_m   = " + std::to_string(span)).c_str());
    s = patch(s.c_str(), "type         = \"fighter\"", (std::string("type         = \"") + type + "\"").c_str());
    return s;
}

TEST_CASE("F-5E Tiger II geometry produces ZERO warnings", "[flight-model-validator]") {
    // 4349 kg, 17.28 m^2, 8.13 m => wing loading 252 kg/m^2, aspect ratio 3.83.
    // This is the whole point of #815: fl-base-pack's first aircraft must validate clean.
    auto r = withGeometry("fighter", 4349.0, 17.28, 8.13);
    auto res = validateFlightModel(r);
    CHECK(res.ok);
    INFO("warnings: " << (res.warnings.empty() ? std::string("none") : res.warnings[0]));
    CHECK(res.warnings.empty());
}

TEST_CASE("F-15C geometry still produces zero warnings", "[flight-model-validator]") {
    // No regression at the other end of the class: 20 200 kg, 56.5 m^2, 13.05 m
    // => wing loading 358, aspect ratio 3.01.
    auto res = validateFlightModel(withGeometry("fighter", 20200.0, 56.5, 13.05));
    CHECK(res.ok);
    INFO("warnings: " << (res.warnings.empty() ? std::string("none") : res.warnings[0]));
    CHECK(res.warnings.empty());
}

TEST_CASE("T-38A Talon geometry produces zero warnings as a trainer", "[flight-model-validator]") {
    // 3270 kg, 15.79 m^2, 7.7 m => wing loading 207, aspect ratio 3.76.
    auto res = validateFlightModel(withGeometry("trainer", 3270.0, 15.79, 7.7));
    CHECK(res.ok);
    INFO("warnings: " << (res.warnings.empty() ? std::string("none") : res.warnings[0]));
    CHECK(res.warnings.empty());
}

TEST_CASE("a mass entered in pounds is still caught", "[flight-model-validator]") {
    // The F-5E's 4349 kg written as 9588 lb. The absolute band cannot see this (9588 is a
    // perfectly plausible fighter mass in kg) -- but the WING LOADING can: 9588 / 17.28 = 555,
    // which is at the very top of the fighter band, and the ratio check is what catches the
    // egregious version below.
    auto nearMiss = validateFlightModel(withGeometry("fighter", 9588.0, 17.28, 8.13));
    CHECK(nearMiss.ok); // still only warnings, never a hard failure

    // A gross unit error -- an F-15C's 44 500 lb entered as kg -- IS caught by the absolute band.
    auto gross = validateFlightModel(withGeometry("fighter", 44500.0, 56.5, 13.05));
    REQUIRE(!gross.warnings.empty());
    bool named = false;
    for (const auto& w : gross.warnings)
        if (w.find("mass_kg") != std::string::npos)
            named = true;
    CHECK(named);
}

TEST_CASE("an implausible wing loading is warned even when the absolutes pass", "[flight-model-validator]") {
    // 20 000 kg on an F-5E's wing: every absolute is in range, but 1157 kg/m^2 is not an aeroplane.
    // This is the class of error the old absolute-only bands could not see at all.
    auto res = validateFlightModel(withGeometry("fighter", 20000.0, 17.28, 8.13));
    CHECK(res.ok);
    bool named = false;
    for (const auto& w : res.warnings)
        if (w.find("wing loading") != std::string::npos)
            named = true;
    CHECK(named);
}

TEST_CASE("an implausible aspect ratio is warned", "[flight-model-validator]") {
    // A 25 m span on a 17.28 m^2 wing: AR 36, a sailplane, not a fighter.
    auto res = validateFlightModel(withGeometry("fighter", 4349.0, 17.28, 25.0));
    CHECK(res.ok);
    bool named = false;
    for (const auto& w : res.warnings)
        if (w.find("aspect ratio") != std::string::npos)
            named = true;
    CHECK(named);
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

// Hardpoints moved to the entity definition TOML in #623. The flight model must now REJECT them,
// not validate them: a pack that leaves them here would otherwise fly with no stations and no
// explanation. These cases replace the old slot/type/allowed/default checks, which now live in
// parseEntityDef (tests/test_entity.cpp).

TEST_CASE("[[hardpoints]] in a flight model fails with a migration message", "[flight-model-validator]") {
    std::string s(kValidFighter);
    s += "\n[[hardpoints]]\nslot = 0\ntype = \"missile\"\n"
         "allowed = [\"aim120c\", \"aim9x\"]\ndefault = \"aim120c\"\n";
    auto r = validateFlightModel(s);
    CHECK_FALSE(r.ok);

    bool explained = false;
    for (const auto& e : r.errors)
        if (e.find("entity definition TOML") != std::string::npos) {
            explained = true;
            break;
        }
    CHECK(explained); // the error has to say WHERE they went, not just that they are wrong
}

TEST_CASE("a flight model without hardpoints still passes", "[flight-model-validator]") {
    auto r = validateFlightModel(kValidFighter);
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

// ---------------------------------------------------------------------------
// [aero.cd_table] (#820)
// ---------------------------------------------------------------------------

static std::string withCdTable(const char* extra, const char* kValue = "k             = 0.0") {
    std::string s = patch(kValidFighter, "k             = 0.14", kValue);
    s += std::string("\n[aero.cd_table]\n") + extra;
    return s;
}

static constexpr const char* kGoodCdTable = "alpha  = [-10, 0, 10, 20]\n"
                                            "mach   = [0.3, 0.9]\n"
                                            "values = [0.10, 0.12, 0.02, 0.03, 0.08, 0.10, 0.30, 0.34]\n";

TEST_CASE("a valid cd_table passes", "[flight-model-validator]") {
    auto r = validateFlightModel(withCdTable(kGoodCdTable));
    INFO("errors: " << (r.errors.empty() ? std::string("none") : r.errors[0]));
    CHECK(r.ok);
    CHECK(r.errors.empty());
}

TEST_CASE("cd_table with a non-zero drag_polar.k is an ERROR", "[flight-model-validator]") {
    // The double-count guard: a cd_table is TOTAL clean drag and already includes induced drag, so
    // a non-zero k would silently apply roughly twice the intended drag.
    auto r = validateFlightModel(withCdTable(kGoodCdTable, "k             = 0.14"));
    CHECK_FALSE(r.ok);
    bool found = false;
    for (const auto& e : r.errors)
        if (e.find("double-count") != std::string::npos)
            found = true;
    CHECK(found);
}

TEST_CASE("cd_table with 3 alpha breakpoints fails", "[flight-model-validator]") {
    auto r = validateFlightModel(withCdTable("alpha  = [-10, 0, 10]\n"
                                             "mach   = [0.3, 0.9]\n"
                                             "values = [0.1, 0.12, 0.02, 0.03, 0.08, 0.10]\n"));
    CHECK_FALSE(r.ok);
}

TEST_CASE("cd_table values size mismatch fails", "[flight-model-validator]") {
    auto r = validateFlightModel(withCdTable("alpha  = [-10, 0, 10, 20]\n"
                                             "mach   = [0.3, 0.9]\n"
                                             "values = [0.10, 0.12, 0.02, 0.03, 0.08]\n"));
    CHECK_FALSE(r.ok);
}

TEST_CASE("cd_table with a non-positive CD fails", "[flight-model-validator]") {
    // A zero or negative CD is a transcription error; the aircraft would accelerate under its drag.
    auto r = validateFlightModel(withCdTable("alpha  = [-10, 0, 10, 20]\n"
                                             "mach   = [0.3, 0.9]\n"
                                             "values = [0.10, 0.12, 0.0, 0.03, 0.08, 0.10, 0.30, 0.34]\n"));
    CHECK_FALSE(r.ok);
}

TEST_CASE("a model with no cd_table still validates (the parabolic path is untouched)", "[flight-model-validator]") {
    auto r = validateFlightModel(kValidFighter);
    CHECK(r.ok);
}

// ---------------------------------------------------------------------------
// Stall consistency (#816) — the check that makes alpha_stall_deg load-bearing.
// ---------------------------------------------------------------------------

TEST_CASE("a cl_table peaking away from alpha_stall_deg is an ERROR", "[flight-model-validator]") {
    // The engine does not clamp CL at the stall -- the table IS the stall. A model whose lift peaks
    // at 25 deg while declaring it departs at 18 is lying about itself, and the flag, the buffet, the
    // HUD and #54's stall-speed gate all inherit the lie.
    auto r = validateFlightModel(patch(kValidFighter, "alpha_stall_deg  = 18.0", "alpha_stall_deg  = 25.0"));
    CHECK_FALSE(r.ok);
    bool found = false;
    for (const auto& e : r.errors)
        if (e.find("alpha_stall_deg") != std::string::npos && e.find("peaks") != std::string::npos)
            found = true;
    CHECK(found);
}

TEST_CASE("a cl_table peaking within tolerance of alpha_stall_deg passes", "[flight-model-validator]") {
    // The fixture's table peaks at 18 deg; 19 is inside the 2-degree tolerance (breakpoints are coarse).
    auto r = validateFlightModel(patch(kValidFighter, "alpha_stall_deg  = 18.0", "alpha_stall_deg  = 19.0"));
    INFO("errors: " << (r.errors.empty() ? std::string("none") : r.errors[0]));
    CHECK(r.ok);
}

TEST_CASE("Ballistic models validate against the reduced schema (#354)", "[validator][ballistic]") {
    const char* ok = R"(
[aircraft]
name = "Test MRBM"
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
[engine.boost]
thrust_n = 300000.0
burn_time_s = 60.0
)";
    auto r = fl::validateFlightModel(ok);
    CHECK(r.ok);
    CHECK(r.errors.empty()); // no CL-table/turbine demands of an unwinged booster

    // The one thing a booster cannot omit is its boost.
    const char* noBoost = R"(
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
)";
    auto r2 = fl::validateFlightModel(noBoost);
    CHECK_FALSE(r2.ok);

    const char* badBurn = R"(
[aircraft]
name = "Bad Burn"
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
[engine.boost]
thrust_n = 300000.0
burn_time_s = 0.0
)";
    CHECK_FALSE(fl::validateFlightModel(badBurn).ok);
}

TEST_CASE("#308: valid engine failure dynamics fields pass", "[flight-model-validator]") {
    auto r = validateFlightModel(patch(kValidFighter, "spool_time_s        = 5.0",
                                       "spool_time_s        = 5.0\nflameout_alt_km = 16.0\n"
                                       "relight_min_mps = 80.0\ncompressor_stall = true\n"
                                       "surge_alpha_margin_deg = 6.0"));
    CHECK(r.ok);
    CHECK(r.errors.empty());
}

TEST_CASE("#308: flameout_alt_km in metres is caught by the plausibility band", "[flight-model-validator]") {
    auto r = validateFlightModel(
        patch(kValidFighter, "spool_time_s        = 5.0", "spool_time_s        = 5.0\nflameout_alt_km = 15000.0"));
    bool found = false;
    for (const auto& w : r.warnings)
        found = found || w.find("flameout_alt_km") != std::string::npos;
    CHECK(found);
}

TEST_CASE("#308: surge margin without compressor_stall warns", "[flight-model-validator]") {
    auto r = validateFlightModel(
        patch(kValidFighter, "spool_time_s        = 5.0", "spool_time_s        = 5.0\nsurge_alpha_margin_deg = 6.0"));
    bool found = false;
    for (const auto& w : r.warnings)
        found = found || w.find("surge_alpha_margin_deg") != std::string::npos;
    CHECK(found);
}

TEST_CASE("#308: out-of-range relight_min_mps fails", "[flight-model-validator]") {
    auto r = validateFlightModel(
        patch(kValidFighter, "spool_time_s        = 5.0", "spool_time_s        = 5.0\nrelight_min_mps = 900.0"));
    CHECK_FALSE(r.ok);
}

// ── multirotor (#349) ────────────────────────────────────────────────────────

static constexpr const char* kValidQuad = R"toml(
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
)toml";

TEST_CASE("#349: a valid multirotor validates against the reduced schema", "[flight-model-validator]") {
    auto r = validateFlightModel(kValidQuad);
    CHECK(r.ok);
    CHECK(r.errors.empty());
}

TEST_CASE("#349: a multirotor that cannot hover fails", "[flight-model-validator]") {
    auto r = validateFlightModel(patch(kValidQuad, "rotor_thrust_max_n = 60.0", "rotor_thrust_max_n = 20.0"));
    CHECK_FALSE(r.ok); // 4 x 20 N = 80 N < 137 N all-up weight
}

TEST_CASE("#349: a multirotor without [multirotor] fails", "[flight-model-validator]") {
    std::string s(kValidQuad);
    auto pos = s.find("[multirotor]");
    REQUIRE(pos != std::string::npos);
    auto r = validateFlightModel(s.substr(0, pos));
    CHECK_FALSE(r.ok);
}

TEST_CASE("#349: an implausible rotor_count fails", "[flight-model-validator]") {
    auto r = validateFlightModel(patch(kValidQuad, "rotor_count        = 4", "rotor_count        = 2"));
    CHECK_FALSE(r.ok);
}

// ── helicopter (#350) ────────────────────────────────────────────────────────

static constexpr const char* kValidHelo = R"toml(
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
)toml";

TEST_CASE("#350: a valid helicopter validates against the reduced schema", "[flight-model-validator]") {
    auto r = validateFlightModel(kValidHelo);
    CHECK(r.ok);
    CHECK(r.errors.empty());
}

TEST_CASE("#350: a helicopter that cannot hover fails", "[flight-model-validator]") {
    auto r = validateFlightModel(
        patch(kValidHelo, "main_rotor_max_thrust_n = 90000.0", "main_rotor_max_thrust_n = 40000.0"));
    CHECK_FALSE(r.ok); // 40 kN < 58.8 kN all-up weight
}

TEST_CASE("#350: implausible disc loading warns", "[flight-model-validator]") {
    // Radius entered in feet (26.9) triples the disc loading past the band.
    auto r = validateFlightModel(patch(kValidHelo, "main_rotor_radius_m     = 8.2", "main_rotor_radius_m     = 3.0"));
    bool found = false;
    for (const auto& w : r.warnings)
        found = found || w.find("disc loading") != std::string::npos;
    CHECK(found);
}

TEST_CASE("#350: a helicopter without [engine] fuel flows fails", "[flight-model-validator]") {
    std::string s(kValidHelo);
    auto pos = s.find("[engine]");
    REQUIRE(pos != std::string::npos);
    auto r = validateFlightModel(s.substr(0, pos));
    CHECK_FALSE(r.ok);
}
