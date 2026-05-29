// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/EntityEvent.h"
#include "entity/EntityPool.h"
#include "entity/EntityTypeRegistry.h"
#include "loop/ISimUpdate.h"

#include <atomic>
#include <cstdint>
#include <vector>

class ILogger;

namespace fl {

// Central entity subsystem. Owns the object pool and dispatches per-tick housekeeping.
//
// Threading model:
//   Sim thread  — onTick(), spawn(), kill(), applyDamage(), get(), forEach()
//   Main thread — liveCount() (atomic snapshot), setSoftCap(), addEventHandler(),
//                 removeEventHandler() (must be called BEFORE GameLoop::start())
//
// Event handlers are registered before GameLoop::start() and never mutated while the sim
// thread is running. Callbacks fire on the sim thread; do not call HAL methods (except
// ILogger::log) from within onEntityEvent().
class EntityManager : public ISimUpdate {
  public:
    EntityManager(ILogger& logger, EntityTypeRegistry& registry);
    ~EntityManager() override = default;

    // ISimUpdate — called once per fixed sim tick on the sim thread.
    // Phase 2.2: housekeeping only (process kills, reap dead slots, update live count).
    // Per-entity physics / AI advance is added in later workstreams.
    void onTick(double simDt, uint64_t tickIndex) override;

    // ── entity lifecycle (sim thread) ────────────────────────────────────────

    // Spawns an entity of the given type. Returns null() if typeId is not registered
    // or the soft cap is reached.
    EntityId spawn(const char* typeId, const EntityTransform& transform, uint32_t ownerId = 0);

    // Marks the entity dead and queues it for reaping at the end of the current tick.
    // Fires a Died event (and ScoreAwarded to instigator's owner if instigator is valid).
    void kill(EntityId id, EntityId instigator = EntityId::null());

    // Reduces entity HP by amount. Evaluates damage level thresholds and fires events.
    // No-ops on invalid or already-dead entities.
    void applyDamage(EntityId id, float amount, EntityId instigator = EntityId::null());

    // ── state access (sim thread) ─────────────────────────────────────────────

    // Returns nullptr if id is not valid. Pointer valid only until the next alloc().
    [[nodiscard]] EntityState* get(EntityId id) noexcept;
    [[nodiscard]] const EntityState* get(EntityId id) const noexcept;

    // Visits every live entity. Fn: void(EntityState&) or void(const EntityState&).
    template <typename Fn> void forEach(Fn&& fn) {
        m_pool.forEach(std::forward<Fn>(fn));
    }
    template <typename Fn> void forEach(Fn&& fn) const {
        m_pool.forEach(std::forward<Fn>(fn));
    }

    // ── configuration (main thread, before GameLoop::start()) ─────────────────

    void addEventHandler(IEntityEventHandler* handler);
    void removeEventHandler(IEntityEventHandler* handler);

    // Propagates to EntityPool. 0 = unlimited.
    void setSoftCap(uint32_t cap) noexcept;

    // ── thread-safe snapshot ──────────────────────────────────────────────────

    // Safe to call from the main thread. Updated at end of each onTick().
    [[nodiscard]] uint32_t liveCount() const noexcept;

  private:
    void evaluateAndFireDamageEvents(EntityState& state, DamageLevel prevLevel, EntityId instigator);
    void fireEvent(const EntityEvent& event);
    void reapDeadEntities();

    ILogger& m_logger;
    EntityTypeRegistry& m_registry;
    EntityPool m_pool;
    std::vector<IEntityEventHandler*> m_handlers;
    std::atomic<uint32_t> m_liveCount{0};
    std::vector<EntityId> m_pendingKill;
};

} // namespace fl
