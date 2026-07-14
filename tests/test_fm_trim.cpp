// SPDX-License-Identifier: GPL-3.0-or-later
//
// fm-trim (#817).
//
// These tests LOCK THE NUMBERS, and that is arguably their more valuable job: they are the engine's
// guard against someone "improving" AeroForces.cpp and silently changing the performance of every
// aircraft ever authored. If the drag model moves, an aircraft's sustained turn moves, and this
// notices.

#include "expect.h"

#include "flight/Trim.h"

#include "flight/BuiltinFlightModel.h"
#include "flight/FlightModelParser.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>

using Catch::Approx;
using namespace fl;

namespace {

// A light fighter in the F-5E class: the aircraft this whole epic exists to fly.
const char* kLightFighter = R"(
[aircraft]
name         = "Test Light Fighter"
type         = "fighter"
engine_type  = "turbojet"
has_fbw      = false
cruise_alt_m = 11000.0

[flight_model]
mass_kg      = 4349.0
wing_area_m2 = 17.28
wingspan_m   = 8.13
mac_m        = 2.44
fuel_kg      = 2000.0
ixx_kg_m2    = 3800.0
iyy_kg_m2    = 25000.0
izz_kg_m2    = 27000.0

[aero.cl_table]
alpha  = [-5.0, 0.0, 5.0, 10.0, 15.0, 18.0, 22.0, 26.0]
mach   = [0.3, 0.6, 0.9, 1.2]
values = [
    -0.25,-0.26,-0.28,-0.20,
     0.05, 0.05, 0.06, 0.04,
     0.40, 0.43, 0.48, 0.35,
     0.78, 0.84, 0.92, 0.66,
     1.10, 1.18, 1.28, 0.92,
     1.24, 1.32, 1.42, 1.02,
     1.15, 1.22, 1.30, 0.94,
     0.95, 1.00, 1.08, 0.78,
]

[aero.drag_polar]
cd0           = 0.0200
k             = 0.115
speedbrake_cd = 0.06
gear_cd       = 0.025

[aero.moments]
cm_alpha = -0.6
cm_q     = -9.0
cm_de    = -0.9
cl_beta  = -0.09
cl_p     = -0.38
cl_da    =  0.08
cn_beta  =  0.12
cn_r     = -0.14
cn_dr    = -0.06

[aero.limits]
alpha_stall_deg  = 18.0
max_g_structural =  7.33
min_g_structural = -3.0
max_mach         =  1.63

[aero.controls]
max_elevator_deg = 25.0
max_aileron_deg  = 20.0
max_rudder_deg   = 30.0

[engine]
fuel_flow_idle_kg_s = 0.05
fuel_flow_mil_kg_s  = 0.52
fuel_flow_ab_kg_s   = 1.55
spool_time_s        = 4.0

[engine.mil_thrust]
mach   = [0.0, 0.6, 0.9, 1.2, 1.6]
alt_km = [0.0, 6.0, 11.0, 15.0]
values = [
    22.2, 12.0,  6.4, 3.4,
    21.0, 12.6,  7.0, 3.8,
    20.4, 13.0,  7.6, 4.2,
    19.0, 12.6,  7.6, 4.3,
    16.6, 11.4,  7.0, 4.0,
]

[engine.ab_thrust]
mach   = [0.0, 0.6, 0.9, 1.2, 1.6]
alt_km = [0.0, 6.0, 11.0, 15.0]
values = [
    31.2, 17.4,  9.6, 5.2,
    33.0, 19.6, 11.0, 6.0,
    36.0, 22.0, 12.6, 7.0,
    38.0, 24.4, 14.4, 8.2,
    36.0, 24.0, 14.6, 8.6,
]
)";

TrimPoint at(float alt, float mass) {
    TrimPoint p;
    p.altitude_m = alt;
    p.mass_kg = mass;
    return p;
}

} // namespace

TEST_CASE("fm-trim: the builtin model trims to plausible, stable numbers", "[fm_trim]") {
    const auto& d = *BuiltinFlightModel::get();
    const TrimResult r = trim(d, at(1000.f, 0.f));

    REQUIRE(r.converged);
    CHECK(r.stall_speed_1g_mps > 0.f);
    CHECK(r.max_level_mach > 0.f);
    CHECK(r.instant_turn_deg_s > 0.f);
    CHECK(r.sustained_turn_deg_s > 0.f);
}

