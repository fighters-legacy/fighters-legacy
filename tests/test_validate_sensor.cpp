// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "sensor_validator.h"

#include <algorithm>
#include <string>

using namespace fl;

namespace {

const char* kValidRadar = R"toml(
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

// True when any warning mentions the given field/phrase.
bool warns(const SensorValidationResult& r, std::string_view needle) {
    return std::any_of(r.warnings.begin(), r.warnings.end(),
                       [&](const std::string& w) { return w.find(needle) != std::string::npos; });
}

} // namespace

TEST_CASE("validate-sensor passes a well-formed radar with no warnings") {
    const auto r = validateSensor(kValidRadar);
    CHECK(r.ok);
    CHECK(r.errors.empty());
    CHECK(r.warnings.empty());
}

TEST_CASE("validate-sensor reports the parser's error rather than inventing its own") {
    const auto r = validateSensor(R"toml(
[sensor]
id   = "t:x"
name = "X"
type = "radar"

[search]
az_half_angle_deg = 60.0
el_half_angle_deg = 30.0
max_range_nm      = 40.0
pod               = 2.0
)toml");

    REQUIRE_FALSE(r.ok);
    REQUIRE(r.errors.size() == 1);
    CHECK(r.errors[0].find("pod") != std::string::npos);
}

TEST_CASE("validate-sensor warns about a passive sensor that emits") {
    const auto r = validateSensor(R"toml(
[sensor]
id      = "t:irst"
name    = "IRST"
type    = "ir"
emitter = true

[search]
az_half_angle_deg = 60.0
el_half_angle_deg = 60.0
max_range_nm      = 20.0
pod               = 0.3
)toml");

    CHECK(r.ok); // legal — a warning, not an error
    CHECK(warns(r, "passive"));
}

TEST_CASE("validate-sensor warns about a radar track lobe that can never be used") {
    // emitter = false + a radar [track] lobe: the emissions kernel requires the observer to be
    // radiating, so this sensor carries a lobe it can never use.
    const auto r = validateSensor(R"toml(
[sensor]
id      = "t:x"
name    = "X"
type    = "radar"
emitter = false

[search]
az_half_angle_deg = 60.0
el_half_angle_deg = 30.0
max_range_nm      = 40.0
pod               = 0.35

[track]
az_half_angle_deg = 30.0
el_half_angle_deg = 20.0
max_range_nm      = 30.0
pod               = 0.65
lock_hold_s       = 4.0
)toml");

    CHECK(r.ok);
    CHECK(warns(r, "never hold a lock"));
}

TEST_CASE("validate-sensor warns when the track lobe outreaches the search lobe that feeds it") {
    const auto r = validateSensor(R"toml(
[sensor]
id      = "t:x"
name    = "X"
type    = "radar"
emitter = true

[search]
az_half_angle_deg = 30.0
el_half_angle_deg = 20.0
max_range_nm      = 20.0
pod               = 0.65

[track]
az_half_angle_deg = 60.0
el_half_angle_deg = 30.0
max_range_nm      = 40.0
pod               = 0.35
lock_hold_s       = 4.0
)toml");

    CHECK(r.ok);
    CHECK(warns(r, "cannot be tracked before it has been found"));
    CHECK(warns(r, "wider than the [search] lobe"));
    CHECK(warns(r, "track.pod is below search.pod"));
}

TEST_CASE("validate-sensor warns about a unit mix-up in the range fields") {
    // 74000 authored into a field that wants nautical miles — the classic metres-for-miles slip.
    const auto r = validateSensor(R"toml(
[sensor]
id   = "t:x"
name = "X"
type = "visual"

[search]
az_half_angle_deg = 90.0
el_half_angle_deg = 60.0
max_range_nm      = 74000.0
pod               = 0.15
)toml");

    CHECK(r.ok);
    CHECK(warns(r, "check the units"));
    CHECK(warns(r, "visual sensor"));
}

TEST_CASE("validate-sensor warns about a track that never coasts") {
    const auto r = validateSensor(R"toml(
[sensor]
id      = "t:x"
name    = "X"
type    = "radar"
emitter = true

[search]
az_half_angle_deg = 60.0
el_half_angle_deg = 30.0
max_range_nm      = 40.0
pod               = 0.35

[track]
az_half_angle_deg = 30.0
el_half_angle_deg = 20.0
max_range_nm      = 30.0
pod               = 0.65
)toml");

    CHECK(r.ok);
    CHECK(warns(r, "lock_hold_s"));
}
