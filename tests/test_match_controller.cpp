// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "match/BotFillPolicy.h"
#include "match/MatchController.h"
#include "match/TeamBalancer.h"

#include <vector>

using namespace fl;

// The fixed step every ISimUpdate receives; MatchController ignores it and gates on the tick.
constexpr double kDt = 1.0 / 60.0;

namespace {
// Two-team TDM-like mode with short deterministic timings for testing.
GameModeDef testTdm(int scoreLimit, double timeLimitS, double warmupS, int minPlayers) {
    GameModeDef m;
    m.id = "test:tdm";
    m.name = "Test TDM";
    m.useMissionSides = false;
    m.teams = {{"red", "Red", 0}, {"blue", "Blue", 0}};
    m.pointsPerKill = 1;
    m.scoreLimit = scoreLimit;
    m.timeLimitS = timeLimitS;
    m.warmupS = warmupS;
    m.minPlayers = minPlayers;
    return m;
}
std::vector<TeamState> twoTeams() {
    return {{1, 0, 0}, {2, 0, 0}};
}
} // namespace

TEST_CASE("TeamBalancer: pickTeam chooses the smaller team, ties to lower index", "[team_balancer]") {
    std::vector<TeamState> teams = {{1, 0, 3}, {2, 0, 1}};
    CHECK(pickTeam(teams) == 2u); // team 2 has fewer players

    std::vector<TeamState> even = {{1, 0, 2}, {2, 0, 2}};
    CHECK(pickTeam(even) == 1u); // tie -> lower factionIndex

    std::vector<TeamState> empty;
    CHECK_FALSE(pickTeam(empty).has_value());
}

TEST_CASE("TeamBalancer: pickTeam respects capacity, refuses when all full", "[team_balancer]") {
    std::vector<TeamState> teams = {{1, 4, 4}, {2, 4, 2}};
    CHECK(pickTeam(teams) == 2u); // team 1 is full

    std::vector<TeamState> allFull = {{1, 2, 2}, {2, 2, 2}};
    CHECK_FALSE(pickTeam(allFull).has_value()); // MatchFull

    // Unlimited (capacity 0) always has room.
    std::vector<TeamState> unlimited = {{1, 0, 100}, {2, 2, 2}};
    CHECK(pickTeam(unlimited) == 1u);
}

TEST_CASE("TeamBalancer: switchAllowed forbids stacking", "[team_balancer]") {
    // from {count 3} to {count 1}: 1+1=2 <= 3-1+1=3 -> allowed (rebalancing).
    CHECK(switchAllowed(TeamState{1, 0, 3}, TeamState{2, 0, 1}));
    // from {count 1} to {count 3}: 3+1=4 <= 1-1+1=1 -> denied (would stack).
    CHECK_FALSE(switchAllowed(TeamState{1, 0, 1}, TeamState{2, 0, 3}));
    // from an equal team {2} to an equal team {2}: after the move it is 1 vs 3 -> denied (unbalances).
    CHECK_FALSE(switchAllowed(TeamState{1, 0, 2}, TeamState{2, 0, 2}));
    // from a larger team {3} to an equal-after team {2}: after it is 2 vs 3 -> allowed (evens out).
    CHECK(switchAllowed(TeamState{1, 0, 3}, TeamState{2, 0, 2}));
    // Full destination is denied.
    CHECK_FALSE(switchAllowed(TeamState{1, 0, 3}, TeamState{2, 2, 2}));
    // Switching to the same team is denied.
    CHECK_FALSE(switchAllowed(TeamState{1, 0, 3}, TeamState{1, 0, 3}));
}

