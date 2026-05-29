// SPDX-License-Identifier: GPL-3.0-or-later
#include "entity/EntityManager.h"

#include "ILogger.h"

#include <algorithm>
#include <atomic>
#include <limits>

static_assert(std::atomic<uint32_t>::is_always_lock_free,
              "EntityManager requires lock-free uint32_t atomics on this platform");

namespace fl {

EntityManager::EntityManager(ILogger& logger, EntityTypeRegistry& registry) : m_logger(logger), m_registry(registry) {}

// ── ISimUpdate ────────────────────────────────────────────────────────────────

void EntityManager::onTick(double /*simDt*/, uint64_t /*tickIndex*/) {
    // Phase 2.2: housekeeping only. Per-entity physics / AI advance added in later workstreams.
    reapDeadEntities();
    m_liveCount.store(m_pool.liveCount(), std::memory_order_release);
}

// ── entity lifecycle ──────────────────────────────────────────────────────────

EntityId EntityManager::spawn(const char* typeId, const EntityTransform& transform, uint32_t ownerId) {
    uint32_t typeIndex = m_registry.indexById(typeId);
    if (typeIndex == std::numeric_limits<uint32_t>::max()) {
        m_logger.log(LogLevel::Warn, __FILE__, __LINE__,
                     (std::string("EntityManager::spawn: unknown type '") + typeId + "'").c_str());
        return EntityId::null();
    }

    EntityId id = m_pool.alloc();
    if (!id.valid())
        return EntityId::null();

    const EntityDef& def = *m_registry.byIndex(typeIndex);
    EntityState* s = m_pool.get(id);

    s->typeIndex = typeIndex;
    s->transform = transform;
    s->maxHp = def.maxHp;
    s->hp = def.maxHp;
    s->damageLevel = DamageLevel::Intact;
    s->dead = false;
    s->playerOwned = false;
    s->ownerId = ownerId;

    return id;
}

void EntityManager::kill(EntityId id, EntityId instigator) {
    EntityState* s = m_pool.get(id);
    if (!s || s->dead)
        return;

    s->dead = true;

    EntityEvent ev{};
    ev.type = EntityEventType::Died;
    ev.subject = id;
    ev.instigator = instigator;
    fireEvent(ev);

    if (instigator.valid()) {
        EntityEvent score{};
        score.type = EntityEventType::ScoreAwarded;
        score.subject = id;
        score.instigator = instigator;
        score.score = 1;
        fireEvent(score);
    }

    m_pendingKill.push_back(id);
}

void EntityManager::applyDamage(EntityId id, float amount, EntityId instigator) {
    EntityState* s = m_pool.get(id);
    if (!s || s->dead)
        return;

    DamageLevel prev = s->damageLevel;
    s->hp = s->hp - amount;
    if (s->hp < 0.f)
        s->hp = 0.f;

    if (s->hp <= 0.f) {
        kill(id, instigator);
        return;
    }

    const EntityDef* def = m_registry.byIndex(s->typeIndex);
    if (def && def->damage) {
        float fraction = (s->maxHp > 0.f) ? (s->hp / s->maxHp) : 0.f;
        s->damageLevel = evaluateDamageLevel(*def->damage, fraction);
        evaluateAndFireDamageEvents(*s, prev, instigator);
    }
}

// ── state access ──────────────────────────────────────────────────────────────

EntityState* EntityManager::get(EntityId id) noexcept {
    return m_pool.get(id);
}

const EntityState* EntityManager::get(EntityId id) const noexcept {
    return m_pool.get(id);
}

// ── configuration ─────────────────────────────────────────────────────────────

void EntityManager::addEventHandler(IEntityEventHandler* handler) {
    if (handler)
        m_handlers.push_back(handler);
}

void EntityManager::removeEventHandler(IEntityEventHandler* handler) {
    m_handlers.erase(std::remove(m_handlers.begin(), m_handlers.end(), handler), m_handlers.end());
}

void EntityManager::setSoftCap(uint32_t cap) noexcept {
    m_pool.setSoftCap(cap);
}

uint32_t EntityManager::liveCount() const noexcept {
    return m_liveCount.load(std::memory_order_acquire);
}

// ── private helpers ───────────────────────────────────────────────────────────

void EntityManager::evaluateAndFireDamageEvents(EntityState& state, DamageLevel prevLevel, EntityId instigator) {
    if (state.damageLevel == prevLevel)
        return;

    EntityEvent ev{};
    ev.type = EntityEventType::DamageLevelChanged;
    ev.subject = state.id;
    ev.instigator = instigator;
    ev.newDamageLevel = state.damageLevel;
    fireEvent(ev);
}

void EntityManager::fireEvent(const EntityEvent& event) {
    for (auto* handler : m_handlers)
        handler->onEntityEvent(event);
}

void EntityManager::reapDeadEntities() {
    for (EntityId id : m_pendingKill)
        m_pool.free(id);
    m_pendingKill.clear();
}

} // namespace fl
