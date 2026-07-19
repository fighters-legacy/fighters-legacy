// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "CrewClientState.h"
#include "entity/CrewDef.h"   // CrewCapability / hasCapability
#include "net/GameProtocol.h" // SeatOccupancy

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace fl {

// ── Crew seat picker (#966/#975) ─────────────────────────────────────────────
//
// Roster-driven seat selection, built on the EntitySelector / WingmanMenu precedent: PURE logic over
// the client's crew rosters (no SDL, no rendering), so it is unit-testable and the SDL layer only
// drives key events into it. It flattens every JOINABLE seat across all known crewed aircraft — a
// non-fly seat that is Empty or Bot-held (a human never displaces another human, so human-held seats
// are not offered) — into a cyclable list, and produces the {entity, seat} a MsgSeatRequest targets.

// The seat index `peerId` occupies in `roster`, or -1 if it occupies none. Drives the per-seat camera
// + HUD (which seat am I?).
[[nodiscard]] inline int occupiedSeat(const CrewRosterInfo& roster, uint32_t peerId) noexcept {
    for (const CrewSeatInfo& s : roster.seats)
        if (s.occupancy == static_cast<uint8_t>(SeatOccupancy::Human) && s.occupantPeerId == peerId)
            return static_cast<int>(s.seatIndex);
    return -1;
}

// Whether seat `seatIdx` of `roster` is the Fly seat (owns the flight controls). Out-of-range = false.
// The client runs flight prediction / attitude only for the Fly seat (#975 acceptance).
[[nodiscard]] inline bool seatIsFly(const CrewRosterInfo& roster, int seatIdx) noexcept {
    for (const CrewSeatInfo& s : roster.seats)
        if (static_cast<int>(s.seatIndex) == seatIdx)
            return hasCapability(s.capabilities, CrewCapability::Fly);
    return false;
}

// Whether a seat can be JOINED by a human: a non-fly seat that is not currently human-held. (The Fly
// seat belongs to the owning pilot; a human never displaces another human — the server enforces both.)
[[nodiscard]] inline bool seatJoinable(const CrewSeatInfo& s) noexcept {
    if (hasCapability(s.capabilities, CrewCapability::Fly))
        return false;
    return s.occupancy != static_cast<uint8_t>(SeatOccupancy::Human);
}

// One joinable seat, flattened for the picker.
struct SeatTarget {
    uint32_t entityIdx{0};
    uint32_t entityGen{0};
    uint8_t seatIndex{0};
    std::string role; // the seat's display role (localizable label)
};

class CrewSeatPicker {
  public:
    // Rebuild the joinable-seat list from the client's rosters, preserving the current selection when it
    // is still present. Iterated in a stable order (ascending entity index, then seat index) so cycling
    // is deterministic regardless of the map's iteration order.
    void rebuild(const std::unordered_map<uint32_t, CrewRosterInfo>& rosters) {
        // Remember the current pick to restore it.
        SeatTarget prev{};
        const bool had = m_sel < m_targets.size();
        if (had)
            prev = m_targets[m_sel];

        m_targets.clear();
        std::vector<uint32_t> idxs;
        idxs.reserve(rosters.size());
        for (const auto& [idx, roster] : rosters)
            idxs.push_back(idx);
        std::sort(idxs.begin(), idxs.end());
        for (uint32_t idx : idxs) {
            const CrewRosterInfo& roster = rosters.at(idx);
            for (const CrewSeatInfo& s : roster.seats) {
                if (!seatJoinable(s))
                    continue;
                m_targets.push_back({roster.entityIdx, roster.entityGen, s.seatIndex, s.role});
            }
        }

        m_sel = 0;
        if (had) {
            for (std::size_t i = 0; i < m_targets.size(); ++i)
                if (m_targets[i].entityIdx == prev.entityIdx && m_targets[i].entityGen == prev.entityGen &&
                    m_targets[i].seatIndex == prev.seatIndex) {
                    m_sel = i;
                    break;
                }
        }
    }

    void next() noexcept {
        if (!m_targets.empty())
            m_sel = (m_sel + 1) % m_targets.size();
    }
    void prev() noexcept {
        if (!m_targets.empty())
            m_sel = (m_sel + m_targets.size() - 1) % m_targets.size();
    }

    [[nodiscard]] bool empty() const noexcept {
        return m_targets.empty();
    }
    [[nodiscard]] std::span<const SeatTarget> targets() const noexcept {
        return {m_targets.data(), m_targets.size()};
    }
    [[nodiscard]] const SeatTarget* selected() const noexcept {
        return m_sel < m_targets.size() ? &m_targets[m_sel] : nullptr;
    }
    [[nodiscard]] std::size_t selectedIndex() const noexcept {
        return m_sel;
    }

  private:
    std::vector<SeatTarget> m_targets;
    std::size_t m_sel{0};
};

} // namespace fl
