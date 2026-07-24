// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "flight/AeroForces.h"
#include "flight/Atmosphere.h"
#include "flight/FlightModelParser.h"

#include <cmath>
#include <string>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using namespace fl;

static const std::string kGenericToml = R"(
[aircraft]
name         = "Test"
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
alpha  = [-10.0, -5.0, 0.0, 5.0, 10.0, 15.0, 18.0, 20.0, 25.0]
mach   = [0.3, 0.6, 0.9]
values = [
    -0.55,-0.60,-0.66,
    -0.20,-0.22,-0.24,
     0.05, 0.06, 0.07,
     0.40, 0.45, 0.52,
     0.75, 0.84, 0.97,
     1.05, 1.18, 1.36,
     1.18, 1.32, 1.52,
     1.10, 1.23, 1.42,
     0.85, 0.95, 1.10,
]

[aero.drag_polar]
cd0           = 0.018
k             = 0.14
speedbrake_cd = 0.08
gear_cd       = 0.03

[aero.cd_wave]
mach   = [0.70, 0.85, 0.95, 1.00, 1.10, 1.50]
values = [0.000, 0.010, 0.040, 0.036, 0.018, 0.003]

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

static FlightModelData makeData() {
    return parseFlightModel(kGenericToml);
}

TEST_CASE("Lift is zero at zero alpha and Mach 0.6", "[aero]") {
    auto data = makeData();
    auto atmos = computeAtmosphere(0.f);
    PayloadEffect payload{};
    // alpha=0, beta=0, Mach=0.6 (speed = 0.6 * 340.3 ≈ 204 m/s)
    float spd = 0.6f * atmos.speed_of_sound_m_s;
    auto f = computeForces(0.f, 0.f, 0.6f, spd, 0.f, 55.f, false, 0.f, payload, data, atmos, ArticulationState{});
    // At alpha=0 CL≈0.06; lift is small but positive (forces[1] = lift*cos(alpha) > 0).
    // Mainly verifying no crash and values are finite.
    CHECK(std::isfinite(f[0]));
    CHECK(std::isfinite(f[1]));
    CHECK(std::isfinite(f[2]));
}

TEST_CASE("Stall region CL is lower than pre-stall peak CL", "[aero]") {
    auto data = makeData();
    float mach = 0.3f;
    // Stall peak (18 deg) CL > deep stall (25 deg) CL
    float cl_peak = data.cl_table.lookup(18.f, mach);
    float cl_deep = data.cl_table.lookup(25.f, mach);
    CHECK(cl_peak > cl_deep);
}

TEST_CASE("Wave drag makes transonic CD higher than subsonic CD at same CL", "[aero]") {
    auto data = makeData();
    // CL at 5 deg, Mach 0.3 vs Mach 0.95
    float cl_sub = data.cl_table.lookup(5.f, 0.3f);
    float cl_tran = data.cl_table.lookup(5.f, 0.95f);
    float cd_sub = data.drag_polar.cd0 + data.drag_polar.k * cl_sub * cl_sub;
    float cd_wave = data.cd_wave->lookup(0.95f);
    float cd_tran = data.drag_polar.cd0 + data.drag_polar.k * cl_tran * cl_tran + cd_wave;
    CHECK(cd_tran > cd_sub);
    CHECK(cd_wave > 0.f);
}

TEST_CASE("Speedbrake and gear drag stack correctly", "[aero]") {
    auto data = makeData();
    auto atmos = computeAtmosphere(0.f);
    PayloadEffect payload{};
    float spd = 0.3f * atmos.speed_of_sound_m_s;

    // Device drag follows the actuator POSITION, not the command (#842).
    ArticulationState clean{};
    ArticulationState with_brake{};
    with_brake.speedbrake = 1.f;
    ArticulationState with_gear{};
    with_gear.gear = 1.f;
    ArticulationState both{};
    both.speedbrake = 1.f;
    both.gear = 1.f;

    auto f_clean = computeForces(0.f, 0.f, 0.3f, spd, 0.f, 55.f, false, 0.f, payload, data, atmos, clean);
    auto f_brake = computeForces(0.f, 0.f, 0.3f, spd, 0.f, 55.f, false, 0.f, payload, data, atmos, with_brake);
    auto f_gear = computeForces(0.f, 0.f, 0.3f, spd, 0.f, 55.f, false, 0.f, payload, data, atmos, with_gear);
    auto f_both = computeForces(0.f, 0.f, 0.3f, spd, 0.f, 55.f, false, 0.f, payload, data, atmos, both);

    // Forward force (x) should be smaller (more drag) when speedbrake or gear are deployed
    CHECK(f_brake[0] < f_clean[0]);
    CHECK(f_gear[0] < f_clean[0]);
    CHECK(f_both[0] < f_brake[0]);
    CHECK(f_both[0] < f_gear[0]);
}

