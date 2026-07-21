// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "match/GameModeDef.h"

#include <string>
#include <string_view>
#include <vector>

namespace fl {

// Result of parsing a game-mode TOML document (#521). Mirrors MissionParseResult: `ok` is false only on
// a structural/semantic error (the `error` string says why); range problems and unknown keys are
// non-fatal `warnings` with the field clamped/ignored — the same tolerant contract the mission and
// weapon parsers use, so a slightly-off mode still loads with sane values.
struct GameModeParseResult {
    bool ok{false};
    GameModeDef mode;
    std::string error;
    std::vector<std::string> warnings;
};

// Parse a `[mode]`/`[teams]`/`[scoring]`/`[match]`/`[respawn]`/`[rules]` TOML document into a
// GameModeDef. The validator (tools/validate-mode) and the engine both call THIS function, so they
// cannot drift. Schema (all sections optional; omitted keys take the GameModeDef defaults):
//
//   [mode]    id = "pack:tdm"  name = "Team Deathmatch"
//   [teams]   use_mission_sides = false
//   [[teams.team]] id = "red"  name = "Red"  capacity = 16
//   [[teams.team]] id = "blue" name = "Blue" capacity = 16
//   [scoring] points_per_kill = 1  points_per_assist = 0  points_per_objective = 0  score_limit = 50
//   [match]   time_limit_min = 15  warmup_s = 30  min_players = 2
//   [respawn] delay_s = 10  waves = false  wave_interval_s = 15
//   [rules]   friendly_fire = "off"   # "server" | "on" | "off"
GameModeParseResult parseGameModeToml(std::string_view tomlContent);

} // namespace fl
