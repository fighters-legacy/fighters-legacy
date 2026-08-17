// SPDX-License-Identifier: GPL-3.0-or-later
//
// The FlightState -> ArtChannel mapping (#1195): the one place the simulation's actuator and
// wing-sweep state becomes the normalized channel values the wire codec and the renderer share.
//
// This test exists because the mapping used to be written out by hand TWICE — once in
// SnapshotPipeline for the wire, once in ClientPrediction for the own aircraft — and a channel that
// landed after both copies were written (`sweep`) was simply never added to either. The B-1B's wing
// sweep clip therefore never played for anybody, including the player flying it, while every
// validator passed and the flight model swept correctly the whole time.

#include "flight/ArticulationChannels.h"
#include "flight/FlightModelParser.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace fl;

namespace {

// The minimum a flight model needs to parse, plus an optional [wing_sweep] block. Numbers are a
// light fighter's; nothing here depends on them beyond the sweep limits.
std::string modelToml(bool withSweep) {
    std::string s = R"(
[aircraft]
name         = "Test"
type         = "fighter"
engine_type  = "turbojet"
has_fbw      = false
cruise_alt_m = 9000.0

[flight_model]
mass_kg      = 4300.0
wing_area_m2 = 17.3
wingspan_m   = 8.13
mac_m        = 2.4
fuel_kg      = 1800.0
ixx_kg_m2    = 3000.0
iyy_kg_m2    = 40000.0
izz_kg_m2    = 42000.0

[aero.cl_table]
alpha  = [-4.0, 0.0, 8.0, 16.0]
mach   = [0.30, 0.90]
values = [
    -0.30, -0.30,
     0.10,  0.10,
     0.90,  0.90,
     1.40,  1.40,
]

[aero.drag_polar]
cd0           = 0.020
k             = 0.130
speedbrake_cd = 0.040
gear_cd       = 0.025

[aero.moments]
cm_alpha = -0.60
cm_q     = -9.00
cm_de    = -1.10
cl_beta  = -0.10
cl_p     = -0.40
cl_da    =  0.15
cn_beta  =  0.12
cn_r     = -0.15
cn_dr    = -0.08

[aero.limits]
alpha_stall_deg  = 16.0
max_g_structural =  7.33
min_g_structural = -3.00
max_mach         =  1.60

[aero.controls]
max_elevator_deg = 25.0
max_aileron_deg  = 20.0
max_rudder_deg   = 25.0

[engine]
fuel_flow_idle_kg_s = 0.10
fuel_flow_mil_kg_s  = 0.60
fuel_flow_ab_kg_s   = 1.80
spool_time_s        = 1.50

[engine.mil_thrust]
mach   = [0.00, 0.90]
alt_km = [0.00, 9.00]
values = [
    15.0,  8.0,
    17.0,  9.0,
]
)";
    if (withSweep) {
        // The B-1B's limits, because they are the ones that found the defect and they are usefully
        // asymmetric: min_deg is 15, not 0, so a mapping that forgot to subtract it still "works"
        // at max_deg and is wrong everywhere else.
        s += R"(
[wing_sweep]
ref_sweep_deg   = 15.0
min_deg         = 15.0
max_deg         = 67.5
slew_rate_deg_s = 5.0

[wing_sweep.schedule]
mach  = [0.00, 0.70, 0.85, 0.95, 1.05, 1.25]
sweep = [15.0, 15.0, 25.0, 55.0, 67.5, 67.5]

[wing_sweep.spread]
cl_scale  = 1.000
k_scale   = 1.000
cd0_delta = +0.0000

[wing_sweep.swept]
cl_scale  = 0.666
k_scale   = 1.715
cd0_delta = -0.0020
)";
    }
    return s;
}

constexpr std::size_t kSweep = static_cast<std::size_t>(ArtChannel::Sweep);

} // namespace

TEST_CASE("sweepChannelValue maps the sweep range onto the clip parameter (#1195)", "[articulation][sweep]") {
    const FlightModelData vg = parseFlightModel(modelToml(true));
    REQUIRE(vg.wing_sweep.has_value());

    FlightState s;

    s.current_sweep_deg = vg.wing_sweep->min_deg;
    CHECK(sweepChannelValue(s, vg) == Catch::Approx(0.f));

    s.current_sweep_deg = vg.wing_sweep->max_deg;
    CHECK(sweepChannelValue(s, vg) == Catch::Approx(1.f));

    // Mid-travel is the case a mapping that ignores min_deg gets wrong: 41.25 deg is halfway from 15
    // to 67.5, but only a quarter of the way from 0.
    s.current_sweep_deg = 41.25f;
    CHECK(sweepChannelValue(s, vg) == Catch::Approx(0.5f));

    // The schedule's own cruise detent, as a cross-check against a real authored number.
    s.current_sweep_deg = 25.f;
    CHECK(sweepChannelValue(s, vg) == Catch::Approx(10.f / 52.5f));

    // Clamped both ways. The integrator clamps too, but the clip parameter must be in range for a
    // caller that built a FlightState by hand -- scrubbing past the end of a clip is undefined.
    s.current_sweep_deg = -90.f;
    CHECK(sweepChannelValue(s, vg) == Catch::Approx(0.f));
    s.current_sweep_deg = 400.f;
    CHECK(sweepChannelValue(s, vg) == Catch::Approx(1.f));
}

