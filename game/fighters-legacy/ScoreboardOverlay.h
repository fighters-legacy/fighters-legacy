// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fl {

class IGui;

// The multiplayer scoreboard (#647): an IGui table shown while the Scoreboard key is held and auto-shown
// in the match end phase. It renders a plain ScoreboardData snapshot (assembled in Game.cpp from the
// ClientNetEventHandler's match state + scoreboard rows + roster), so the overlay stays free of the
// network handler and is unit-testable against the scripted NullGui. Grouping/sort/totals/highlight all
// live in render(); Game.cpp only assembles the data and decides visibility.

// One participant's row.
struct ScoreboardPlayer {
    std::string callsign;
    uint16_t factionIndex{0};
    int32_t score{0};
    uint16_t kills{0};
    uint16_t deaths{0};
    uint16_t pingMs{0};
    bool isBot{false};
    bool isSelf{false};
};

// A team's display record (from MsgMatchState; the authoritative match score, not a sum of player rows).
struct ScoreboardTeam {
    uint16_t factionIndex{0};
    std::string name;
    int32_t score{0};
};

// The immutable per-frame snapshot the overlay renders.
struct ScoreboardData {
    bool hasMatch{false};              // true once MsgMatchState has arrived (team grouping + phase header)
    std::string modeName;              // game-mode display name
    std::string phaseLabel;            // "WARMUP" / "ACTIVE" / "MATCH OVER" / …
    std::int64_t secondsRemaining{-1}; // phase countdown; < 0 = untimed
    uint16_t scoreLimit{0};            // team score that ends the match; 0 = none
    std::vector<ScoreboardTeam> teams; // teams, in display order
    std::vector<ScoreboardPlayer> players;
};

// Phase-ordinal (MatchPhase) presentation helpers — kept here so the mapping and the auto-show rule are
// tested with the overlay and Game.cpp stays declarative. Ordinals: 0 Idle, 1 Warmup, 2 Active,
// 3 Ending, 4 PostMatch (mirrors fl::MatchPhase without linking engine-match into the client).
[[nodiscard]] std::string_view matchPhaseLabel(uint8_t phase) noexcept;
[[nodiscard]] bool scoreboardAutoShows(uint8_t phase) noexcept;

class ScoreboardOverlay {
  public:
    // Emit the scoreboard through the injected GUI. No-op when gui is null.
    void render(IGui* gui, const ScoreboardData& data);
};

} // namespace fl