TEST_CASE("Zero speed produces zero aerodynamic forces without NaN", "[aero]") {
    auto data = makeData();
    auto atmos = computeAtmosphere(0.f);
    PayloadEffect payload{};
    auto f = computeForces(0.f, 0.f, 0.f, 0.f, 0.f, 55.f, false, 0.f, payload, data, atmos, ArticulationState{});
    CHECK(std::isfinite(f[0]));
    CHECK(std::isfinite(f[1]));
    CHECK(std::isfinite(f[2]));
    // Dynamic pressure is zero, so aero forces are zero (thrust also 0 at throttle=0)
    CHECK_THAT(f[0], WithinAbs(0.f, 1e-3f));
    CHECK_THAT(f[1], WithinAbs(0.f, 1e-3f));
    CHECK_THAT(f[2], WithinAbs(0.f, 1e-3f));
}

TEST_CASE("Zero speed produces finite moments without NaN", "[aero]") {
    auto data = makeData();
    auto atmos = computeAtmosphere(0.f);
    ControlInput ctrl{};
    auto m = computeMoments(0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, ctrl, data, atmos, ArticulationState{});
    CHECK(std::isfinite(m[0]));
    CHECK(std::isfinite(m[1]));
    CHECK(std::isfinite(m[2]));
}

TEST_CASE("Payload drag and mass accumulate correctly", "[aero]") {
    auto data = makeData();
    auto atmos = computeAtmosphere(0.f);

    PayloadEffect no_payload{};
    PayloadEffect with_payload{.extra_mass_kg = 500.f, .extra_cd0 = 0.016f};

    float spd = 0.6f * atmos.speed_of_sound_m_s;
    auto f0 = computeForces(0.f, 0.f, 0.6f, spd, 0.f, 55.f, false, 0.8f, no_payload, data, atmos, ArticulationState{});
    auto fp =
        computeForces(0.f, 0.f, 0.6f, spd, 0.f, 55.f, false, 0.8f, with_payload, data, atmos, ArticulationState{});
    // Higher drag from payload → less forward force
    CHECK(fp[0] < f0[0]);
}

TEST_CASE("AeroForces: lift is in the Y-up direction at positive alpha", "[aero]") {
    // Regression test for the Y-up body-frame alignment.
    // With body frame [x=fwd, y=up, z=right], aerodynamic lift must be positive in
    // forces[1] (the upward axis), not negative in forces[2] (the old Z-down convention).
    auto data = makeData();
    auto atmos = computeAtmosphere(0.f);
    PayloadEffect payload{};
    float spd = 0.4f * atmos.speed_of_sound_m_s; // subsonic, well-behaved alpha

    auto f = computeForces(10.f * 3.14159f / 180.f, 0.f, 0.4f, spd, 0.f, 55.f, false, 0.f, payload, data, atmos,
                           ArticulationState{});

    CHECK(f[1] > 0.f);  // upward aerodynamic force is positive Y
    CHECK(f[2] == 0.f); // no lateral force
}

// ---------------------------------------------------------------------------
// [aero.cd_table] — tabulated total clean drag (#820)
//
// A strictly parabolic polar (cd0 + k*CL^2) forces specific excess power to be exactly quadratic in
// load factor, which means the implied induced-drag coefficient must be CONSTANT across a Ps chart.
// Real fighters do not behave that way -- against T.O. 1F-5E-1 the implied coefficient grows 3.5x
// from the 1-2 g region to the 4-5 g region. There is no value of k that fits both cruise and the
// hard-turn end, and NASA TP-1538's F-16 CD tables cannot be transcribed into a polar at all.
// ---------------------------------------------------------------------------