TEST_CASE("MatchController: Idle -> Warmup -> Active on join with warmup", "[match_controller]") {
    MatchController mc;
    mc.configure(testTdm(50, 0, 2.0, 1), twoTeams(), 1.0); // simDt=1s: warmup 2 ticks
    CHECK(mc.phase() == MatchPhase::Idle);
    mc.onTick(kDt, 0);
    CHECK(mc.phase() == MatchPhase::Idle); // no participants yet

    mc.participantJoined(0, 1, false);
    mc.onTick(kDt, 1); // Idle -> Warmup (countdown not yet started)
    CHECK(mc.phase() == MatchPhase::Warmup);
    mc.onTick(kDt, 2); // in Warmup with minPlayers met -> countdown starts, endTick = 2 + 2 = 4
    CHECK(mc.phase() == MatchPhase::Warmup);
    mc.onTick(kDt, 3); // tick < endTick
    CHECK(mc.phase() == MatchPhase::Warmup);
    mc.onTick(kDt, 4); // tick >= endTick
    CHECK(mc.phase() == MatchPhase::Active);
}

TEST_CASE("MatchController: warmup=0 goes straight to Active", "[match_controller]") {
    MatchController mc;
    mc.configure(testTdm(50, 0, 0.0, 1), twoTeams(), 1.0);
    mc.participantJoined(0, 1, false);
    mc.onTick(kDt, 1); // Idle->Warmup
    mc.onTick(kDt, 2); // Warmup(0)->Active
    CHECK(mc.phase() == MatchPhase::Active);
}

TEST_CASE("MatchController: score limit ends the match; scoring frozen outside Active", "[match_controller]") {
    MatchController mc;
    mc.configure(testTdm(2, 0, 0.0, 1), twoTeams(), 1.0);
    mc.setEndingSeconds(1.0);
    mc.participantJoined(0, 1, false); // team 1 (red)
    mc.participantJoined(10, 2, false);

    // A kill during Warmup is ignored.
    mc.recordKill(0, 10, false);
    mc.onTick(kDt, 1); // Idle->Warmup
    mc.onTick(kDt, 2); // Warmup->Active
    REQUIRE(mc.phase() == MatchPhase::Active);
    CHECK(mc.teamScores()[0].score == 0); // the warmup kill did not count

    mc.recordKill(0, 10, false); // team 1 scores 1
    mc.recordKill(0, 10, false); // team 1 scores 2 -> reaches limit
    mc.onTick(kDt, 3);
    CHECK(mc.phase() == MatchPhase::Ending);
    CHECK(mc.winner() == 1u);
    CHECK(mc.combatFrozen());

    // Ending lasts 1 tick -> PostMatch + rotate fires once.
    int rotates = 0;
    mc.setOnRotate([&] { ++rotates; });
    mc.onTick(kDt, 4); // >= ending end tick (3 + 1)
    CHECK(mc.phase() == MatchPhase::PostMatch);
    CHECK(rotates == 1);
}

TEST_CASE("MatchController: same-team kill scores nothing", "[match_controller]") {
    MatchController mc;
    mc.configure(testTdm(50, 0, 0.0, 1), twoTeams(), 1.0);
    mc.participantJoined(0, 1, false);
    mc.participantJoined(1, 1, false);
    mc.onTick(kDt, 1);
    mc.onTick(kDt, 2);
    REQUIRE(mc.phase() == MatchPhase::Active);
    mc.recordKill(0, 1, /*sameTeam=*/true);
    CHECK(mc.teamScores()[0].score == 0);
}

TEST_CASE("MatchController: recordObjective scores during Active, frozen outside (#1000)", "[match_controller]") {
    MatchController mc;
    GameModeDef strike = testTdm(100, 0, 0.0, 1);
    strike.pointsPerObjective = 10;
    mc.configure(strike, twoTeams(), 1.0);
    mc.participantJoined(0, 1, false);

    // Frozen during warmup (before Active).
    mc.recordObjective(1, 1);
    mc.onTick(kDt, 1); // Idle->Warmup
    mc.onTick(kDt, 2); // Warmup->Active
    REQUIRE(mc.phase() == MatchPhase::Active);
    CHECK(mc.teamScores()[0].score == 0); // the warmup objective did not count

    mc.recordObjective(1, 1); // team 1 (index 0): +10
    CHECK(mc.teamScores()[0].score == 10);
    mc.recordObjective(1, 3); // +30
    CHECK(mc.teamScores()[0].score == 40);
    mc.recordObjective(2, 1); // team 2 (index 1): +10
    CHECK(mc.teamScores()[1].score == 10);
    // A non-positive count or an unknown faction awards nothing.
    mc.recordObjective(1, 0);
    mc.recordObjective(99, 1);
    CHECK(mc.teamScores()[0].score == 40);
}

