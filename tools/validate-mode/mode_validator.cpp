// SPDX-License-Identifier: GPL-3.0-or-later
#include "mode_validator.h"

#include "match/GameModeParser.h"

#include <set>

namespace fl {

GameModeValidationResult validateGameMode(std::string_view tomlContent) {
    GameModeValidationResult out;

    const GameModeParseResult parsed = parseGameModeToml(tomlContent);
    for (const std::string& w : parsed.warnings)
        out.warnings.push_back(w);
    if (!parsed.ok) {
        out.ok = false;
        out.errors.push_back(parsed.error);
        return out;
    }

    const GameModeDef& m = parsed.mode;

    // Duplicate team ids.
    std::set<std::string> ids;
    for (const GameModeTeam& t : m.teams) {
        if (!ids.insert(t.id).second)
            out.errors.push_back("duplicate team id '" + t.id + "'");
    }

    // Explicit teams with mission-sides off need at least two to be a meaningful team mode.
    if (!m.useMissionSides && m.teams.size() < 2)
        out.warnings.push_back("use_mission_sides is false but fewer than two teams are defined — "
                               "a team mode usually wants two or more sides");

    // Total declared capacity vs a plausible player cap (128 is the project's scale target).
    if (!m.teams.empty()) {
        long totalCap = 0;
        bool anyUnlimited = false;
        for (const GameModeTeam& t : m.teams) {
            if (t.capacity == 0)
                anyUnlimited = true;
            else
                totalCap += t.capacity;
        }
        if (!anyUnlimited && totalCap < 2)
            out.errors.push_back("the sum of team capacities is below 2 — no match could start");
        if (!anyUnlimited && totalCap > 128)
            out.warnings.push_back("the sum of team capacities exceeds the 128-player scale target");
    }

    // Warmup longer than the match clock means the match ends before it begins.
    if (m.timeLimitS > 0.0 && m.warmupS >= m.timeLimitS)
        out.errors.push_back("warmup_s is >= the match time limit — the match would end during warmup");

    // A score-limit-only mode with points_per_kill == 0 can never reach the limit.
    if (m.scoreLimit > 0 && m.pointsPerKill == 0 && m.pointsPerObjective == 0)
        out.warnings.push_back("score_limit is set but neither points_per_kill nor points_per_objective "
                               "awards points — the score limit is unreachable");

    if (!out.errors.empty())
        out.ok = false;
    return out;
}

} // namespace fl
