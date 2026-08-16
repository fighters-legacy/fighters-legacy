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

#include "flight/Atmosphere.h"
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

TEST_CASE("fm-trim: a cambered wing's negative trim alpha is a flight condition, not a failure", "[fm_trim]") {
    // A LEF-scheduled or cambered deck carries positive CL at alpha 0 (the F-16A's is +0.10),
    // so above a modest speed the 1 g trim alpha is NEGATIVE. alphaForLoad's old -1 sentinel
    // made every caller read that as "the wing cannot hold it": the max-level scan broke at
    // the alpha = 0 crossing (M ~0.4 here, ~0.65 for the real F-16A at sea level) and reported
    // a top speed that FELL as fuel burned off. The sentinel is NaN now; a negative alpha is
    // an answer. Every aircraft authored before the F-16A had a symmetric table, which is why
    // published cambered data had to arrive before this could be seen.
    std::string cambered{kLightFighter};
    const auto tbl = cambered.find("[aero.cl_table]");
    const auto val = cambered.find("values = [", tbl);
    const auto end = cambered.find("]", val);
    cambered.replace(val, end - val + 1,
                     "values = [\n"
                     "     0.00, 0.00,-0.02, 0.02,\n"
                     "     0.30, 0.30, 0.32, 0.26,\n"
                     "     0.65, 0.68, 0.73, 0.58,\n"
                     "     1.03, 1.09, 1.17, 0.88,\n"
                     "     1.35, 1.43, 1.53, 1.14,\n"
                     "     1.49, 1.57, 1.67, 1.24,\n"
                     "     1.40, 1.47, 1.55, 1.16,\n"
                     "     1.20, 1.25, 1.33, 1.00,\n"
                     "]");
    const FlightModelData d = parseFlightModel(cambered);

    // Sea level, full gross: the alpha = 0 crossing sits near M 0.41. The aircraft has thrust
    // for far more than that; the scan must sail through the crossing, not break on it.
    TrimPoint p = at(0.f, 6349.f);
    const TrimResult r = trim(d, p, {});
    REQUIRE(r.converged);
    CHECK(r.max_level_mach > 0.6f);

    // The pinned-Mach path takes the same sentinel: Ps at 1 g above the crossing must be a
    // real number, not the "no such flight condition" zero.
    p.mach = 0.55f;
    p.load_factor = 1.f;
    const TrimResult rp = trim(d, p, {});
    REQUIRE(rp.converged);
    CHECK(rp.ps_mps > 0.f);
}

