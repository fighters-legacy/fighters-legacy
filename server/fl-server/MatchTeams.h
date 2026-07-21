// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ILogger.h"
#include "entity/DamageApplication.h" // DamageRules
#include "match/GameModeDef.h"
#include "match/TeamBalancer.h"
#include "world/FactionRegistry.h"

#include <cstdio>
#include <string>
#include <vector>

namespace fl {

// The teams a match runs with (#522), mapped onto FactionRegistry indices. `haveTeams` is false for a
// free-for-all (free-flight, or use_mission_sides with no mission loaded) — the caller then leaves the
// team assigner unset, preserving the legacy m_playerFaction behavior.
struct MatchTeamSetup {
    std::vector<TeamState> teams; // factionIndex + capacity; count is filled live by the assigner
    bool haveTeams{false};
    bool synthesizedRegistry{false}; // true if we loaded `factions` ourselves (no mission provided sides)
};

// Map a game mode's teams onto the FactionRegistry. Cases (matching the design):
//   * use_mission_sides (or no explicit teams): teams are the mission's non-neutral factions
//     (capacity unlimited). With no mission loaded there are no sides → a free-for-all.
//   * explicit teams + a mission with sides: positional aliasing — mode team i -> mission faction i+1
//     (the mode supplies capacity/display; a count mismatch warns and the mission's sides win).
//   * explicit teams + no mission: synthesize the FactionRegistry from the mode teams (index 0 neutral,
//     teams 1..N mutually hostile) and load it, so a zero-pack TDM server has real, hostile teams.
[[nodiscard]] inline MatchTeamSetup buildMatchTeams(const GameModeDef& mode, FactionRegistry& factions, ILogger& log) {
    MatchTeamSetup out;

    const bool modeDefinesTeams = !mode.useMissionSides && !mode.teams.empty();

    if (!modeDefinesTeams) {
        // Teams follow the mission's sides. Every non-neutral faction is a team, unlimited capacity.
        for (uint16_t fi = 1; fi < factions.count(); ++fi)
            out.teams.push_back(TeamState{fi, /*capacity=*/0, /*count=*/0});
        out.haveTeams = out.teams.size() >= 2; // a single side needs no balancing
        return out;
    }

    if (factions.count() > 1) {
        // Positional aliasing onto the mission's existing sides.
        if (mode.teams.size() != static_cast<std::size_t>(factions.count() - 1)) {
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                          "game mode defines %zu team(s) but the mission has %u side(s); the mission's sides win",
                          mode.teams.size(), static_cast<unsigned>(factions.count() - 1));
            log.log(LogLevel::Warn, __FILE__, __LINE__, buf);
        }
        for (std::size_t i = 0; i < mode.teams.size(); ++i) {
            const uint16_t fi = static_cast<uint16_t>(i + 1);
            if (fi >= factions.count())
                break;
            out.teams.push_back(TeamState{fi, mode.teams[i].capacity, 0});
        }
        out.haveTeams = out.teams.size() >= 2;
        return out;
    }

    // Synthesize the registry from the mode teams (no mission provided sides).
    std::vector<FactionDef> defs;
    defs.push_back(FactionDef{"neutral", "Neutral", AlertLevel::Peacetime}); // index 0
    for (const GameModeTeam& t : mode.teams)
        defs.push_back(FactionDef{t.id, t.name.empty() ? t.id : t.name, AlertLevel::Peacetime});
    factions.load(std::move(defs));
    // Load seeds off-diagonal Neutral; make distinct teams explicitly Hostile so sensing/AI fight.
    for (uint16_t a = 1; a < factions.count(); ++a)
        for (uint16_t b = static_cast<uint16_t>(a + 1); b < factions.count(); ++b)
            factions.setRelationship(a, b, FactionRelation::Hostile);
    out.synthesizedRegistry = true;

    for (std::size_t i = 0; i < mode.teams.size(); ++i)
        out.teams.push_back(TeamState{static_cast<uint16_t>(i + 1), mode.teams[i].capacity, 0});
    out.haveTeams = out.teams.size() >= 2;
    return out;
}

// The effective friendly-fire rule for a mode: a mode override (On/Off) wins over the server default.
[[nodiscard]] inline DamageRules effectiveDamageRules(const GameModeDef& mode, bool serverFriendlyFire,
                                                      bool crashDamage) {
    bool ff = serverFriendlyFire;
    if (mode.friendlyFire == ModeFriendlyFire::On)
        ff = true;
    else if (mode.friendlyFire == ModeFriendlyFire::Off)
        ff = false;
    return DamageRules{ff, crashDamage};
}

} // namespace fl
