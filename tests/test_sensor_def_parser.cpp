// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "sensor/BuiltinSensors.h"
#include "sensor/SensorDefParser.h"

#include <stdexcept>
#include <string>

using namespace fl::sensor;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

// The APG-63 example from docs/modding/formats.md, verbatim.
const std::string kRadar = R"toml(
[sensor]
id              = "fl-base:apg63"
name            = "APG-63 pulse-doppler radar"
type            = "radar"
omnidirectional = false
emitter         = true

[search]
az_half_angle_deg = 60.0
el_half_angle_deg = 30.0
min_range_nm      = 0.08
max_range_nm      = 40.0
pod               = 0.35

[track]
az_half_angle_deg = 30.0
el_half_angle_deg = 20.0
min_range_nm      = 0.08
max_range_nm      = 30.0
pod               = 0.65
lock_hold_s       = 4.0
)toml";

// Search-only, no [track] — the shape every passive sensor takes.
const std::string kEyeball = R"toml(
[sensor]
id   = "fl-base:eyeball"
name = "Mark One Eyeball"
type = "visual"

[search]
az_half_angle_deg = 90.0
el_half_angle_deg = 60.0
max_range_nm      = 8.0
pod               = 0.15
)toml";

constexpr float kM_PER_NM = 1852.f;

} // namespace

TEST_CASE("parseSensorDef reads every field of a dual-lobe radar") {
    const SensorDef s = parseSensorDef(kRadar);

    CHECK(s.id == "fl-base:apg63");
    CHECK(s.name == "APG-63 pulse-doppler radar");
    CHECK(s.type == SensorType::Radar);
    CHECK_FALSE(s.omnidirectional);
    CHECK(s.emitter);

    CHECK_THAT(s.search.azHalfAngleDeg, WithinAbs(60.0, 1e-4));
    CHECK_THAT(s.search.elHalfAngleDeg, WithinAbs(30.0, 1e-4));
    CHECK_THAT(s.search.pod, WithinAbs(0.35, 1e-6));

    REQUIRE(s.track.has_value());
    CHECK_THAT(s.track->azHalfAngleDeg, WithinAbs(30.0, 1e-4));
    CHECK_THAT(s.track->pod, WithinAbs(0.65, 1e-6));
    CHECK_THAT(s.lockHoldS, WithinAbs(4.0, 1e-4));
}

TEST_CASE("parseSensorDef converts authored nautical miles to SI metres") {
    const SensorDef s = parseSensorDef(kRadar);

    CHECK_THAT(s.search.maxRangeM, WithinRel(40.f * kM_PER_NM, 1e-5f));
    CHECK_THAT(s.search.minRangeM, WithinRel(0.08f * kM_PER_NM, 1e-4f));
    CHECK_THAT(s.track->maxRangeM, WithinRel(30.f * kM_PER_NM, 1e-5f));
}

TEST_CASE("parseSensorDef defaults the optional fields") {
    const SensorDef s = parseSensorDef(kEyeball);

    CHECK_FALSE(s.omnidirectional);                       // default false
    CHECK_FALSE(s.emitter);                               // default false
    CHECK_THAT(s.search.minRangeM, WithinAbs(0.0, 1e-6)); // no dead zone
    CHECK_FALSE(s.track.has_value());                     // absent [track] = search-only
    CHECK_THAT(s.lockHoldS, WithinAbs(0.0, 1e-6));
}

TEST_CASE("parseSensorDef accepts both spellings of the infrared type") {
    const std::string base = R"toml(
[sensor]
id   = "t:irst"
name = "IRST"
type = ")toml";
    const std::string tail = R"toml("

[search]
az_half_angle_deg = 60.0
el_half_angle_deg = 60.0
max_range_nm      = 20.0
pod               = 0.3
)toml";

    CHECK(parseSensorDef(base + "ir" + tail).type == SensorType::Ir);
    CHECK(parseSensorDef(base + "infrared" + tail).type == SensorType::Ir);
    CHECK(parseSensorDef(base + "laser" + tail).type == SensorType::Laser);
    CHECK_THROWS_AS(parseSensorDef(base + "sonar" + tail), std::runtime_error);
}

