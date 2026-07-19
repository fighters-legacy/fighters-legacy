// SPDX-License-Identifier: GPL-3.0-or-later
//
// Crew seat picker (#966/#975): the pure roster-driven seat-selection logic that the SDL layer drives.

#include "CrewSeatMenu.h"

#include <catch2/catch_test_macros.hpp>

#include <unordered_map>

using namespace fl;

namespace {
CrewSeatInfo seat(uint8_t idx, CrewCapabilityMask caps, SeatOccupancy occ, uint32_t peer = 0xFFFFFFFFu,
                  const char* role = "seat") {
    CrewSeatInfo s;
    s.seatIndex = idx;
    s.capabilities = caps;
    s.occupancy = static_cast<uint8_t>(occ);
    s.occupantPeerId = peer;
    s.role = role;
    return s;
}
CrewRosterInfo bomber(uint32_t idx, uint32_t gen = 1) {
    CrewRosterInfo r;
    r.entityIdx = idx;
    r.entityGen = gen;
    const auto fly = static_cast<CrewCapabilityMask>(CrewCapability::Fly);
    const auto fire = static_cast<CrewCapabilityMask>(CrewCapability::Fire);
    r.seats.push_back(seat(0, fly, SeatOccupancy::Human, 5, "pilot"));                // Fly seat, human-held
    r.seats.push_back(seat(1, fire, SeatOccupancy::Bot, 0xFFFFFFFFu, "tail-gunner")); // joinable
    return r;
}
} // namespace

TEST_CASE("CrewSeatMenu: occupiedSeat / seatIsFly (#975)", "[crew][seat-menu]") {
    const CrewRosterInfo r = bomber(7);
    CHECK(occupiedSeat(r, 5) == 0); // peer 5 holds the pilot seat
    CHECK(occupiedSeat(r, 9) == -1);
    CHECK(seatIsFly(r, 0));
    CHECK_FALSE(seatIsFly(r, 1));
    CHECK_FALSE(seatIsFly(r, 99)); // out of range
}

TEST_CASE("CrewSeatMenu: only non-fly non-human seats are joinable (#975)", "[crew][seat-menu]") {
    const auto fly = static_cast<CrewCapabilityMask>(CrewCapability::Fly);
    const auto fire = static_cast<CrewCapabilityMask>(CrewCapability::Fire);
    CHECK_FALSE(seatJoinable(seat(0, fly, SeatOccupancy::Empty)));     // Fly seat: never joinable
    CHECK(seatJoinable(seat(1, fire, SeatOccupancy::Bot)));            // bot seat: joinable (parks the bot)
    CHECK(seatJoinable(seat(1, fire, SeatOccupancy::Empty)));          // empty seat: joinable
    CHECK_FALSE(seatJoinable(seat(1, fire, SeatOccupancy::Human, 3))); // human-held: not joinable
}

TEST_CASE("CrewSeatMenu: the picker flattens joinable seats and cycles deterministically (#975)", "[crew][seat-menu]") {
    std::unordered_map<uint32_t, CrewRosterInfo> rosters;
    rosters.emplace(20, bomber(20));
    rosters.emplace(7, bomber(7));

    CrewSeatPicker picker;
    picker.rebuild(rosters);

    // One joinable seat per bomber (seat 1), ordered by ascending entity index.
    REQUIRE(picker.targets().size() == 2u);
    REQUIRE(picker.selected() != nullptr);
    CHECK(picker.selected()->entityIdx == 7u);
    CHECK(picker.selected()->seatIndex == 1);

    picker.next();
    CHECK(picker.selected()->entityIdx == 20u);
    picker.next(); // wraps
    CHECK(picker.selected()->entityIdx == 7u);
    picker.prev(); // wraps back
    CHECK(picker.selected()->entityIdx == 20u);
}

TEST_CASE("CrewSeatMenu: rebuild preserves the current selection when still present (#975)", "[crew][seat-menu]") {
    std::unordered_map<uint32_t, CrewRosterInfo> rosters;
    rosters.emplace(7, bomber(7));
    rosters.emplace(20, bomber(20));

    CrewSeatPicker picker;
    picker.rebuild(rosters);
    picker.next(); // select entity 20
    REQUIRE(picker.selected()->entityIdx == 20u);

    // Re-broadcast (occupancy unchanged) keeps the selection on entity 20.
    picker.rebuild(rosters);
    CHECK(picker.selected()->entityIdx == 20u);

    // If entity 20's seat becomes human-held (no longer joinable), the selection falls back to index 0.
    rosters.at(20).seats[1].occupancy = static_cast<uint8_t>(SeatOccupancy::Human);
    rosters.at(20).seats[1].occupantPeerId = 42;
    picker.rebuild(rosters);
    REQUIRE(picker.targets().size() == 1u);
    CHECK(picker.selected()->entityIdx == 7u);
}

TEST_CASE("CrewSeatMenu: no crewed aircraft = an empty picker (#975)", "[crew][seat-menu]") {
    std::unordered_map<uint32_t, CrewRosterInfo> rosters;
    CrewSeatPicker picker;
    picker.rebuild(rosters);
    CHECK(picker.empty());
    CHECK(picker.selected() == nullptr);
    picker.next(); // no-op
    picker.prev(); // no-op
    CHECK(picker.empty());
}
