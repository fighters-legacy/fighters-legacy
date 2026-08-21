// SPDX-License-Identifier: GPL-3.0-or-later
//
// MissionReport::toJson (#1234): the mission name is pack-authored content, so the report must stay
// valid JSON whatever the mission is named — a quote in the YAML `name:` used to emit a document
// that killed the CI harness's json.load. Round-trips through the same util/Json.h reader family
// the server's own consumers use.

#include <mission/MissionReport.h>
#include <util/Json.h>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace fl;

TEST_CASE("MissionReport toJson survives a quoted mission name and round-trips it") {
    MissionReport r;
    r.missionName = "Operation \"Hammer\" \\ phase 1";
    r.outcome = "success";
    r.elapsedSeconds = 12.5;
    r.ticks = 750;

    const std::string out = toJson(r);

    CHECK(json::isObject(out));
    auto name = json::stringField(out, "mission_name");
    REQUIRE(name.has_value());
    CHECK(*name == r.missionName);
    auto outcome = json::stringField(out, "outcome");
    REQUIRE(outcome.has_value());
    CHECK(*outcome == "success");
}

TEST_CASE("MissionReport toJson keeps its deterministic %.6g number format") {
    MissionReport r;
    r.missionName = "plain";
    r.outcome = "timeout";
    r.elapsedSeconds = 1.0 / 3.0;

    const std::string out = toJson(r);
    CHECK(out.find("\"elapsed_seconds\": 0.333333") != std::string::npos);
}
