// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for the multiplayer ScoreboardOverlay (#647), driven against the scripted NullGui (#156).
// The overlay renders a plain ScoreboardData POD, so the test builds the data directly and asserts the
// emitted IGui vocabulary (window / labels / table headers + cells / grouping / sort / self highlight).

#include "ScoreboardOverlay.h"

#include "mock_gui.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>

using namespace fl;

namespace {

bool hasCell(const NullGui& gui, const std::string& text) {
    return std::find(gui.cells.begin(), gui.cells.end(), text) != gui.cells.end();
}

// Index of the first cell equal to `text`, or -1.
int cellIndex(const NullGui& gui, const std::string& text) {
    for (std::size_t i = 0; i < gui.cells.size(); ++i)
        if (gui.cells[i] == text)
            return static_cast<int>(i);
    return -1;
}

ScoreboardPlayer player(std::string name, uint16_t faction, int score, uint16_t kills, uint16_t deaths,
                        uint16_t ping = 20, bool self = false, bool bot = false) {
    ScoreboardPlayer p;
    p.callsign = std::move(name);
    p.factionIndex = faction;
    p.score = score;
    p.kills = kills;
    p.deaths = deaths;
    p.pingMs = ping;
    p.isSelf = self;
    p.isBot = bot;
    return p;
}

} // namespace

TEST_CASE("matchPhaseLabel + scoreboardAutoShows map the phase ordinals", "[scoreboard]") {
    CHECK(matchPhaseLabel(1) == "WARMUP");
    CHECK(matchPhaseLabel(2) == "ACTIVE");
    CHECK(matchPhaseLabel(3) == "MATCH OVER");
    CHECK(matchPhaseLabel(4) == "MATCH OVER");
    CHECK_FALSE(scoreboardAutoShows(1));
    CHECK_FALSE(scoreboardAutoShows(2));
    CHECK(scoreboardAutoShows(3)); // Ending
    CHECK(scoreboardAutoShows(4)); // PostMatch
}

TEST_CASE("ScoreboardOverlay: null gui is a no-op", "[scoreboard]") {
    ScoreboardOverlay ov;
    ScoreboardData data;
    ov.render(nullptr, data); // must not crash
    SUCCEED();
}

TEST_CASE("ScoreboardOverlay: teams grouped, players sorted by score, self marked", "[scoreboard]") {
    NullGui gui;
    ScoreboardOverlay ov;

    ScoreboardData data;
    data.hasMatch = true;
    data.modeName = "Team Deathmatch";
    data.phaseLabel = "ACTIVE";
    data.secondsRemaining = 125; // 2:05
    data.scoreLimit = 50;
    data.teams = {{1, "Red", 12}, {2, "Blue", 7}};
    data.players = {
        player("Maverick", 1, 8, 8, 1, 20, /*self=*/true),
        player("Iceman", 1, 3, 3, 2),
        player("Viper", 2, 5, 5, 0),
        player("Jester-bot", 2, 2, 2, 4, 0, false, /*bot=*/true),
    };

    ov.render(&gui, data);

    // A window + a header table per team.
    REQUIRE(gui.windows.size() == 1u);
    CHECK(gui.windows[0] == "Scoreboard");

    // The match header label contains mode, phase and the countdown.
    bool foundHeader = false;
    for (const auto& l : gui.labels)
        if (l.find("Team Deathmatch") != std::string::npos && l.find("ACTIVE") != std::string::npos &&
            l.find("2:05") != std::string::npos)
            foundHeader = true;
    CHECK(foundHeader);

    // Team section labels carry the authoritative match score.
    bool redHdr = false, blueHdr = false;
    for (const auto& l : gui.labels) {
        if (l.find("Red") != std::string::npos && l.find("12") != std::string::npos)
            redHdr = true;
        if (l.find("Blue") != std::string::npos && l.find("7") != std::string::npos)
            blueHdr = true;
    }
    CHECK(redHdr);
    CHECK(blueHdr);

    // Two table rows per team → 4 rows total.
    CHECK(gui.rowCount == 4);

    // Self row is marked; bot row is badged.
    CHECK(hasCell(gui, "> Maverick"));
    CHECK(hasCell(gui, "Jester-bot [bot]"));

    // Within Red, Maverick (8) sorts above Iceman (3): its name cell comes first.
    CHECK(cellIndex(gui, "> Maverick") < cellIndex(gui, "Iceman"));

    // A bot's ping renders as "--", not a number.
    // (Jester-bot has ping 0 but is a bot → dashes.)
    CHECK(hasCell(gui, "--"));
}

TEST_CASE("ScoreboardOverlay: free-flight with no teams uses one flat table", "[scoreboard]") {
    NullGui gui;
    ScoreboardOverlay ov;

    ScoreboardData data;
    data.hasMatch = false;
    data.players = {
        player("Alpha", 0, 4, 4, 0),
        player("Bravo", 0, 9, 9, 1),
    };

    ov.render(&gui, data);
    // Headers emitted once (single table).
    CHECK(gui.rowCount == 2);
    // Bravo (9) sorts above Alpha (4).
    CHECK(cellIndex(gui, "Bravo") < cellIndex(gui, "Alpha"));
    // No team header labels in free-flight.
    for (const auto& l : gui.labels)
        CHECK(l.find("Team") == std::string::npos);
}
