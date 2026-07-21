// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "match/TeamBalancer.h"

#include <vector>

using namespace fl;

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
