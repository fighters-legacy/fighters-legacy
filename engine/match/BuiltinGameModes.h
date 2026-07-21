// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "match/GameModeParser.h"

#include <optional>
#include <string_view>

namespace fl {

// Compiled-in game modes (#521), resolved by id so a zero-content-pack server can run a real match.
// Embedded as TOML strings and parsed through the SAME parseGameModeToml the pack path uses, so a
// builtin cannot diverge from the schema. Two ship now:
//
//   builtin:free-flight  the global default; no teams beyond the mission's, unlimited, no limits, no
//                        warmup, respawn immediate, friendly fire = server default. Byte-identical to
//                        the pre-#521 sandbox behavior, so an unconfigured server behaves as it did.
//   builtin:tdm          two teams (Red/Blue), score limit 50, 15-minute clock, 10 s respawn, 30 s
//                        warmup, friendly fire off, needs 2 players.
//
// Strike/conquest are deferred until the objective-scoring channel lands (#1000).

namespace detail {

inline constexpr std::string_view kBuiltinFreeFlightToml = R"([mode]
id = "builtin:free-flight"
name = "Free Flight"

[teams]
use_mission_sides = true

[scoring]
points_per_kill = 1
score_limit = 0

[match]
time_limit_min = 0
warmup_s = 0
min_players = 1

[respawn]
delay_s = 0

[rules]
friendly_fire = "server"
)";

inline constexpr std::string_view kBuiltinTdmToml = R"([mode]
id = "builtin:tdm"
name = "Team Deathmatch"

[teams]
use_mission_sides = false
[[teams.team]]
id = "red"
name = "Red Force"
capacity = 0
[[teams.team]]
id = "blue"
name = "Blue Force"
capacity = 0

[scoring]
points_per_kill = 1
score_limit = 50

[match]
time_limit_min = 15
warmup_s = 30
min_players = 2

[respawn]
delay_s = 10
waves = false

[rules]
friendly_fire = "off"
)";

} // namespace detail

// Resolve a "builtin:" game-mode id to its parsed GameModeDef, or nullopt if the id is not a builtin.
// The default (unknown/empty) server behavior should use builtin:free-flight.
inline std::optional<GameModeDef> builtinGameMode(std::string_view id) {
    std::string_view toml;
    if (id == "builtin:free-flight")
        toml = detail::kBuiltinFreeFlightToml;
    else if (id == "builtin:tdm")
        toml = detail::kBuiltinTdmToml;
    else
        return std::nullopt;
    GameModeParseResult r = parseGameModeToml(toml);
    if (!r.ok)
        return std::nullopt; // a compiled-in mode should always parse; guard anyway
    return r.mode;
}

// The default game mode when nothing is configured — free flight (today's behavior).
inline GameModeDef defaultGameMode() {
    return builtinGameMode("builtin:free-flight").value_or(GameModeDef{});
}

} // namespace fl