// kGenericToml with a cd_table appended and drag_polar.k zeroed (the table owns induced drag).
// The table's values are EXACTLY parabolic (cd0 + k*CL^2 at the fixture's own cl_table), so the two
// paths must agree at the breakpoints -- which is what makes the divergence test below meaningful.
static std::string withCdTable(const char* values) {
    std::string s = kGenericToml;
    auto pos = s.find("k             = ");
    REQUIRE(pos != std::string::npos);
    auto end = s.find('\n', pos);
    s.replace(pos, end - pos, "k             = 0.0");
    s += std::string("\n[aero.cd_table]\n"
                     "alpha  = [-10.0, 0.0, 10.0, 20.0]\n"
                     "mach   = [0.3, 0.9]\n"
                     "values = [\n") +
         values + "\n]\n";
    return s;
}

TEST_CASE("cd_table parses and is absent by default", "[aero][cd_table]") {
    auto plain = makeData();
    CHECK_FALSE(plain.cd_table.has_value()); // no regression: the parabolic path is untouched

    auto tabled = parseFlightModel(withCdTable("0.10, 0.12,\n0.02, 0.03,\n0.08, 0.10,\n0.30, 0.34,"));
    REQUIRE(tabled.cd_table.has_value());
    CHECK(tabled.cd_table->rows.size() == 4);
    CHECK(tabled.cd_table->cols.size() == 2);
}

TEST_CASE("cd_table REPLACES the parabolic polar rather than adding to it", "[aero][cd_table]") {
    // Build a table whose value at (alpha=0, mach=0.3) is a known constant, then check the drag that
    // comes out is that constant -- NOT that constant plus cd0 + k*CL^2. Summing them would be a
    // silent ~2x drag bug that no content author could debug.
    const float kCd = 0.05f;
    auto data = parseFlightModel(withCdTable("0.05, 0.05,\n0.05, 0.05,\n0.05, 0.05,\n0.05, 0.05,"));
    REQUIRE(data.cd_table.has_value());

    auto atmos = computeAtmosphere(0.f);
    PayloadEffect payload{};
    const float mach = 0.3f;
    const float spd = mach * atmos.speed_of_sound_m_s;
    const float q = 0.5f * atmos.density_kg_m3 * spd * spd;
    const float S = data.geometry.wing_area_m2;

    // alpha = 0 so lift ~ 0 and the body-x force is (thrust=0) - drag*cos(0) + lift*sin(0) = -drag.
    auto f = computeForces(0.f, 0.f, mach, spd, 0.f, 55.f, false, 0.f, payload, data, atmos, ArticulationState{});

    const float cl = data.cl_table.lookup(0.f, mach);
    const float expectedDrag = q * S * kCd; // wave drag is 0 at Mach 0.3 in this fixture
    const float parabolicWouldBe = q * S * (kCd + data.drag_polar.cd0 + 0.14f * cl * cl);

    const float lift = q * S * cl;
    const float actualDrag = -(f[0] - lift * std::sin(0.f));

    CHECK_THAT(actualDrag, WithinAbs(expectedDrag, expectedDrag * 0.01f));
    CHECK(actualDrag < parabolicWouldBe * 0.95f); // decisively NOT the summed version
}

TEST_CASE("a cd_table can express drag rising faster than CL^2", "[aero][cd_table]") {
    // THE POINT OF THE FEATURE. Fit the table's low-alpha end to a parabola, then let the high-alpha
    // end rise far above what any k could produce there. A parabolic polar cannot do this, which is
    // why the F-5E out-turns the real aircraft by 18% when k is fitted to cruise.
    auto data = parseFlightModel(withCdTable("0.05, 0.05,\n0.02, 0.02,\n0.09, 0.09,\n0.60, 0.60,"));
    REQUIRE(data.cd_table.has_value());

    const float mach = 0.3f;
    const float cd_cruise = data.cd_table->lookup(0.f, mach);    // near-zero lift
    const float cd_hardTurn = data.cd_table->lookup(20.f, mach); // at high AoA

    const float cl_cruise = data.cl_table.lookup(0.f, mach);
    const float cl_hardTurn = data.cl_table.lookup(20.f, mach);

    // The implied k is NOT constant -- it grows sharply toward max lift, exactly as a real wing does.
    const float k_cruise = (cd_cruise - 0.02f) / (cl_cruise * cl_cruise + 1e-6f);
    const float k_hardTurn = (cd_hardTurn - 0.02f) / (cl_hardTurn * cl_hardTurn + 1e-6f);
    CHECK(k_hardTurn > k_cruise * 2.f);
}

