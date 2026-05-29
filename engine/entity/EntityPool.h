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
    template <typename Fn> void forEach(Fn&& fn) {
        for (auto& slot : m_slots) {
            if (slot.alive)
                fn(slot.state);
        }
    }

    template <typename Fn> void forEach(Fn&& fn) const {
        for (const auto& slot : m_slots) {
            if (slot.alive)
                fn(slot.state);
        }
    }

  private:
    static constexpr uint32_t kNull = std::numeric_limits<uint32_t>::max();

    struct Slot {
        EntityState state;
        uint32_t generation{0}; // 0 = never allocated; increments on each free()
        uint32_t nextFree{kNull};
        bool alive{false};
    };

    std::vector<Slot> m_slots;
    uint32_t m_freeHead{kNull};
    uint32_t m_count{0};
    uint32_t m_softCap{0};
};

} // namespace fl
