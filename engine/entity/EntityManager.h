// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/EntityEvent.h"
#include "entity/EntityPool.h"
#include "entity/EntityTypeRegistry.h"
#include "loop/ISimUpdate.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace fl {
class ILogger;
class SimRenderBridge;
} // namespace fl

namespace fl {

// Central entity subsystem. Owns the object pool and dispatches per-tick housekeeping.
//
// Threading model:
//   Sim thread  — onTick(), spawn(), kill(), applyDamage(), get(), forEach(), setSoftCap()
//   Main thread — liveCount() + softCapRefusals() (atomic snapshots), setSoftCap(),
//                 addEventHandler(), removeEventHandler() (the last two must be called BEFORE
//                 GameLoop::start(); setSoftCap() is main-thread before start and sim-thread after,
//                 i.e. from an enqueueSimCallback body — see its declaration)
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

    // Spawns an entity of the given type. Returns null() if typeId is not registered or the soft cap
    // for `cls` is reached. A cap refusal is counted (softCapRefusals()) and reported by the periodic
    // Warn in onTick() — a spawn that silently returns null is undiagnosable from an operator's seat,
    // which is what made the unwired cap (#1049) survive as long as it did.
    EntityId spawn(const char* typeId, const EntityTransform& transform, uint32_t ownerId = 0,
                   SpawnClass cls = SpawnClass::World);

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

    // Returns the live entity state at pool slot `idx` (the index yielded by
    // SpatialIndex::queryRadius / EntityState::id.index), or nullptr if the slot is out of range
    // or not alive. Lets AI range conditions look up a candidate's state without a generation.
    [[nodiscard]] const EntityState* getByIndex(uint32_t idx) const noexcept;

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

    // Propagates to EntityPool. 0 = unlimited; `playerReserve` is the headroom inside the cap that
    // only SpawnClass::Player spawns may use (clamped to cap/2 — see EntityPool.h).
    //
    // Safe from the main thread before GameLoop::start(), and from the sim thread afterwards (an
    // enqueueSimCallback body — this is how `reload_config` re-applies world.entity_soft_cap).
    void setSoftCap(uint32_t cap, uint32_t playerReserve = 0) noexcept;

    [[nodiscard]] uint32_t softCap() const noexcept;
    [[nodiscard]] uint32_t playerReserve() const noexcept;

    // Total spawns refused by the soft cap since startup. Monotonic; safe from any thread. A
    // non-zero value is the operator-visible evidence that the cap is doing something.
    [[nodiscard]] uint64_t softCapRefusals() const noexcept;

    // Attach a render bridge. Must be called before GameLoop::start().
    // When set, onTick() publishes an EntityRenderEntry snapshot after each tick.
    // Pass nullptr to detach (renders silently skip publishing).
    void setRenderBridge(SimRenderBridge* bridge) noexcept;

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
    std::atomic<uint64_t> m_softCapRefusals{0}; // all-time; read from any thread (status/metrics)
    uint32_t m_refusalsSinceLog{0};             // sim thread only; drained by the periodic Warn
    uint64_t m_nextCapLogTick{0};               // sim thread only; earliest tick the next Warn may fire
    std::vector<EntityId> m_pendingKill;
    SimRenderBridge* m_renderBridge{nullptr}; // optional; set before GameLoop::start()
};

} // namespace fl
