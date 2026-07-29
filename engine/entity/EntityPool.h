// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/EntityState.h"

#include <cstdint>
#include <limits>
#include <vector>

namespace fl {

// What a spawn is FOR, as far as the soft cap is concerned (#1049).
//
// The cap is a resource control, and a flat first-come-first-served ceiling gives the wrong answer
// under exactly the conditions it exists for: the things that fill a world fastest (projectiles,
// AI, respawning bots, a runaway mission script) are the things the cap is meant to bound, and once
// they have taken the last slot the next casualty is a HUMAN who cannot join or respawn. So a slice
// of the cap is held back for player airframes and only they may draw on it.
enum class SpawnClass : uint8_t {
    World = 0,  // AI, projectiles, mission objects, effects, parachutes — refused first
    Player = 1, // a pilot's airframe — may draw on the reserved headroom
};

// O(1) alloc/free object pool with generation-counted handles.
//
// Pointer stability: raw pointers returned by get() are invalidated by any alloc() call that
// causes the backing vector to reallocate. Callers must NOT cache raw pointers across spawn()
// calls or tick boundaries. Store EntityId and call get() per use.
//
// Soft cap: if softCap > 0, alloc() returns null() instead of growing once the live count reaches
// the ceiling for that spawn's SpawnClass. 0 means unlimited (the default).
//
//   SpawnClass::Player -> refused at liveCount() == softCap
//   SpawnClass::World  -> refused at liveCount() == softCap - playerReserve  (= worldCap())
//
// playerReserve is clamped to half the cap, so a large max_peers configured against a small cap
// cannot starve the world of every non-player entity. A reserve of 0 (the default) makes both
// tiers the same number, i.e. the flat cap.
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

    // Returns a valid EntityId on success, null() when the soft cap for `cls` is reached.
    EntityId alloc(SpawnClass cls = SpawnClass::World);

    // Marks the slot as free and increments its generation counter.
    // Silently ignores invalid or already-free ids.
    void free(EntityId id);

    // Returns true only if id was produced by alloc() and has not been freed since.
    [[nodiscard]] bool valid(EntityId id) const noexcept;

    // Returns a pointer to the entity state, or nullptr if id is not valid.
    // The pointer is invalidated by the next alloc() that grows the backing store.
    [[nodiscard]] EntityState* get(EntityId id) noexcept;
    [[nodiscard]] const EntityState* get(EntityId id) const noexcept;

    // Returns the live entity state at pool slot `idx` (as yielded by SpatialIndex::queryRadius
    // and EntityState::id.index), or nullptr if the slot is out of range or not alive. Unlike
    // get(EntityId), no generation is required — the caller already holds the current slot index.
    [[nodiscard]] const EntityState* getByIndex(uint32_t idx) const noexcept {
        if (idx >= m_slots.size() || !m_slots[idx].alive)
            return nullptr;
        return &m_slots[idx].state;
    }

    [[nodiscard]] uint32_t liveCount() const noexcept {
        return m_count;
    }
    [[nodiscard]] uint32_t capacity() const noexcept {
        return static_cast<uint32_t>(m_slots.size());
    }
    [[nodiscard]] uint32_t softCap() const noexcept {
        return m_softCap;
    }
    // Headroom inside softCap that only SpawnClass::Player may allocate from (0 when uncapped).
    [[nodiscard]] uint32_t playerReserve() const noexcept {
        return m_playerReserve;
    }
    // The ceiling a SpawnClass::World spawn is refused at (== softCap when there is no reserve).
    [[nodiscard]] uint32_t worldCap() const noexcept {
        return m_worldCap;
    }
    // `playerReserve` is clamped to cap/2; a cap of 0 clears both tiers (unlimited).
    void setSoftCap(uint32_t cap, uint32_t playerReserve = 0) noexcept;

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
    uint32_t m_softCap{0};       // 0 = unlimited
    uint32_t m_playerReserve{0}; // clamped slice of m_softCap only SpawnClass::Player may use
    uint32_t m_worldCap{0};      // m_softCap - m_playerReserve; meaningless when m_softCap == 0
};

} // namespace fl
