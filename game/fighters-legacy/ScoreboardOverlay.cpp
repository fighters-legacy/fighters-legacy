// SPDX-License-Identifier: GPL-3.0-or-later
#include "ScoreboardOverlay.h"

#include "IGui.h"

#include <algorithm>
#include <array>
#include <cstdio>

namespace fl {

std::string_view matchPhaseLabel(uint8_t phase) noexcept {
    switch (phase) {
    case 0:
        return "PRE-MATCH";
    case 1:
        return "WARMUP";
    case 2:
        return "ACTIVE";
    case 3:
        return "MATCH OVER";
    case 4:
        return "MATCH OVER";
    default:
        return "";
    }
}

bool scoreboardAutoShows(uint8_t phase) noexcept {
    return phase == 3 || phase == 4; // Ending / PostMatch
}

namespace {

// Sort a team's players: higher score first, then more kills, then callsign for a stable order.
bool scoreDesc(const ScoreboardPlayer& a, const ScoreboardPlayer& b) {
    if (a.score != b.score)
        return a.score > b.score;
    if (a.kills != b.kills)
        return a.kills > b.kills;
    return a.callsign < b.callsign;
}

// Emit one participant row into the 5-column table (Player, K, D, Score, Ping). The self row is marked
// with a leading marker (the read-only table cannot colour, so the marker is the highlight).
void emitPlayerRow(IGui& gui, const ScoreboardPlayer& p) {
    gui.tableNextRow();
    std::string name;
    if (p.isSelf)
        name += "> ";
    name += p.callsign;
    if (p.isBot)
        name += " [bot]";
    gui.tableCell(name);

    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(p.kills));
    gui.tableCell(buf);
    std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(p.deaths));
    gui.tableCell(buf);
    std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(p.score));
    gui.tableCell(buf);
    if (p.isBot)
        gui.tableCell("--");
    else {
        std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(p.pingMs));
        gui.tableCell(buf);
    }
}

} // namespace

void ScoreboardOverlay::render(IGui* guiPtr, const ScoreboardData& data) {
    if (!guiPtr)
        return;
    IGui& gui = *guiPtr;

    if (!gui.beginWindow("Scoreboard", 0.20f, 0.12f, 0.60f, 0.72f)) {
        gui.endWindow();
        return;
    }

    // Match header: mode, phase and countdown.
    if (data.hasMatch) {
        std::string header = data.modeName.empty() ? std::string("Match") : data.modeName;
        if (!data.phaseLabel.empty())
            header += "  \xE2\x80\x94  " + data.phaseLabel; // em dash
        if (data.secondsRemaining >= 0) {
            char t[24];
            const unsigned s = static_cast<unsigned>(std::clamp<std::int64_t>(data.secondsRemaining, 0, 359999));
            std::snprintf(t, sizeof(t), "  %u:%02u", s / 60u, s % 60u);
            header += t;
        }
        if (data.scoreLimit > 0) {
            char lim[24];
            std::snprintf(lim, sizeof(lim), "  (to %u)", static_cast<unsigned>(data.scoreLimit));
            header += lim;
        }
        gui.label(header);
        gui.separator();
    }

    static constexpr std::array<std::string_view, 5> kHeaders{"Player", "K", "D", "Score", "Ping"};
    const std::span<const std::string_view> headerSpan(kHeaders.data(), kHeaders.size());

    if (data.hasMatch && !data.teams.empty()) {
        // Grouped by team: a labelled section (team name + authoritative match score) then that team's
        // players sorted by score.
        for (const ScoreboardTeam& team : data.teams) {
            char teamHdr[96];
            std::snprintf(teamHdr, sizeof(teamHdr), "%s  \xE2\x80\x94  %d",
                          team.name.empty() ? "Team" : team.name.c_str(), static_cast<int>(team.score));
            gui.label(teamHdr);

            std::vector<ScoreboardPlayer> rows;
            for (const ScoreboardPlayer& p : data.players)
                if (p.factionIndex == team.factionIndex)
                    rows.push_back(p);
            std::sort(rows.begin(), rows.end(), scoreDesc);

            char tableId[24];
            std::snprintf(tableId, sizeof(tableId), "team%u", static_cast<unsigned>(team.factionIndex));
            if (gui.beginTable(tableId, 5)) {
                gui.tableHeadersRow(headerSpan);
                for (const ScoreboardPlayer& p : rows)
                    emitPlayerRow(gui, p);
                gui.endTable();
            }
            gui.separator();
        }
    } else {
        // Free-flight / no teams: a single flat table sorted by score.
        std::vector<ScoreboardPlayer> rows = data.players;
        std::sort(rows.begin(), rows.end(), scoreDesc);
        if (gui.beginTable("players", 5)) {
            gui.tableHeadersRow(headerSpan);
            for (const ScoreboardPlayer& p : rows)
                emitPlayerRow(gui, p);
            gui.endTable();
        }
    }

    gui.endWindow();
}

} // namespace fl