TEST_CASE("a model with no cd_table produces bit-identical forces", "[aero][cd_table]") {
    // The regression guard: every existing aircraft, and most community content, uses the parabolic
    // path and must be completely unaffected by this feature.
    auto data = makeData();
    auto atmos = computeAtmosphere(5000.f);
    ArticulationState art{};
    art.speedbrake = 0.3f; // POSITION, not command (#842)
    PayloadEffect payload{80.f, 0.002f};

    auto f = computeForces(0.09f, 0.f, 0.7f, 220.f, 5000.f, 55.f, false, 0.6f, payload, data, atmos, art);

    // Recompute the pre-#820 expression by hand and require an exact match.
    const float alpha_deg = 0.09f / (3.14159265358979f / 180.f);
    const float cl = data.cl_table.lookup(alpha_deg, 0.7f);
    const float q = 0.5f * atmos.density_kg_m3 * 220.f * 220.f;
    const float cd0 = data.drag_polar.cd0 + payload.extra_cd0;
    const float cd_wave = data.cd_wave ? data.cd_wave->lookup(0.7f) : 0.f;
    const float cd_dev = 0.3f * data.drag_polar.speedbrake_cd;
    const float cd = cd0 + data.drag_polar.k * cl * cl + cd_wave + cd_dev;
    const float drag = q * data.geometry.wing_area_m2 * cd;
    const float lift = q * data.geometry.wing_area_m2 * cl;

    const float expected_y = lift * std::cos(0.09f) + drag * std::sin(0.09f);
    CHECK_THAT(f[1], WithinAbs(expected_y, std::abs(expected_y) * 1e-4f));
}

// ---------------------------------------------------------------------------
// Idle-thrust deck (#898)
//
// Below MIL the engine used to model thrust as a straight throttle x mil line with no idle deck. Real
// turbofan idle thrust is neither zero nor linear: at altitude and speed it goes NEGATIVE (ram drag >
// idle gross thrust). An optional [engine.idle_thrust] table blends idle -> mil across throttle.
// ---------------------------------------------------------------------------

// kGenericToml with an idle deck on the same (mach, alt_km) grid as mil_thrust. Static idle is a small
// positive; at M 0.9 it is strongly negative, the ram-drag regime this feature exists to model.
static std::string withIdleThrust() {
    return kGenericToml + R"(
[engine.idle_thrust]
mach   = [0.0, 0.3, 0.9]
alt_km = [0.0, 12.0]
values = [2.8, 1.0, 1.0, -2.0, -16.0, -10.0]
)";
}

TEST_CASE("engineThrustN without an idle deck is the linear throttle x mil line", "[aero][idle_thrust]") {
    auto d = makeData(); // no idle_thrust
    CHECK(!d.engine.idle_thrust.has_value());
    const float mil = d.engine.mil_thrust.lookup(0.f, 0.f); // 60 kN
    CHECK_THAT(engineThrustN(d.engine, 0.f, 0.f, false, 0.f), WithinAbs(0.f, 1e-3f));
    CHECK_THAT(engineThrustN(d.engine, 0.f, 0.f, false, 0.6f), WithinRel(0.6f * mil * 1000.f, 1e-5f));
    CHECK_THAT(engineThrustN(d.engine, 0.f, 0.f, false, 1.f), WithinRel(mil * 1000.f, 1e-5f));
}

TEST_CASE("engineThrustN blends the idle deck to MIL across throttle", "[aero][idle_thrust]") {
    auto d = parseFlightModel(withIdleThrust());
    REQUIRE(d.engine.idle_thrust.has_value());
    const float mil = d.engine.mil_thrust.lookup(0.f, 0.f);    // 60 kN at M0 SL
    const float idle = d.engine.idle_thrust->lookup(0.f, 0.f); // 2.8 kN at M0 SL
    REQUIRE(idle == Catch::Approx(2.8f));

    // Throttle 0 -> the published static idle, not zero.
    CHECK_THAT(engineThrustN(d.engine, 0.f, 0.f, false, 0.f), WithinRel(idle * 1000.f, 1e-5f));
    // Throttle 1 -> MIL, exactly as without the deck.
    CHECK_THAT(engineThrustN(d.engine, 0.f, 0.f, false, 1.f), WithinRel(mil * 1000.f, 1e-5f));
    // Half throttle -> the linear blend.
    CHECK_THAT(engineThrustN(d.engine, 0.f, 0.f, false, 0.5f), WithinRel((idle + 0.5f * (mil - idle)) * 1000.f, 1e-5f));
}

