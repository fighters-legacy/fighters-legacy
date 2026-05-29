// SPDX-License-Identifier: GPL-3.0-or-later
#include "entity/EntityPool.h"

#include <limits>

namespace fl {

EntityPool::EntityPool(uint32_t initialCapacity) {
    m_slots.reserve(initialCapacity);
}

EntityId EntityPool::alloc() {
    if (m_softCap > 0 && m_count >= m_softCap)
        return EntityId::null();

    uint32_t index;
    if (m_freeHead != kNull) {
        // Reuse a freed slot
        index = m_freeHead;
        m_freeHead = m_slots[index].nextFree;
    } else {
        // Grow the backing store
        index = static_cast<uint32_t>(m_slots.size());
        m_slots.emplace_back();
    }

    Slot& slot = m_slots[index];
    slot.generation = (slot.generation == 0) ? 1 : slot.generation; // keep non-zero
    slot.alive = true;
    slot.nextFree = kNull;
    slot.state = EntityState{};
    slot.state.id = {index, slot.generation};

    ++m_count;
    return slot.state.id;
}

void EntityPool::free(EntityId id) {
    if (id.index >= m_slots.size())
        return;
    Slot& slot = m_slots[id.index];
    if (!slot.alive || slot.generation != id.generation)
        return;

    slot.alive = false;
    ++slot.generation;
    if (slot.generation == 0)
        slot.generation = 1; // skip 0 so generation==0 always means "never used"
    slot.nextFree = m_freeHead;
    m_freeHead = id.index;
    --m_count;
}

bool EntityPool::valid(EntityId id) const noexcept {
    if (!id.valid() || id.index >= m_slots.size())
        return false;
    const Slot& slot = m_slots[id.index];
    return slot.alive && slot.generation == id.generation;
}

EntityState* EntityPool::get(EntityId id) noexcept {
    if (id.index >= m_slots.size())
        return nullptr;
    Slot& slot = m_slots[id.index];
    if (!slot.alive || slot.generation != id.generation)
        return nullptr;
    return &slot.state;
}

const EntityState* EntityPool::get(EntityId id) const noexcept {
    if (id.index >= m_slots.size())
        return nullptr;
    const Slot& slot = m_slots[id.index];
    if (!slot.alive || slot.generation != id.generation)
        return nullptr;
    return &slot.state;
}

} // namespace fl
