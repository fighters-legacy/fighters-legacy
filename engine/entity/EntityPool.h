// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/EntityState.h"

#include <cstdint>
#include <limits>
#include <vector>

namespace fl {

// O(1) alloc/free object pool with generation-counted handles.
//
// Pointer stability: raw pointers returned by get() are invalidated by any alloc() call that
// causes the backing vector to reallocate. Callers must NOT cache raw pointers across spawn()
// calls or tick boundaries. Store EntityId and call get() per use.
//
// Soft cap: if softCap > 0, alloc() returns null() when liveCount() == softCap instead of
// growing. 0 means unlimited.
//
// Iteration: forEach() walks a dense list of live slot indices, so it is O(liveCount), NOT
// O(capacity) — dead slots left behind by high spawn/reap churn (e.g. projectiles) cost nothing.
// The iteration ORDER is free-history-dependent (free() does an O(1) swap-remove on the live-index
// list), so every consumer must be order-independent. Today they all are: the SpatialIndex insert
// is order-free, the WorldBroadcaster snapshot map is keyed by entity index with the visible set
// sorted, the render snapshot is a flat list re-sorted by the renderer, and Lua / AI-factory lookups
// search by index. This is guarded by the test_world_broadcaster serial-equivalence + TSan tests.
//
// Threading: all methods are sim-thread-only.
class EntityPool {
  public:
    explicit EntityPool(uint32_t initialCapacity = 256);

    // Returns a valid EntityId on success, null() when the soft cap is reached.
    EntityId alloc();

    // Marks the slot as free and increments its generation counter.
    // Silently ignores invalid or already-free ids.
    void free(EntityId id);

    // Returns true only if id was produced by alloc() and has not been freed since.
    [[nodiscard]] bool valid(EntityId id) const noexcept;

    // Returns a pointer to the entity state, or nullptr if id is not valid.
    // The pointer is invalidated by the next alloc() that grows the backing store.
    [[nodiscard]] EntityState* get(EntityId id) noexcept;
    [[nodiscard]] const EntityState* get(EntityId id) const noexcept;

    [[nodiscard]] uint32_t liveCount() const noexcept {
        return m_count;
    }
    [[nodiscard]] uint32_t capacity() const noexcept {
        return static_cast<uint32_t>(m_slots.size());
    }
    [[nodiscard]] uint32_t softCap() const noexcept {
        return m_softCap;
    }
    void setSoftCap(uint32_t cap) noexcept {
        m_softCap = cap;
    }

    // Visits every live entity. Fn signature: void(EntityState&) or void(const EntityState&).
    // O(liveCount): iterates the dense live-index list, not the (possibly sparse) slot vector.
    // Order is free-history-dependent — see the class comment; consumers must be order-independent.
    template <typename Fn> void forEach(Fn&& fn) {
        for (uint32_t idx : m_liveIndices)
            fn(m_slots[idx].state);
    }

    template <typename Fn> void forEach(Fn&& fn) const {
        for (uint32_t idx : m_liveIndices)
            fn(m_slots[idx].state);
    }

  private:
    static constexpr uint32_t kNull = std::numeric_limits<uint32_t>::max();

    struct Slot {
        EntityState state;
        uint32_t generation{0}; // 0 = never allocated; increments on each free()
        uint32_t nextFree{kNull};
        uint32_t livePos{kNull}; // index into m_liveIndices while alive; kNull when free
        bool alive{false};
    };

    std::vector<Slot> m_slots;
    std::vector<uint32_t> m_liveIndices; // dense list of live slot indices (drives O(liveCount) forEach)
    uint32_t m_freeHead{kNull};
    uint32_t m_count{0};
    uint32_t m_softCap{0};
};

} // namespace fl