TEST_CASE("idle thrust goes negative in the ram-drag regime", "[aero][idle_thrust]") {
    auto d = parseFlightModel(withIdleThrust());
    // At M 0.9 sea level the deck is -16 kN: throttle 0 produces negative net thrust.
    CHECK(engineThrustN(d.engine, 0.9f, 0.f, false, 0.f) < 0.f);
    CHECK_THAT(engineThrustN(d.engine, 0.9f, 0.f, false, 0.f), WithinRel(-16.f * 1000.f, 1e-5f));
}

// ---------------------------------------------------------------------------
// Flight-model schema gaps from TP-1538 (#899): cm0, cross terms, speed-brake
// pitch/lift increments, and alpha-dependent dynamic dampers. All additive and
// optional — a model that sets none is byte-identical to before.
// ---------------------------------------------------------------------------

TEST_CASE("moments: cm0 adds a constant zero-alpha pitching moment", "[aero][gaps899]") {
    auto d = makeData();
    auto atmos = computeAtmosphere(0.f);
    const float spd = 0.3f * atmos.speed_of_sound_m_s;
    ControlInput ctrl{};
    // alpha=0, all rates 0, no elevator/speedbrake -> the only pitch term is cm0.
    auto base = computeMoments(0.f, 0.f, 0.f, 0.f, 0.f, spd, 0.f, 0.f, ctrl, d, atmos, ArticulationState{});
    d.moments.cm0 = 0.05f;
    auto with_cm0 = computeMoments(0.f, 0.f, 0.f, 0.f, 0.f, spd, 0.f, 0.f, ctrl, d, atmos, ArticulationState{});
    const float q = 0.5f * atmos.density_kg_m3 * spd * spd;
    const float expected = q * d.geometry.wing_area_m2 * d.geometry.mac_m * 0.05f;
    CHECK_THAT(with_cm0[1] - base[1], WithinRel(expected, 1e-4f));
}

TEST_CASE("moments: cn_da yaws against the commanded roll (adverse yaw)", "[aero][gaps899]") {
    auto d = makeData();
    auto atmos = computeAtmosphere(0.f);
    const float spd = 0.3f * atmos.speed_of_sound_m_s;
    ControlInput ctrl{};
    ctrl.aileron = 1.f; // roll right
    auto base = computeMoments(0.f, 0.f, 0.f, 0.f, 0.f, spd, 0.f, 0.f, ctrl, d, atmos, ArticulationState{});
    CHECK_THAT(base[2], WithinAbs(0.f, 1e-3f)); // no adverse yaw by default
    d.moments.cn_da = -0.02f;                   // negative = yaw opposes the roll
    auto with = computeMoments(0.f, 0.f, 0.f, 0.f, 0.f, spd, 0.f, 0.f, ctrl, d, atmos, ArticulationState{});
    CHECK(with[2] < base[2]); // right roll now yaws left
}

TEST_CASE("moments: cl_dr rolls the aircraft with rudder", "[aero][gaps899]") {
    auto d = makeData();
    auto atmos = computeAtmosphere(0.f);
    const float spd = 0.3f * atmos.speed_of_sound_m_s;
    ControlInput ctrl{};
    ctrl.rudder = 1.f;
    auto base = computeMoments(0.f, 0.f, 0.f, 0.f, 0.f, spd, 0.f, 0.f, ctrl, d, atmos, ArticulationState{});
    d.moments.cl_dr = 0.03f;
    auto with = computeMoments(0.f, 0.f, 0.f, 0.f, 0.f, spd, 0.f, 0.f, ctrl, d, atmos, ArticulationState{});
    CHECK(with[0] != Catch::Approx(base[0])); // rudder now induces a roll moment
}

