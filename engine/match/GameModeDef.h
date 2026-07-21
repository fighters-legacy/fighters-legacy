// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fl {

// A team a game mode defines (#521). Teams ARE FactionRegistry factions at runtime; fl-server maps a
// mode's teams onto factions at load (either the mission's sides or a synthesized set). `capacity` 0 =
// unlimited players on the team.
struct GameModeTeam {
    std::string id;   // e.g. "red"
    std::string name; // e.g. "Red Force"; empty falls back to id
    int capacity{0};  // max players; 0 = unlimited
};

// Friendly-fire policy for a mode. `Server` defers to the server's [gameplay] friendly_fire; On/Off
// force it regardless of the server default, so a mode is self-contained.
enum class ModeFriendlyFire : uint8_t {
    Server = 0,
    On = 1,
    Off = 2,
};

// A data-driven multiplayer game mode (#521). Loaded from a `modes/*.toml` content-pack asset or one of
// the compiled-in builtins (BuiltinGameModes.h). Pure data; the MatchController (#523) reads it to run
// the match lifecycle, and fl-server maps its teams onto the FactionRegistry (#522).
//
// The default-constructed value is `builtin:free-flight` in spirit: no teams beyond the mission's,
// unlimited, no score/time limit, no warmup, respawn immediate — i.e. today's sandbox behavior.
struct GameModeDef {
    std::string id;   // "builtin:tdm" or "pack:my-mode"
    std::string name; // display name; empty falls back to id

    // Team model. When useMissionSides is true the mode inherits the loaded mission's `sides` as its
    // teams (and `teams` is ignored); otherwise `teams` defines them and fl-server synthesizes the
    // factions. A mode with neither is a free-for-all against the mission/world factions.
    bool useMissionSides{true};
    std::vector<GameModeTeam> teams;

    // Scoring. pointsPerAssist / pointsPerObjective are parsed and stored now but only wired when the
    // objective-scoring channel lands (#1000) — a mode may declare them today without effect.
    int pointsPerKill{1};
    int pointsPerAssist{0};
    int pointsPerObjective{0};
    int scoreLimit{0};    // team score that ends the match; 0 = no score limit
    double timeLimitS{0}; // match time limit in seconds; 0 = no time limit

    // Respawn policy (consumed by #648). delay 0 = respawn immediately on request.
    double respawnDelayS{0};
    bool respawnWaves{false};
    double waveIntervalS{15};

    ModeFriendlyFire friendlyFire{ModeFriendlyFire::Server};

    // Match pacing.
    double warmupS{0}; // warmup countdown before the match goes active; 0 = start active immediately
    int minPlayers{1}; // humans required before warmup counts down / the match starts
};

} // namespace fl