TEST_CASE("parseSensorDef lets an omnidirectional sensor omit its half-angles") {
    // An RWR has no cone to point. The lobe still describes a full sphere, so the detection math
    // needs no special case for the flag.
    const SensorDef s = parseSensorDef(R"toml(
[sensor]
id              = "t:rwr"
name            = "RWR"
type            = "radar"
omnidirectional = true

[search]
max_range_nm = 100.0
pod          = 0.9
)toml");

    CHECK(s.omnidirectional);
    CHECK_THAT(s.search.azHalfAngleDeg, WithinAbs(180.0, 1e-4));
    CHECK_THAT(s.search.elHalfAngleDeg, WithinAbs(90.0, 1e-4));
}

TEST_CASE("parseSensorDef requires half-angles on a directional sensor") {
    CHECK_THROWS_AS(parseSensorDef(R"toml(
[sensor]
id   = "t:x"
name = "X"
type = "radar"

[search]
max_range_nm = 40.0
pod          = 0.35
)toml"),
                    std::runtime_error);
}

TEST_CASE("parseSensorDef rejects a malformed sensor rather than defaulting it") {
    auto sensorWith = [](const char* searchBody) {
        return std::string(R"toml(
[sensor]
id   = "t:x"
name = "X"
type = "radar"

[search]
)toml") + searchBody;
    };

    // A missing [sensor] table, and a missing required identity field.
    CHECK_THROWS_AS(parseSensorDef("[search]\nmax_range_nm = 1.0\npod = 0.5\n"), std::runtime_error);
    CHECK_THROWS_AS(parseSensorDef("[sensor]\nid = \"t:x\"\ntype = \"radar\"\n"), std::runtime_error);

    // A missing [search] table: a sensor that cannot find anything is not a sensor.
    CHECK_THROWS_AS(parseSensorDef("[sensor]\nid = \"t:x\"\nname = \"X\"\ntype = \"radar\"\n"), std::runtime_error);

    // Range violations, one per rule.
    CHECK_THROWS_AS(parseSensorDef(sensorWith("az_half_angle_deg = 0.0\nel_half_angle_deg = 30.0\n"
                                              "max_range_nm = 40.0\npod = 0.35\n")),
                    std::runtime_error); // half-angle must be > 0
    CHECK_THROWS_AS(parseSensorDef(sensorWith("az_half_angle_deg = 181.0\nel_half_angle_deg = 30.0\n"
                                              "max_range_nm = 40.0\npod = 0.35\n")),
                    std::runtime_error); // half-angle must be <= 180
    CHECK_THROWS_AS(parseSensorDef(sensorWith("az_half_angle_deg = 60.0\nel_half_angle_deg = 30.0\n"
                                              "min_range_nm = 50.0\nmax_range_nm = 40.0\npod = 0.35\n")),
                    std::runtime_error); // max must exceed min
    CHECK_THROWS_AS(parseSensorDef(sensorWith("az_half_angle_deg = 60.0\nel_half_angle_deg = 30.0\n"
                                              "min_range_nm = -1.0\nmax_range_nm = 40.0\npod = 0.35\n")),
                    std::runtime_error); // min must be >= 0
    CHECK_THROWS_AS(parseSensorDef(sensorWith("az_half_angle_deg = 60.0\nel_half_angle_deg = 30.0\n"
                                              "max_range_nm = 40.0\npod = 0.0\n")),
                    std::runtime_error); // pod must be > 0
    CHECK_THROWS_AS(parseSensorDef(sensorWith("az_half_angle_deg = 60.0\nel_half_angle_deg = 30.0\n"
                                              "max_range_nm = 40.0\npod = 1.5\n")),
                    std::runtime_error); // pod must be <= 1

    // Broken TOML.
    CHECK_THROWS_AS(parseSensorDef("[sensor\nid = "), std::runtime_error);
}

