// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "match/BuiltinGameModes.h"
#include "match/GameModeParser.h"

using namespace fl;

TEST_CASE("GameModeParser: full schema round-trips", "[game_mode]") {
    const char* toml = R"([mode]
id = "pack:tdm-custom"
name = "Custom TDM"
[teams]
use_mission_sides = false
[[teams.team]]
id = "red"
name = "Red"
capacity = 16
[[teams.team]]
id = "blue"
name = "Blue"
capacity = 16
[scoring]
points_per_kill = 2
points_per_assist = 1
score_limit = 40
[match]
time_limit_min = 10
warmup_s = 20
min_players = 4
[respawn]
delay_s = 8
waves = true
wave_interval_s = 12
[rules]
friendly_fire = "off"
)";
    GameModeParseResult r = parseGameModeToml(toml);
    REQUIRE(r.ok);
    CHECK(r.mode.id == "pack:tdm-custom");
    CHECK(r.mode.name == "Custom TDM");
    CHECK_FALSE(r.mode.useMissionSides);
    REQUIRE(r.mode.teams.size() == 2u);
    CHECK(r.mode.teams[0].id == "red");
    CHECK(r.mode.teams[1].capacity == 16);
    CHECK(r.mode.pointsPerKill == 2);
    CHECK(r.mode.pointsPerAssist == 1);
    CHECK(r.mode.scoreLimit == 40);
    CHECK(r.mode.timeLimitS == Catch::Approx(600.0)); // 10 min -> seconds
    CHECK(r.mode.warmupS == Catch::Approx(20.0));
    CHECK(r.mode.minPlayers == 4);
    CHECK(r.mode.respawnDelayS == Catch::Approx(8.0));
    CHECK(r.mode.respawnWaves);
    CHECK(r.mode.friendlyFire == ModeFriendlyFire::Off);
}

TEST_CASE("GameModeParser: missing id is an error", "[game_mode]") {
    GameModeParseResult r = parseGameModeToml("[match]\nwarmup_s = 5\n");
    CHECK_FALSE(r.ok);
    CHECK_FALSE(r.error.empty());
}

TEST_CASE("GameModeParser: defaults when sections omitted", "[game_mode]") {
    GameModeParseResult r = parseGameModeToml("[mode]\nid = \"pack:x\"\n");
    REQUIRE(r.ok);
    CHECK(r.mode.useMissionSides); // default
    CHECK(r.mode.pointsPerKill == 1);
    CHECK(r.mode.scoreLimit == 0);
    CHECK(r.mode.timeLimitS == Catch::Approx(0.0));
    CHECK(r.mode.warmupS == Catch::Approx(0.0));
    CHECK(r.mode.minPlayers == 1);
    CHECK(r.mode.friendlyFire == ModeFriendlyFire::Server);
}

TEST_CASE("GameModeParser: friendly_fire tri-state", "[game_mode]") {
    CHECK(parseGameModeToml("[mode]\nid=\"x\"\n[rules]\nfriendly_fire=\"on\"\n").mode.friendlyFire ==
          ModeFriendlyFire::On);
    CHECK(parseGameModeToml("[mode]\nid=\"x\"\n[rules]\nfriendly_fire=\"OFF\"\n").mode.friendlyFire ==
          ModeFriendlyFire::Off);
    CHECK(parseGameModeToml("[mode]\nid=\"x\"\n[rules]\nfriendly_fire=\"server\"\n").mode.friendlyFire ==
          ModeFriendlyFire::Server);
    // Unknown value warns, keeps the server default.
    GameModeParseResult r = parseGameModeToml("[mode]\nid=\"x\"\n[rules]\nfriendly_fire=\"maybe\"\n");
    CHECK(r.mode.friendlyFire == ModeFriendlyFire::Server);
    CHECK_FALSE(r.warnings.empty());
}

TEST_CASE("GameModeParser: out-of-range values are clamped with a warning", "[game_mode]") {
    GameModeParseResult r = parseGameModeToml("[mode]\nid=\"x\"\n[match]\nmin_players = -5\n");
    REQUIRE(r.ok);
    CHECK(r.mode.minPlayers == 1); // clamped up
    CHECK_FALSE(r.warnings.empty());
}

TEST_CASE("GameModeParser: use_mission_sides with an explicit team list warns", "[game_mode]") {
    const char* toml = R"([mode]
id = "x"
[teams]
use_mission_sides = true
[[teams.team]]
id = "red"
)";
    GameModeParseResult r = parseGameModeToml(toml);
    REQUIRE(r.ok);
    CHECK(r.mode.useMissionSides);
    CHECK_FALSE(r.warnings.empty()); // contradiction flagged
}

TEST_CASE("BuiltinGameModes: free-flight and tdm resolve", "[game_mode]") {
    auto ff = builtinGameMode("builtin:free-flight");
    REQUIRE(ff.has_value());
    CHECK(ff->useMissionSides);
    CHECK(ff->scoreLimit == 0);
    CHECK(ff->warmupS == Catch::Approx(0.0));
    CHECK(ff->respawnDelayS == Catch::Approx(0.0));

    auto tdm = builtinGameMode("builtin:tdm");
    REQUIRE(tdm.has_value());
    CHECK_FALSE(tdm->useMissionSides);
    CHECK(tdm->teams.size() == 2u);
    CHECK(tdm->scoreLimit == 50);
    CHECK(tdm->timeLimitS == Catch::Approx(900.0));
    CHECK(tdm->friendlyFire == ModeFriendlyFire::Off);
    CHECK(tdm->minPlayers == 2);

    CHECK_FALSE(builtinGameMode("builtin:nope").has_value());
    CHECK(defaultGameMode().id == "builtin:free-flight");
}