TEST_CASE("a fixed-geometry aircraft reports neutral sweep and emits nothing (#1195)", "[articulation][sweep]") {
    // THE COMPATIBILITY CLAIM. Everything without a [wing_sweep] table must land exactly on the
    // channel's neutral, because that is what makes it absent from the snapshot TLV: if this were
    // any other value, every aircraft in the game would start paying a byte per snapshot for a
    // channel its mesh does not even model.
    const FlightModelData fixed = parseFlightModel(modelToml(false));
    REQUIRE_FALSE(fixed.wing_sweep.has_value());

    FlightState s;
    CHECK(sweepChannelValue(s, fixed) == artChannelNeutral(ArtChannel::Sweep));

    // Even carrying FlightState's bare default (55 deg), which is a number no fixed-geometry
    // aircraft has any business reporting.
    s.current_sweep_deg = 55.f;
    CHECK(sweepChannelValue(s, fixed) == artChannelNeutral(ArtChannel::Sweep));

    float ch[kArtChannelCount];
    fillArtChannels(s, fixed, ch);
    CHECK(ch[kSweep] == artChannelNeutral(ArtChannel::Sweep));
}

TEST_CASE("fillArtChannels carries the actuators and the wing together (#1195)", "[articulation]") {
    const FlightModelData vg = parseFlightModel(modelToml(true));

    FlightState s;
    s.articulation.gear = 1.f;
    s.articulation.flaps = 0.5f;
    s.articulation.speedbrake = 0.25f;
    s.articulation.hook = 1.f;
    s.articulation.canopy = 0.75f;
    s.current_sweep_deg = 67.5f;

    float ch[kArtChannelCount];
    fillArtChannels(s, vg, ch);

    CHECK(ch[static_cast<std::size_t>(ArtChannel::Gear)] == Catch::Approx(1.f));
    CHECK(ch[static_cast<std::size_t>(ArtChannel::Flaps)] == Catch::Approx(0.5f));
    CHECK(ch[static_cast<std::size_t>(ArtChannel::Speedbrake)] == Catch::Approx(0.25f));
    CHECK(ch[static_cast<std::size_t>(ArtChannel::Hook)] == Catch::Approx(1.f));
    CHECK(ch[static_cast<std::size_t>(ArtChannel::Canopy)] == Catch::Approx(0.75f));
    CHECK(ch[kSweep] == Catch::Approx(1.f));

    // Every channel with no writer yet stays at its neutral, so it is absent from the wire and the
    // mesh renders it exactly as it did before this existed. When one of them acquires a writer,
    // THIS is the list that has to change -- which is the whole point of there being only one.
    for (std::size_t i = 0; i < kArtChannelCount; ++i) {
        const auto c = static_cast<ArtChannel>(i);
        if (c == ArtChannel::Gear || c == ArtChannel::Flaps || c == ArtChannel::Speedbrake || c == ArtChannel::Hook ||
            c == ArtChannel::Canopy || c == ArtChannel::Sweep)
            continue;
        INFO("channel " << artChannelName(c));
        CHECK(ch[i] == artChannelNeutral(c));
    }
}

TEST_CASE("fillArtChannels overwrites every slot it is handed (#1195)", "[articulation]") {
    // The caller passes a raw array. A fill that only wrote the channels it knows about would leave
    // an unrelated caller's stale values in the rest, and those would be sent as if the simulation
    // had commanded them.
    const FlightModelData fixed = parseFlightModel(modelToml(false));
    float ch[kArtChannelCount];
    for (std::size_t i = 0; i < kArtChannelCount; ++i)
        ch[i] = 0.42f;

    fillArtChannels(FlightState{}, fixed, ch);
    for (std::size_t i = 0; i < kArtChannelCount; ++i) {
        INFO("channel " << artChannelName(static_cast<ArtChannel>(i)));
        CHECK(ch[i] == artChannelNeutral(static_cast<ArtChannel>(i)));
    }
}