TEST_CASE("parseSensorDef range-checks the track lobe and its lock hold") {
    auto radarWithTrack = [](const char* trackBody) {
        return std::string(R"toml(
[sensor]
id   = "t:x"
name = "X"
type = "radar"

[search]
az_half_angle_deg = 60.0
el_half_angle_deg = 30.0
max_range_nm      = 40.0
pod               = 0.35

[track]
)toml") + trackBody;
    };

    CHECK_THROWS_AS(parseSensorDef(radarWithTrack("az_half_angle_deg = 30.0\nel_half_angle_deg = 20.0\n"
                                                  "max_range_nm = 30.0\npod = 0.65\nlock_hold_s = 61.0\n")),
                    std::runtime_error); // lock_hold_s <= 60
    CHECK_THROWS_AS(parseSensorDef(radarWithTrack("az_half_angle_deg = 30.0\nel_half_angle_deg = 20.0\n"
                                                  "max_range_nm = 30.0\npod = 0.65\nlock_hold_s = -1.0\n")),
                    std::runtime_error); // lock_hold_s >= 0
    CHECK_THROWS_AS(parseSensorDef(radarWithTrack("az_half_angle_deg = 30.0\nel_half_angle_deg = 20.0\n"
                                                  "pod = 0.65\n")),
                    std::runtime_error); // the track lobe needs its own max_range_nm

    // lock_hold_s is optional: a track lobe without one simply does not coast.
    const SensorDef s = parseSensorDef(radarWithTrack("az_half_angle_deg = 30.0\nel_half_angle_deg = 20.0\n"
                                                      "max_range_nm = 30.0\npod = 0.65\n"));
    REQUIRE(s.track.has_value());
    CHECK_THAT(s.lockHoldS, WithinAbs(0.0, 1e-6));
}

TEST_CASE("the builtin eyeball exists with no content pack and is search-only") {
    // Honest sensing is the DEFAULT, not an opt-in: an AI entity with no declared sensors gets this
    // one, in every configuration of the engine including the zero-content sandbox.
    const SensorDef& s = BuiltinSensors::eyeball();

    CHECK(s.id == "builtin:eyeball");
    CHECK(s.type == SensorType::Visual);
    CHECK_FALSE(s.emitter); // an eyeball does not radiate
    CHECK_FALSE(s.track);   // and it does not hold a lock
    CHECK(s.search.maxRangeM > 0.f);
    CHECK(s.search.pod > 0.f);
    CHECK(s.search.pod <= 1.f);

    // Same object every call — it is a compiled-in singleton, like BuiltinFlightModel.
    CHECK(&BuiltinSensors::eyeball() == &s);
}

TEST_CASE("the builtin seeker heads exist and follow the sensor vocabulary", "[sensor][builtin]") {
    const SensorDef& ir = fl::sensor::BuiltinSensors::irSeeker();
    CHECK(ir.id == "builtin:ir-seeker");
    CHECK(ir.type == SensorType::Ir);
    CHECK_FALSE(ir.emitter); // an IR seeker announces nothing
    REQUIRE(ir.track.has_value());
    CHECK(ir.track->azHalfAngleDeg >= ir.search.azHalfAngleDeg); // the gimbal holds wider than it acquires
    CHECK(ir.track->pod > ir.search.pod);
    CHECK(ir.lockHoldS > 0.f);

    const SensorDef& rdr = fl::sensor::BuiltinSensors::radarSeeker();
    CHECK(rdr.id == "builtin:radar-seeker");
    CHECK(rdr.type == SensorType::Radar);
    CHECK(rdr.emitter); // pitbull = the missile starts announcing itself (the #529 RWR seam)
    REQUIRE(rdr.track.has_value());
    CHECK(rdr.lockHoldS > 0.f);

    // Same object every call (BuiltinFlightModel pattern).
    CHECK(&fl::sensor::BuiltinSensors::irSeeker() == &ir);
}