TEST_CASE("moments: alpha-damper table replaces the scalar cm_q", "[aero][gaps899]") {
    auto d = makeData();
    auto atmos = computeAtmosphere(0.f);
    const float spd = 0.3f * atmos.speed_of_sound_m_s;
    ControlInput ctrl{};
    const float q_rate = 1.0f; // rad/s pitch rate exercises the damper
    const float alpha = 10.f * 3.14159265f / 180.f;
    auto scalar = computeMoments(alpha, 0.f, 0.f, q_rate, 0.f, spd, 0.f, 0.f, ctrl, d, atmos, ArticulationState{});
    // A constant table at half the scalar damping -> a distinctly different pitch moment.
    Table1D t;
    t.keys = {0.f, 20.f};
    t.values = {-5.f, -5.f}; // scalar cm_q is -10
    d.moments.cm_q_table = t;
    auto tabled = computeMoments(alpha, 0.f, 0.f, q_rate, 0.f, spd, 0.f, 0.f, ctrl, d, atmos, ArticulationState{});
    CHECK(scalar[1] != Catch::Approx(tabled[1]));
}

TEST_CASE("forces + moments: speed-brake lift and pitch increments", "[aero][gaps899]") {
    auto d = makeData();
    auto atmos = computeAtmosphere(0.f);
    const float spd = 0.3f * atmos.speed_of_sound_m_s;
    const float q = 0.5f * atmos.density_kg_m3 * spd * spd;
    ControlInput ctrl{};
    PayloadEffect payload{};
    ArticulationState brakeOut{};
    brakeOut.speedbrake = 1.f; // the brake's aero terms follow its POSITION (#842)

    auto f_base = computeForces(0.f, 0.f, 0.3f, spd, 0.f, 55.f, false, 0.f, payload, d, atmos, brakeOut);
    auto m_base = computeMoments(0.f, 0.f, 0.f, 0.f, 0.f, spd, 0.f, 0.f, ctrl, d, atmos, brakeOut);

    d.drag_polar.speedbrake_cl = -0.10f; // airbrake dumps some lift
    d.moments.cm_speedbrake = 0.04f;     // and pitches the nose

    auto f_with = computeForces(0.f, 0.f, 0.3f, spd, 0.f, 55.f, false, 0.f, payload, d, atmos, brakeOut);
    auto m_with = computeMoments(0.f, 0.f, 0.f, 0.f, 0.f, spd, 0.f, 0.f, ctrl, d, atmos, brakeOut);

    // Lift (body-y at alpha 0) drops by q*S*|dCZ|.
    CHECK_THAT(f_with[1] - f_base[1], WithinRel(q * d.geometry.wing_area_m2 * -0.10f, 1e-4f));
    // Pitch moment gains q*S*mac*dCm.
    CHECK_THAT(m_with[1] - m_base[1], WithinRel(q * d.geometry.wing_area_m2 * d.geometry.mac_m * 0.04f, 1e-4f));
}

// ---------------------------------------------------------------------------
// Asymmetric control travel (#822)
//
// [aero.controls] gave one scalar per axis, and real stabilators are not symmetric: the F-5E's
// travels 17 deg nose-up but only 5 deg nose-down (T.O. 1F-5E-1). With one number, an author must
// pick -- and picking the nose-up figure (which governs pitch authority and reaching the G limit)
// models the aircraft's nose-down authority 3.4x too generous. A bunt would be far more effective
// than the real aeroplane's.
// ---------------------------------------------------------------------------

// kGenericToml with the F-5E's asymmetric stabilator travel.
static std::string withAsymmetricElevator() {
    std::string s = kGenericToml;
    auto pos = s.find("max_elevator_deg = 25.0");
    REQUIRE(pos != std::string::npos);
    s.replace(pos, std::string("max_elevator_deg = 25.0").size(),
              "max_elevator_deg     = 17.0\nmax_elevator_neg_deg =  5.0");
    return s;
}

TEST_CASE("controls: max_elevator_neg_deg defaults to the positive travel", "[aero][controls]") {
    // A model that says nothing gets symmetric travel, exactly as before.
    auto d = makeData();
    CHECK(d.controls.max_elevator_neg_deg == d.controls.max_elevator_deg);
}

