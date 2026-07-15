// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <unordered_map>

#include <glm/vec3.hpp>

namespace fl {

// Rolling per-tick position history for server-side lag compensation (#425). The server records
// every live entity's post-integrate position each tick; a rewound hitscan then tests the world a
// player actually SAW — `currentTick − estimatedDelayTicks` — instead of the world as it is now,
// so leading a target by your own latency is not required to land a gun hit.
//
// A per-tick map of the whole world, not a per-entity ring: the query side wants "where was
// EVERYTHING at tick T", the recorder wants one clear+insert sweep, and despawn GC is free — a
// dead entity simply stops being recorded and its old entries age out as the ring wraps. The
// generation travels with each entry so a recycled pool slot can never be hit through history:
// the entity that occupied that index at the rewound tick is not the entity living there now.
//
// Sim-thread only, like everything WorldBroadcaster owns. Memory: kHistoryTicks × liveEntities
// map entries (~32 B each) — ~1 MB at 1000 entities, bounded and flat.
class TransformHistory {
  public:
    static constexpr uint32_t kHistoryTicks = 32; // ≈533 ms at 60 Hz — the rewind clamp bound

    struct Entry {
        glm::dvec3 pos{};
        uint16_t gen{0};
    };

    // Begin recording `tick`: clears the slot the ring reuses. Call once per tick, then add()
    // every live entity.
    void beginTick(uint64_t tick) {
        Slot& s = m_slots[tick % kHistoryTicks];
        s.tick = tick;
        s.valid = true;
        s.entries.clear();
    }

    void add(uint64_t tick, uint32_t idx, uint16_t gen, const double pos[3]) {
        Slot& s = m_slots[tick % kHistoryTicks];
        if (!s.valid || s.tick != tick)
            return; // beginTick was not called for this tick — refuse to poison another slot
        s.entries.emplace(idx, Entry{{pos[0], pos[1], pos[2]}, gen});
    }

    // Where was entity `idx` at `tick`? Empty when the tick has aged out of the ring, was never
    // recorded, the entity did not exist then, or the pool slot held a DIFFERENT generation —
    // history must never resolve onto a reused index.
    [[nodiscard]] std::optional<glm::dvec3> queryAt(uint64_t tick, uint32_t idx, uint16_t gen) const {
        const Slot& s = m_slots[tick % kHistoryTicks];
        if (!s.valid || s.tick != tick)
            return std::nullopt;
        const auto it = s.entries.find(idx);
        if (it == s.entries.end() || it->second.gen != gen)
            return std::nullopt;
        return it->second.pos;
    }

    // True when `tick` is resident in the ring (recorded and not yet overwritten).
    [[nodiscard]] bool hasTick(uint64_t tick) const noexcept {
        const Slot& s = m_slots[tick % kHistoryTicks];
        return s.valid && s.tick == tick;
    }

    void clear() noexcept {
        for (Slot& s : m_slots) {
            s.valid = false;
            s.entries.clear();
        }
    }

  private:
    struct Slot {
        uint64_t tick{0};
        bool valid{false};
        std::unordered_map<uint32_t, Entry> entries;
    };
    Slot m_slots[kHistoryTicks];
};

} // namespace fl