TEST_CASE("fm-trim: a light fighter's derived performance is physically coherent", "[fm_trim]") {
    const FlightModelData d = parseFlightModel(kLightFighter);
    const TrimResult r = trim(d, at(4572.f, 6500.f)); // 15 000 ft, combat weight

    REQUIRE(r.converged);

    // Stall speed in the right postcode for a 6.5 t jet on a 17 m^2 wing (~170 kt).
    CHECK(r.stall_speed_1g_mps > 70.f);
    CHECK(r.stall_speed_1g_mps < 110.f);

    // THE INVARIANT THAT CAUGHT A REAL BUG. Sustained turn can never equal, let alone exceed, the
    // instantaneous turn: one is what the engine can pay for, the other is what the wing can make.
    // The first version of this tool read the BODY-x force as excess thrust, where lift's sin(alpha)
    // component swamps drag at high alpha -- and it duly reported an aircraft sustaining its own
    // structural limit, forever, for free.
    CHECK(r.sustained_turn_deg_s < r.instant_turn_deg_s);
    CHECK(r.sustained_g < r.instant_g);
    CHECK(r.sustained_g > 1.f);

    // The wing is structure-limited, not lift-limited, at corner.
    CHECK(r.instant_g == Approx(7.33f).margin(0.05f));

    // Afterburner climbs harder than military power.
    CHECK(r.roc_mps_ab > r.roc_mps_mil);
    CHECK(r.roc_mps_mil > 0.f);
}

TEST_CASE("fm-trim: a payload measurably degrades performance", "[fm_trim]") {
    // Closes the loop with #812: the stores an entity carries by default cost it mass and drag, and
    // those numbers show up in its spec sheet rather than being invisible.
    const FlightModelData d = parseFlightModel(kLightFighter);

    const TrimResult clean = trim(d, at(4572.f, 6500.f));
    PayloadEffect stores{};
    stores.extra_mass_kg = 350.f;
    stores.extra_cd0 = 0.004f;
    const TrimResult armed = trim(d, at(4572.f, 6500.f), stores);

    REQUIRE(clean.converged);
    REQUIRE(armed.converged);

    CHECK(armed.stall_speed_1g_mps > clean.stall_speed_1g_mps); // heavier => stalls faster
    CHECK(armed.roc_mps_ab < clean.roc_mps_ab);                 // heavier and draggier => climbs worse
    CHECK(armed.sustained_turn_deg_s < clean.sustained_turn_deg_s);
}

TEST_CASE("fm-trim: an aircraft that cannot fly at an altitude reports it rather than guessing", "[fm_trim]") {
    // A model with no thrust left at 20 km has no performance there. Reporting `converged = false` is
    // the honest answer; inventing a max level Mach would be worse than saying so.
    const FlightModelData d = parseFlightModel(kLightFighter);
    const TrimResult r = trim(d, at(20000.f, 6500.f));

    CHECK_FALSE(r.converged);
}

TEST_CASE("fm-trim: the cd_table drag path is exercised", "[fm_trim]") {
    // #820's tabulated drag must reach the trim solver -- the whole reason cd_table exists is that
    // no parabolic polar can reproduce both a fighter's cruise drag and its sustained turn, and this
    // tool is where that claim is checked against the flight manual.
    std::string toml = kLightFighter;
    auto pos = toml.find("k             = 0.115");
    REQUIRE(pos != std::string::npos);
    toml.replace(pos, std::string("k             = 0.115").size(), "k             = 0.0");
    toml += "\n[aero.cd_table]\n"
            "alpha  = [-5.0, 0.0, 10.0, 18.0]\n"
            "mach   = [0.3, 0.9]\n"
            "values = [0.030, 0.035,\n"
            "          0.020, 0.024,\n"
            "          0.090, 0.100,\n"
            "          0.260, 0.280]\n";

    const FlightModelData d = parseFlightModel(toml);
    REQUIRE(d.cd_table.has_value());

    const TrimResult r = trim(d, at(4572.f, 6500.f));
    REQUIRE(r.converged);
    CHECK(r.sustained_turn_deg_s < r.instant_turn_deg_s);
    CHECK(r.sustained_g > 1.f);
}