TEST_CASE("controls: a symmetric model produces bit-identical pitching moments", "[aero][controls]") {
    // The regression guard. Every existing aircraft, and all community content, uses the symmetric
    // spelling and must be completely unaffected.
    auto d = makeData();
    auto atmos = computeAtmosphere(3000.f);

    ControlInput up{};
    up.elevator = 1.f;
    ControlInput down{};
    down.elevator = -1.f;

    const auto mUp = computeMoments(0.05f, 0.f, 0.f, 0.f, 0.f, 200.f, 0.f, 0.f, up, d, atmos, ArticulationState{});
    const auto mDown = computeMoments(0.05f, 0.f, 0.f, 0.f, 0.f, 200.f, 0.f, 0.f, down, d, atmos, ArticulationState{});

    // Same magnitude of elevator contribution either way: the only difference between the two is the
    // sign of the command, so the pitching moments straddle the stick-free value symmetrically.
    const auto mNeutral =
        computeMoments(0.05f, 0.f, 0.f, 0.f, 0.f, 200.f, 0.f, 0.f, ControlInput{}, d, atmos, ArticulationState{});
    // Relative, not absolute: these moments are ~1e6 N·m, where a float ULP is larger than 1e-3.
    CHECK_THAT(mUp[1] - mNeutral[1], WithinRel(-(mDown[1] - mNeutral[1]), 1e-5f));
}

TEST_CASE("controls: full nose-down gives 5/17 of the nose-up pitching moment", "[aero][controls]") {
    // THE ACCEPTANCE CRITERION. The F-5E's stabilator: 17 deg up, 5 deg down.
    auto d = parseFlightModel(withAsymmetricElevator());
    REQUIRE(d.controls.max_elevator_deg == Catch::Approx(17.f));
    REQUIRE(d.controls.max_elevator_neg_deg == Catch::Approx(5.f));

    auto atmos = computeAtmosphere(3000.f);

    ControlInput up{};
    up.elevator = 1.f;
    ControlInput down{};
    down.elevator = -1.f;

    const auto mNeutral =
        computeMoments(0.05f, 0.f, 0.f, 0.f, 0.f, 200.f, 0.f, 0.f, ControlInput{}, d, atmos, ArticulationState{});
    const auto mUp = computeMoments(0.05f, 0.f, 0.f, 0.f, 0.f, 200.f, 0.f, 0.f, up, d, atmos, ArticulationState{});
    const auto mDown = computeMoments(0.05f, 0.f, 0.f, 0.f, 0.f, 200.f, 0.f, 0.f, down, d, atmos, ArticulationState{});

    // Isolate the elevator's contribution by subtracting the stick-free moment.
    const float upContribution = mUp[1] - mNeutral[1];
    const float downContribution = mDown[1] - mNeutral[1];

    // They still act in opposite directions...
    CHECK(upContribution * downContribution < 0.f);
    // ...but the nose-down authority is 5/17 of the nose-up authority, not equal to it.
    CHECK_THAT(std::abs(downContribution), WithinRel(std::abs(upContribution) * (5.f / 17.f), 1e-3f));
}

TEST_CASE("controls: partial nose-down scales against the negative travel", "[aero][controls]") {
    // Half forward stick is half of the NOSE-DOWN travel, not half of the nose-up travel.
    auto d = parseFlightModel(withAsymmetricElevator());
    auto atmos = computeAtmosphere(3000.f);

    ControlInput half{};
    half.elevator = -0.5f;
    ControlInput full{};
    full.elevator = -1.f;

    const auto mNeutral =
        computeMoments(0.05f, 0.f, 0.f, 0.f, 0.f, 200.f, 0.f, 0.f, ControlInput{}, d, atmos, ArticulationState{});
    const auto mHalf = computeMoments(0.05f, 0.f, 0.f, 0.f, 0.f, 200.f, 0.f, 0.f, half, d, atmos, ArticulationState{});
    const auto mFull = computeMoments(0.05f, 0.f, 0.f, 0.f, 0.f, 200.f, 0.f, 0.f, full, d, atmos, ArticulationState{});

    CHECK_THAT(mHalf[1] - mNeutral[1], WithinRel((mFull[1] - mNeutral[1]) * 0.5f, 1e-3f));
}