TEST_CASE("MatchController: recordObjective is inert when the mode has no objective points (#1000)",
          "[match_controller]") {
    MatchController mc;
    mc.configure(testTdm(50, 0, 0.0, 1), twoTeams(), 1.0); // pointsPerObjective defaults to 0
    mc.participantJoined(0, 1, false);
    mc.onTick(kDt, 1); // Idle->Warmup
    mc.onTick(kDt, 2); // Warmup->Active
    REQUIRE(mc.phase() == MatchPhase::Active);
    mc.recordObjective(1, 5);
    CHECK(mc.teamScores()[0].score == 0);
}

TEST_CASE("MatchController: time limit ends with the leader; a tie is a draw", "[match_controller]") {
    MatchController mc;
    mc.configure(testTdm(0, 3.0, 0.0, 1), twoTeams(), 1.0); // 3-tick time limit
    mc.participantJoined(0, 1, false);
    mc.participantJoined(10, 2, false);
    mc.onTick(kDt, 1); // Warmup
    mc.onTick(kDt, 2); // Active (activeStart=2, endTick=5)
    REQUIRE(mc.phase() == MatchPhase::Active);
    mc.recordKill(0, 10, false); // team 1 leads
    mc.onTick(kDt, 5);           // time limit reached
    CHECK(mc.phase() == MatchPhase::Ending);
    CHECK(mc.winner() == 1u);
}

TEST_CASE("MatchController: forceEnd ends the match", "[match_controller]") {
    MatchController mc;
    mc.configure(testTdm(0, 0, 0.0, 1), twoTeams(), 1.0);
    mc.participantJoined(0, 1, false);
    mc.onTick(kDt, 1);
    mc.onTick(kDt, 2);
    REQUIRE(mc.phase() == MatchPhase::Active);
    mc.forceEnd(std::nullopt);
    mc.onTick(kDt, 3);
    CHECK(mc.phase() == MatchPhase::Ending);
}

TEST_CASE("MatchController: warmup holds below minPlayers", "[match_controller]") {
    MatchController mc;
    mc.configure(testTdm(50, 0, 2.0, 2), twoTeams(), 1.0); // needs 2 players
    mc.participantJoined(0, 1, false);
    mc.onTick(kDt, 1); // Warmup, but only 1 human -> countdown does not run
    mc.onTick(kDt, 50);
    CHECK(mc.phase() == MatchPhase::Warmup); // still held
    mc.participantJoined(1, 2, false);
    mc.onTick(kDt, 51); // countdown starts (endTick 53)
    mc.onTick(kDt, 53);
    CHECK(mc.phase() == MatchPhase::Active);
}

TEST_CASE("BotFillPolicy: desiredBots fills to target, capped", "[bot_fill]") {
    CHECK(desiredBots(0, 8, 16) == 8);   // no humans -> 8 bots
    CHECK(desiredBots(3, 8, 16) == 5);   // 3 humans -> 5 bots
    CHECK(desiredBots(8, 8, 16) == 0);   // full of humans -> no bots
    CHECK(desiredBots(12, 8, 16) == 0);  // more humans than target -> no bots (clamped)
    CHECK(desiredBots(0, 20, 16) == 16); // capped at maxBots
    CHECK(desiredBots(2, 0, 16) == 0);   // fill 0 = disabled
}
