// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <span>

namespace fl {

// One team's live occupancy for balancing (#522). factionIndex is the FactionRegistry index the team
// maps onto; capacity 0 = unlimited; count is the current live player count on the team.
struct TeamState {
    uint16_t factionIndex{0};
    int capacity{0};
    int count{0};
};

// Pick the team a joining player should be assigned to: the one with the smallest live count that
// still has room, breaking ties by the lower factionIndex for determinism. Returns nullopt when every
// team is full (the caller then refuses the connection with ConnectRefusalCode::MatchFull). Pure.
inline std::optional<uint16_t> pickTeam(std::span<const TeamState> teams) {
    const TeamState* best = nullptr;
    for (const TeamState& t : teams) {
        const bool hasRoom = (t.capacity == 0) || (t.count < t.capacity);
        if (!hasRoom)
            continue;
        if (!best || t.count < best->count || (t.count == best->count && t.factionIndex < best->factionIndex))
            best = &t;
    }
    if (!best)
        return std::nullopt;
    return best->factionIndex;
}

// May a player move from team `from` to team `to` without unbalancing the match? Allowed only when the
// destination has room and moving there would not make it larger than the source was before the move
// (i.e. the switch does not create a bigger imbalance than already exists). Pure.
inline bool switchAllowed(const TeamState& from, const TeamState& to) {
    if (from.factionIndex == to.factionIndex)
        return false; // already there
    const bool hasRoom = (to.capacity == 0) || (to.count < to.capacity);
    if (!hasRoom)
        return false;
    // After the move: to.count+1 vs from.count-1. Disallow when it would leave `to` more than one ahead
    // of `from` — the standard "no stacking" rule; an even or self-correcting move is fine.
    return (to.count + 1) <= (from.count - 1) + 1;
}

} // namespace fl
