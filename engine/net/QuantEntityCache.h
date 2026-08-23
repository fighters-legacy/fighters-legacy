// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "net/RenderEntryFromQuant.h" // the ONE QuantEntity -> EntityRenderEntry mapping (D7)
#include "net/SnapshotScheduler.h"    // kSnapshotRetentionTicks

#include <cstdint>
#include <cstring>
#include <unordered_map>

namespace fl {

// The snapshot entity cache, shared by live play and replay playback (#1252).
//
// The live client and ReplayPlayer ran the same machine twice: decode a record, refresh the
// delta baseline from a full record or backfill gen/typeIndex/factionIndex from it, prime the
// articulation channels from the previous frame, build the render entry, and age out anything the
// stream stopped mentioning. The two copies were verbatim down to the memcpy.
//
// D7's premise is that the renderer cannot tell a replay from a live session. That only holds if
// both paths build the same entry from the same bits -- RenderEntryFromQuant.h already guaranteed
// the mapping, and this guarantees the CACHE LOGIC around it, which is where the state that feeds
// the mapping actually lives.
//
// Header-only, like RenderEntryFromQuant.h beside it and for the same reason: it adds no link edge
// for either caller.
class QuantEntityCache {
  public:
    // The delta baseline. Only these three fields are ever read back out, which is why the cache
    // stores them rather than the whole QuantEntity the replay copy used to keep.
    struct Baseline {
        uint16_t gen{0};
        uint32_t typeIndex{0};
        uint16_t factionIndex{0};
    };

    struct Cached {
        EntityRenderEntry re;
        uint64_t lastSeenTick{0};
    };

    // Fold one decoded record in. `qe` is completed in place from the baseline when the record is a
    // delta. Returns false when the record cannot be rendered and should be skipped:
    //
    //   * a delta with no baseline -- live, that means the full record was dropped and the entity
    //     reappears on the next baseline tick; in a replay it means a damaged file;
    //   * a delta whose generation disagrees with the baseline, when `requireGenMatch`. The live
    //     client checks this because a stale record can arrive for a recycled pool slot. A replay
    //     is a single ordered stream with no such race, so it passes false and keeps its own
    //     historical behaviour rather than acquiring a check it cannot need.
    //
    // A delta that CARRIES a generation refreshes the baseline with it. That only ever changes
    // anything for the replay path -- with `requireGenMatch` a disagreeing generation has already
    // been rejected above, and an agreeing one writes back what was there. It matters because the
    // replay copy this replaces stored the whole completed record back into its baseline, and
    // dropping that silently would have changed how a damaged file replays.
    [[nodiscard]] bool applyRecord(QuantEntity& qe, bool genPresent, uint64_t tick, bool requireGenMatch) {
        const auto kit = m_baselines.find(qe.idx);
        if (qe.isFull) {
            m_baselines[qe.idx] = {static_cast<uint16_t>(qe.gen), qe.typeIndex, qe.factionIndex};
        } else {
            if (kit == m_baselines.end())
                return false;
            if (requireGenMatch && genPresent && static_cast<uint16_t>(qe.gen) != kit->second.gen)
                return false;
            if (genPresent)
                kit->second.gen = static_cast<uint16_t>(qe.gen); // the wire is newer than the baseline
            else
                qe.gen = kit->second.gen;
            qe.typeIndex = kit->second.typeIndex;
            qe.factionIndex = kit->second.factionIndex;
        }

        EntityRenderEntry re;
        // Articulation rides its own TLV (#843) and updates on CHANGE, so a record decode must not
        // wipe it: prime from the cache first, since renderEntryFromQuant leaves it alone.
        if (const auto cached = m_entities.find(qe.idx); cached != m_entities.end())
            std::memcpy(re.artChannels, cached->second.re.artChannels, sizeof(re.artChannels));
        renderEntryFromQuant(qe, re);
        m_entities[qe.idx] = {re, tick};
        return true;
    }

    // An explicit despawn. Both maps go together -- leaving a baseline behind would let a later
    // delta for a recycled slot resurrect the entity.
    void despawn(uint32_t idx) {
        m_entities.erase(idx);
        m_baselines.erase(idx);
    }

    // Evict anything not refreshed within the retention window: the backstop for interest-out and
    // dropped despawn packets live, and for a damaged file in a replay.
    void ageOut(uint64_t tick) {
        for (auto it = m_entities.begin(); it != m_entities.end();) {
            const uint64_t age = (tick >= it->second.lastSeenTick) ? (tick - it->second.lastSeenTick) : 0u;
            if (age > kSnapshotRetentionTicks) {
                m_baselines.erase(it->first);
                it = m_entities.erase(it);
            } else {
                ++it;
            }
        }
    }

    void clear() {
        m_entities.clear();
        m_baselines.clear();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return m_entities.size();
    }

    // Mutable access for the articulation TLV, which writes channels straight into the cached entry.
    [[nodiscard]] Cached* find(uint32_t idx) {
        const auto it = m_entities.find(idx);
        return it == m_entities.end() ? nullptr : &it->second;
    }

    // The delta baseline, for callers that need a known entity's generation without its render
    // entry (the datalink track picture resolves a subject index this way).
    [[nodiscard]] const Baseline* baseline(uint32_t idx) const {
        const auto it = m_baselines.find(idx);
        return it == m_baselines.end() ? nullptr : &it->second;
    }

    // Iteration for snapshot assembly.
    [[nodiscard]] auto begin() const noexcept {
        return m_entities.begin();
    }
    [[nodiscard]] auto end() const noexcept {
        return m_entities.end();
    }

  private:
    std::unordered_map<uint32_t, Baseline> m_baselines;
    std::unordered_map<uint32_t, Cached> m_entities;
};

} // namespace fl