TEST_CASE("fm-trim: an aircraft that cannot fly at an altitude reports it rather than guessing", "[fm_trim]") {
    // Heavy, in the stratosphere: there is no speed at which the engine can pay for the drag, so the
    // aircraft genuinely has no performance here. Reporting `converged = false` is the honest answer;
    // inventing a max level Mach would be worse than saying so.
    //
    // THIS TEST USED TO ASSERT 20 km / 6500 kg, WHICH THE AIRCRAFT CAN ACTUALLY FLY. It passed only
    // because the solver broke out of its scan at the first negative excess thrust -- the back side of
    // the power curve (#825) -- and declared the aeroplane unflyable. The assertion was encoding the
    // bug, not the aircraft's limits, which is exactly how a wrong test outlives the code it guards.
    const FlightModelData d = parseFlightModel(kLightFighter);
    const TrimResult r = trim(d, at(15000.f, 12000.f));

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

// ---------------------------------------------------------------------------
// The back side of the power curve (#825)
//
// The level-flight drag curve is U-SHAPED. At the stall speed the wing is at CL_max, where induced
// drag is enormous -- the region of reversed command. As the aircraft accelerates, CL falls, induced
// drag collapses, total drag reaches a minimum at best L/D, and only THEN climbs again toward max
// speed. So excess thrust is routinely negative AT the stall, positive across the whole usable
// envelope, and negative again past max speed.
//
// The original solver broke out of the scan at the first negative, on the assumption that "everything
// faster is unreachable". It saw the back side, concluded the aircraft could not fly, and reported
// `converged = false` -- for an F-5E with fourteen kilonewtons of excess thrust in the middle of its
// envelope. It read like a content bug and would have sent the next author hunting through a drag
// table for a fault that was not there.
// ---------------------------------------------------------------------------

TEST_CASE("fm-trim: an aircraft on the back side of its power curve still trims", "[fm_trim][back_side]") {
    const FlightModelData d = parseFlightModel(kLightFighter);

    // High and heavy: thin air plus weight puts the stall speed deep into the region of reversed
    // command, where drag at CL_max exceeds the thrust available. Stall is 148 m/s here, but the
    // slowest speed the engine can actually sustain is 180 m/s. The old solver saw the negative
    // excess thrust at 148, broke, and declared the aircraft unable to hold level flight.
    const TrimResult r = trim(d, at(11000.f, 9000.f));

    REQUIRE(r.converged);
    CHECK(r.max_level_mach > 0.f);

    // Prove the back side is actually PRESENT here -- otherwise this test would pass against the buggy
    // solver too, and prove nothing. The engine cannot sustain flight at the stall speed: the slowest
    // speed it can hold is strictly faster than the speed at which the wing runs out of lift.
    CHECK(r.min_level_speed_mps > r.stall_speed_1g_mps);
}

TEST_CASE("fm-trim: min level speed is the slowest the ENGINE can sustain, not the stall", "[fm_trim][back_side]") {
    // Above the stall, on the back side, the wing still carries the weight but the engine cannot pay
    // for the drag -- so the aircraft sinks. That gap is the whole point of the metric, and it is the
    // honest answer to "how slow can this thing actually go".
    const FlightModelData d = parseFlightModel(kLightFighter);
    const TrimResult r = trim(d, at(11000.f, 9000.f));

    REQUIRE(r.converged);
    CHECK(r.min_level_speed_mps > r.stall_speed_1g_mps); // ~180 m/s vs a 148 m/s stall
    CHECK(r.max_level_mach > 0.f);
}

TEST_CASE("fm-trim: no regression at sea level, where the bug did not bite", "[fm_trim][back_side]") {
    // At sea level thrust is usually large enough to overcome even stall-speed drag, which is why the
    // bug went unnoticed: min level speed IS the stall speed there.
    const FlightModelData d = parseFlightModel(kLightFighter);
    const TrimResult r = trim(d, at(0.f, 6500.f));

    REQUIRE(r.converged);
    CHECK(r.min_level_speed_mps == Approx(r.stall_speed_1g_mps).margin(4.f));
}

TEST_CASE("fm-trim: an aircraft with NO usable speed still reports not-converged", "[fm_trim][back_side]") {
    // Removing the early break must not turn "cannot fly here" into a false positive. When NO speed in
    // the range has non-negative excess thrust, that IS the real condition, and it must still be said.
    const FlightModelData d = parseFlightModel(kLightFighter);
    const TrimResult r = trim(d, at(15000.f, 12000.f)); // heavy, in the stratosphere: no speed works

    CHECK_FALSE(r.converged);
    CHECK(r.min_level_speed_mps == Approx(0.f));
}

// ---------------------------------------------------------------------------
// Pinned conditions: per-Mach, per-load-factor, per-row payload (#826)
//
// Published flight-manual data is almost never a speed-maximised value. Every F-5E turn number in
// T.O. 1F-5E-1 is quoted AT A MACH (15 000 ft, M 0.60: sustained 3.3 g), and its specific-excess-power
// ladder is quoted at a Mach AND a load factor (M 0.60, n = 4.0 -> Ps = -225 ft/s). Those are not the
// same quantities as "the best turn the aircraft can manage at any speed", so they could not be
// compared against the maximised numbers -- which meant the aircraft's RICHEST drag constraint, the
// one [aero.cd_table] is actually fitted to, was the one the gate could not protect.
// ---------------------------------------------------------------------------

TEST_CASE("fm-trim: pinning a Mach evaluates AT it rather than maximising over speed", "[fm_trim][pinned]") {
    const FlightModelData d = parseFlightModel(kLightFighter);

    TrimPoint maximised = at(4572.f, 6137.f);
    TrimPoint pinned = at(4572.f, 6137.f);
    pinned.mach = 0.60f;

    const TrimResult best = trim(d, maximised);
    const TrimResult atMach = trim(d, pinned);

    REQUIRE(best.converged);
    REQUIRE(atMach.converged);

    // The best turn the aircraft can manage anywhere is at least as good as its turn at one
    // arbitrary Mach -- and in general strictly better, which is exactly why the two numbers cannot
    // be compared against each other.
    CHECK(atMach.sustained_turn_deg_s <= best.sustained_turn_deg_s);
    // The evaluation speed is the pinned Mach at THIS altitude's speed of sound (~322 m/s at 15 000 ft),
    // not at sea level's.
    const float aSound = computeAtmosphere(4572.f).speed_of_sound_m_s;
    CHECK(atMach.corner_speed_mps == Approx(0.60f * aSound).epsilon(0.02));
}

TEST_CASE("fm-trim: max_lift_g at a pinned Mach is the wing's limit, not the structure's", "[fm_trim][pinned]") {
    const FlightModelData d = parseFlightModel(kLightFighter);

    TrimPoint pt = at(4572.f, 6137.f);
    pt.mach = 0.60f;
    const TrimResult r = trim(d, pt);

    REQUIRE(r.converged);
    // It is what pins CL_max, so it must NOT be clipped to max_g_structural -- an aircraft that can
    // pull more lift than its airframe can take is a real and important thing to know.
    CHECK(r.max_lift_g > 1.f);
    CHECK(r.sustained_g <= r.max_lift_g);
}

TEST_CASE("fm-trim: ps_mps falls with load factor and crosses zero at the sustained condition", "[fm_trim][pinned]") {
    // THE PS LADDER. This is the shape of the T.O.'s table, and the whole reason the metric exists:
    // pulling harder costs energy, and the load factor at which Ps hits zero IS the sustained turn.
    const FlightModelData d = parseFlightModel(kLightFighter);

    auto psAt = [&](float n) {
        TrimPoint pt = at(4572.f, 6137.f);
        pt.mach = 0.60f;
        pt.load_factor = n;
        return trim(d, pt);
    };

    const TrimResult ps1 = psAt(1.0f);
    const TrimResult ps3 = psAt(3.0f);
    const TrimResult ps5 = psAt(5.0f);

    REQUIRE(ps1.converged);

    // Monotonic in n: every extra g costs energy.
    CHECK(ps1.ps_mps > ps3.ps_mps);
    CHECK(ps3.ps_mps > ps5.ps_mps);

    // At 1 g with afterburner the aircraft is climbing; pulled hard enough it is bleeding energy.
    CHECK(ps1.ps_mps > 0.f);
    CHECK(ps5.ps_mps < 0.f);

    // And Ps = 0 IS the sustained turn -- the two definitions must agree, or one of them is wrong.
    const float nSustained = ps1.sustained_g;
    const TrimResult atSustained = psAt(nSustained);
    CHECK(atSustained.ps_mps == Approx(0.f).margin(3.f));
}

TEST_CASE("fm-trim: afterburner is honoured per point", "[fm_trim][pinned]") {
    const FlightModelData d = parseFlightModel(kLightFighter);

    TrimPoint ab = at(4572.f, 6137.f);
    ab.mach = 0.60f;
    ab.load_factor = 2.0f;
    ab.afterburner = true;

    TrimPoint mil = ab;
    mil.afterburner = false;

    CHECK(trim(d, ab).ps_mps > trim(d, mil).ps_mps); // burner buys energy
}

TEST_CASE("fm-trim: a per-row payload gates the store-drag path", "[fm_trim][pinned]") {
    // The published clean-vs-loaded max-Mach delta is the ONLY check there is on the whole
    // hardpoints -> WeaponLoad.drag_factor -> PayloadEffect path, and it needs two conditions in ONE
    // file -- which a single CLI --payload flag cannot express.
    const FlightModelData d = parseFlightModel(kLightFighter);

    const std::string expect = "[[expect]]\n"
                               "metric = \"max_level_mach\"\n"
                               "altitude_m = 10973\n"
                               "mass_kg = 6137\n"
                               "expected = 99.0\n" // deliberately unreachable, so we can read `actual`
                               "tolerance = 0.01\n"
                               "\n"
                               "[[expect]]\n"
                               "metric = \"max_level_mach\"\n"
                               "altitude_m = 10973\n"
                               "mass_kg = 6137\n"
                               "payload_kg = 171.0\n"
                               "payload_cd0 = 0.0021\n"
                               "expected = 99.0\n"
                               "tolerance = 0.01\n";

    const ExpectResult r = checkExpectations(d, expect);
    REQUIRE(r.failures.size() == 2);

    // Row 2 carries the stores, so it must be measurably slower than row 1. If the per-row payload
    // were ignored, these would be identical -- and the store-drag path would be ungated.
    CHECK(r.failures[1].actual < r.failures[0].actual);
}

TEST_CASE("fm-trim: ps_mps and max_lift_g demand the condition they need", "[fm_trim][pinned]") {
    // Without a Mach there is no condition to evaluate them at. Silently returning 0 would look like a
    // failing model rather than a malformed row, and would send an author to debug their aircraft.
    const FlightModelData d = parseFlightModel(kLightFighter);

    const ExpectResult noMach = checkExpectations(d, "[[expect]]\nmetric = \"ps_mps\"\naltitude_m = 4572\n"
                                                     "mass_kg = 6137\nexpected = 0.0\n");
    CHECK_FALSE(noMach.ok);
    CHECK_FALSE(noMach.errors.empty());

    const ExpectResult noN = checkExpectations(d, "[[expect]]\nmetric = \"ps_mps\"\naltitude_m = 4572\n"
                                                  "mach = 0.6\nmass_kg = 6137\nexpected = 0.0\n");
    CHECK_FALSE(noN.ok);

    const ExpectResult ok = checkExpectations(d, "[[expect]]\nmetric = \"max_lift_g\"\naltitude_m = 4572\n"
                                                 "mach = 0.6\nmass_kg = 6137\nexpected = 5.2\ntolerance = 0.5\n");
    INFO("errors: " << (ok.errors.empty() ? std::string("none") : ok.errors[0]));
    CHECK(ok.checked == 1);
}

// --- #1181: the dynamic-pressure placard caps level speed, and only where it binds ----------------

TEST_CASE("fm-trim: a KEAS placard caps max level speed at low altitude", "[fm_trim]") {
    // Real top speed is the LESSER of what thrust can push and what the airframe is cleared to
    // withstand. Before this field only the Mach half was expressible, so an aircraft with thrust to
    // spare down low flew as fast as its drag allowed — for the B-1B, ~11% past its published limit
    // in exactly the low-level regime it exists for.
    const FlightModelData clean = parseFlightModel(kLightFighter);
    const TrimResult unplacarded = trim(clean, at(0.f, 6500.f));
    REQUIRE(unplacarded.converged);

    // Placard it a good way below whatever it was reaching, and the cap must bite.
    std::string s(kLightFighter);
    const auto pos = s.find("max_mach");
    REQUIRE(pos != std::string::npos);
    s.insert(pos, "max_keas         = 300.0\n");

    const FlightModelData placarded = parseFlightModel(s);
    REQUIRE(placarded.limits.max_keas == Catch::Approx(300.f));
    const TrimResult limited = trim(placarded, at(0.f, 6500.f));
    REQUIRE(limited.converged);

    INFO("unplacarded " << unplacarded.max_level_mach << " M, placarded " << limited.max_level_mach << " M");
    CHECK(limited.max_level_mach < unplacarded.max_level_mach);

    // 300 kn EAS at sea level is 300 kn TAS, ~M0.45. Allow a step of scan granularity either side.
    CHECK(limited.max_level_mach == Catch::Approx(0.453f).margin(0.03f));
}

TEST_CASE("fm-trim: a KEAS placard does NOT bind in the stratosphere", "[fm_trim]") {
    // The whole reason the field is EAS and not TAS or Mach: one number covers every altitude,
    // biting hard down low and not at all up high, which is exactly how a real placard behaves.
    std::string s(kLightFighter);
    const auto pos = s.find("max_mach");
    REQUIRE(pos != std::string::npos);
    s.insert(pos, "max_keas         = 710.0\n"); // the F-5E / T-38A placard

    const FlightModelData placarded = parseFlightModel(s);
    const FlightModelData clean = parseFlightModel(kLightFighter);

    const TrimResult hiPlacard = trim(placarded, at(12000.f, 6500.f));
    const TrimResult hiClean = trim(clean, at(12000.f, 6500.f));
    REQUIRE(hiPlacard.converged);
    REQUIRE(hiClean.converged);

    // At 12 km, 710 KEAS is far faster than this jet can go, so the placard is inert.
    CHECK(hiPlacard.max_level_mach == Catch::Approx(hiClean.max_level_mach));
}

TEST_CASE("fm-trim: omitting the placard changes nothing", "[fm_trim]") {
    // Every existing flight model omits this field; none of them may move.
    const FlightModelData d = parseFlightModel(kLightFighter);
    CHECK(d.limits.max_keas == Catch::Approx(0.f));
    const TrimResult r = trim(d, at(4572.f, 6500.f));
    CHECK(r.converged);
}

namespace {

// kLightFighter with a variable-sweep wing: spread below M0.5, fully swept past M0.8, and a swept
// configuration that visibly costs lift (cl_scale 0.65). The spread configuration is deliberately
// the identity (1.0 / 1.0 / 0.0) so that any metric evaluated with the wings forward must equal the
// fixed-wing model's number exactly — which is what makes the per-Mach resolution provable.
std::string withVariableSweep(float refSweepDeg) {
    std::string s(kLightFighter);
    s += "\n[wing_sweep]\n"
         "ref_sweep_deg   = " +
         std::to_string(refSweepDeg) +
         "\n"
         "min_deg         = 20.0\n"
         "max_deg         = 68.0\n"
         "slew_rate_deg_s = 7.5\n"
         "[wing_sweep.schedule]\n"
         "mach  = [0.0, 0.5, 0.8, 2.0]\n"
         "sweep = [20.0, 20.0, 68.0, 68.0]\n"
         "[wing_sweep.spread]\n"
         "cl_scale  = 1.0\n"
         "k_scale   = 1.0\n"
         "cd0_delta = 0.0\n"
         "[wing_sweep.swept]\n"
         "cl_scale  = 0.65\n"
         "k_scale   = 1.7\n"
         "cd0_delta = -0.002\n";
    return s;
}

} // namespace

TEST_CASE("fm-trim: a variable-sweep wing is measured where the schedule puts it (#1187)", "[fm_trim][wing_sweep]") {
    // The integrator flies the schedule — sweep is looked up from Mach every tick — so the tool must
    // evaluate the same configuration or the acceptance gate measures an aircraft that never flies.
    // Before #1187 it passed ref_sweep_deg at EVERY Mach, which for the B-1B meant gating the
    // spread wing at M1.25, a configuration the schedule leaves behind at M0.7.
    const FlightModelData fixedWing = parseFlightModel(kLightFighter);
    const FlightModelData vg = parseFlightModel(withVariableSweep(20.f));

    // Low speed = wings forward = the identity configuration: the stall must not move. This is the
    // half that proves the schedule is resolved at each metric's OWN Mach, not once per run.
    const TrimResult vgLow = trim(vg, at(4572.f, 6500.f));
    const TrimResult fixedLow = trim(fixedWing, at(4572.f, 6500.f));
    REQUIRE(vgLow.converged);
    REQUIRE(fixedLow.converged);
    CHECK(vgLow.stall_speed_1g_mps == Approx(fixedLow.stall_speed_1g_mps));

    // Pinned past the schedule's knee = wings aft: max_lift_g is lift-limited, and CL scales
    // linearly by cl_scale, so the swept number is 0.65 x the fixed-wing number EXACTLY. Before the
    // fix both evaluated spread and this ratio was 1.0 — [wing_sweep.swept] was dead to the tool.
    TrimPoint highMach = at(4572.f, 6500.f);
    highMach.mach = 0.9f;
    const TrimResult vgHigh = trim(vg, highMach);
    const TrimResult fixedHigh = trim(fixedWing, highMach);
    REQUIRE(vgHigh.converged);
    REQUIRE(fixedHigh.converged);
    CHECK(vgHigh.max_lift_g == Approx(0.65f * fixedHigh.max_lift_g).epsilon(0.01f));
}

TEST_CASE("fm-trim: ref_sweep_deg no longer leaks into the measurement (#1187)", "[fm_trim][wing_sweep]") {
    // ref_sweep_deg says where the base tables were MEASURED, not where the aircraft flies. Two
    // models differing only in the reference must trim identically, because the schedule — the same
    // in both — decides the configuration at every Mach. Before the fix these two disagreed on
    // every number the tool prints.
    const FlightModelData refSpread = parseFlightModel(withVariableSweep(20.f));
    const FlightModelData refSwept = parseFlightModel(withVariableSweep(68.f));

    const TrimResult a = trim(refSpread, at(4572.f, 6500.f));
    const TrimResult b = trim(refSwept, at(4572.f, 6500.f));
    REQUIRE(a.converged);
    REQUIRE(b.converged);
    CHECK(a.stall_speed_1g_mps == b.stall_speed_1g_mps);
    CHECK(a.max_level_mach == b.max_level_mach);
    CHECK(a.sustained_turn_deg_s == b.sustained_turn_deg_s);
    CHECK(a.roc_mps_ab == b.roc_mps_ab);
}

TEST_CASE("fm-trim: a wing_sweep block with no schedule falls back to the reference sweep (#1187)",
          "[fm_trim][wing_sweep]") {
    // The parser refuses [wing_sweep] without a schedule, so this arm exists for programmatically
    // built data only — but it must not assert or diverge from the integrator's init state, which
    // parks an unscheduled wing at ref_sweep_deg too.
    FlightModelData d = parseFlightModel(withVariableSweep(20.f));
    d.wing_sweep->schedule.keys.clear();
    d.wing_sweep->schedule.values.clear();

    const FlightModelData fixedWing = parseFlightModel(kLightFighter);
    const TrimResult r = trim(d, at(4572.f, 6500.f));
    const TrimResult fixedR = trim(fixedWing, at(4572.f, 6500.f));
    REQUIRE(r.converged);
    // Parked at ref = 20 deg = full spread = the identity configuration: every number matches the
    // fixed-wing model.
    CHECK(r.stall_speed_1g_mps == fixedR.stall_speed_1g_mps);
    CHECK(r.max_level_mach == fixedR.max_level_mach);
}