TEST_CASE("fm-trim: --expect passes a model that meets its published numbers", "[fm_trim]") {
    const FlightModelData d = parseFlightModel(kLightFighter);
    const TrimResult truth = trim(d, at(4572.f, 6500.f));
    REQUIRE(truth.converged);

    // Author the model's OWN derived numbers as the expectation: it must, trivially, meet them.
    const std::string expect = "[[expect]]\n"
                               "metric = \"stall_speed_1g_mps\"\n"
                               "altitude_m = 4572\n"
                               "mass_kg = 6500\n"
                               "expected = " +
                               std::to_string(truth.stall_speed_1g_mps) +
                               "\n"
                               "tolerance = 0.05\n";

    const ExpectResult r = checkExpectations(d, expect);
    INFO("errors: " << (r.errors.empty() ? std::string("none") : r.errors[0]));
    CHECK(r.ok);
    CHECK(r.checked == 1);
}

TEST_CASE("fm-trim: --expect FAILS a model that has drifted from its flight manual", "[fm_trim]") {
    // THIS IS #54'S ACCEPTANCE GATE. A pack authors what the aircraft's manual publishes; when
    // someone retunes the drag polar and the aircraft stops matching, CI says so.
    const FlightModelData d = parseFlightModel(kLightFighter);

    const std::string expect = "[[expect]]\n"
                               "metric = \"stall_speed_1g_mps\"\n"
                               "altitude_m = 4572\n"
                               "mass_kg = 6500\n"
                               "expected = 40.0\n" // nowhere near the truth
                               "tolerance = 0.05\n";

    const ExpectResult r = checkExpectations(d, expect);
    CHECK_FALSE(r.ok);
    REQUIRE(r.failures.size() == 1);
    CHECK(r.failures[0].metric == "stall_speed_1g_mps");
    CHECK(r.failures[0].expected == Approx(40.f));
}

TEST_CASE("fm-trim: an unknown metric and a malformed file are reported", "[fm_trim]") {
    const FlightModelData d = parseFlightModel(kLightFighter);

    const ExpectResult bad = checkExpectations(d, "[[expect]]\nmetric = \"warp_factor\"\nexpected = 9.0\n");
    CHECK_FALSE(bad.ok);
    CHECK_FALSE(bad.errors.empty());

    const ExpectResult broken = checkExpectations(d, "this is not toml [[[");
    CHECK_FALSE(broken.ok);
    CHECK_FALSE(broken.errors.empty());

    const ExpectResult empty = checkExpectations(d, "[other]\nx = 1\n");
    CHECK_FALSE(empty.ok);
}

TEST_CASE("fm-trim: a model that outruns its own declared max_mach is an error", "[fm_trim]") {
    // #816 deliberately removed the artificial drag wall: an aircraft's top speed comes from drag
    // rising to meet thrust. If a model can exceed the max_mach it declares, the MODEL is wrong, and
    // it is caught here rather than papered over in the engine.
    std::string toml = kLightFighter;
    auto pos = toml.find("max_mach         =  1.63");
    REQUIRE(pos != std::string::npos);
    toml.replace(pos, std::string("max_mach         =  1.63").size(), "max_mach         =  0.30");

    const FlightModelData d = parseFlightModel(toml);
    ExpectResult r;
    checkMaxMach(d, r);

    CHECK_FALSE(r.ok);
    REQUIRE_FALSE(r.errors.empty());
    CHECK(r.errors[0].find("max_mach") != std::string::npos);
}

TEST_CASE("fm-trim: JSON output carries every metric", "[fm_trim]") {
    // The JSON is consumed by CI *and* by the in-game aircraft manual (#821), which renders these
    // numbers rather than duplicating them. A metric missing here is a blank line in the manual.
    const FlightModelData d = parseFlightModel(kLightFighter);
    const std::vector<TrimPoint> pts{at(0.f, 6500.f), at(11000.f, 6500.f)};
    const std::vector<TrimResult> rs{trim(d, pts[0]), trim(d, pts[1])};

    const std::string json = toJson(d, pts, rs);

    for (const char* key : {"aircraft", "limits", "max_g_structural", "stall_speed_1g_mps", "max_level_mach",
                            "roc_mps_mil", "roc_mps_ab", "sustained_turn_deg_s", "instant_turn_deg_s",
                            "corner_speed_mps", "fuel_flow_mil_kg_s", "specific_range_m_per_kg", "converged"}) {
        INFO("missing key: " << key);
        CHECK(json.find(key) != std::string::npos);
    }
}
