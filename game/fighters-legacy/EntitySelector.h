// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "render/RenderSnapshot.h"

#include <cstdint>
#include <span>

namespace fl {

// Observer entity selection (#860): tracks which live entity a spectator has picked to view from, and
// cycles the selection through the entities present in the current render snapshot. Pure logic over
// EntityRenderEntry (no SDL / no rendering), so it is unit-testable and reusable for the Game-Master
// map (#861).
//
// Cycling is stable regardless of snapshot ordering: it walks entities in ascending entityIdx order and
// wraps. The selection is an {idx, gen} handle so a stale pick (the entity died and its pool slot was
// reused by a different entity) resolves to nullptr rather than silently viewing the wrong aircraft --
// the caller degrades to the free camera.
class EntitySelector {
  public:
    // Advance the selection to the next / previous entity in ascending-entityIdx order, wrapping. If
    // nothing is selected (or the current selection is gone), next() picks the lowest-idx entity and
    // prev() the highest. A no-op when there are no entities.
    void cycleNext(std::span<const EntityRenderEntry> entries) noexcept {
        cycle(entries, +1);
    }
    void cyclePrev(std::span<const EntityRenderEntry> entries) noexcept {
        cycle(entries, -1);
    }

    // The selected entry within this snapshot, or nullptr when nothing is selected or the selected
    // entity is no longer present (destroyed / interest-out / gen mismatch).
    [[nodiscard]] const EntityRenderEntry* resolve(std::span<const EntityRenderEntry> entries) const noexcept {
        if (m_gen == 0)
            return nullptr;
        for (const auto& e : entries)
            if (e.entityIdx == m_idx && e.entityGen == m_gen)
                return &e;
        return nullptr;
    }

    void select(uint32_t idx, uint32_t gen) noexcept {
        m_idx = idx;
        m_gen = gen;
    }
    void clear() noexcept {
        m_idx = 0;
        m_gen = 0;
    }
    [[nodiscard]] bool hasSelection() const noexcept {
        return m_gen != 0;
    }
    [[nodiscard]] uint32_t selectedIdx() const noexcept {
        return m_idx;
    }
    [[nodiscard]] uint32_t selectedGen() const noexcept {
        return m_gen;
    }

  private:
    void cycle(std::span<const EntityRenderEntry> entries, int dir) noexcept {
        const EntityRenderEntry* best = nullptr;    // the entry we will land on
        const EntityRenderEntry* extreme = nullptr; // lowest (next) or highest (prev) idx overall, for wrap
        const bool haveCur = m_gen != 0;
        for (const auto& e : entries) {
            if (e.entityGen == 0)
                continue; // never select an invalid handle
            // Track the wrap target: the global min (next) or max (prev).
            if (!extreme || (dir > 0 ? e.entityIdx < extreme->entityIdx : e.entityIdx > extreme->entityIdx))
                extreme = &e;
            // Track the nearest entity strictly beyond the current one in the cycle direction.
            if (haveCur) {
                const bool beyond = dir > 0 ? e.entityIdx > m_idx : e.entityIdx < m_idx;
                if (beyond && (!best || (dir > 0 ? e.entityIdx < best->entityIdx : e.entityIdx > best->entityIdx)))
                    best = &e;
            }
        }
        const EntityRenderEntry* pick = best ? best : extreme; // wrap when nothing is beyond the current
        if (pick) {
            m_idx = pick->entityIdx;
            m_gen = pick->entityGen;
        }
    }

    uint32_t m_idx{0};
    uint32_t m_gen{0};
};

} // namespace fl
