// SPDX-License-Identifier: GPL-3.0-or-later
#include "entity/EntityTypeRegistry.h"

#include <limits>

namespace fl {

uint32_t EntityTypeRegistry::registerType(EntityDef def) {
    if (m_index.count(def.id))
        return std::numeric_limits<uint32_t>::max();

    uint32_t index = static_cast<uint32_t>(m_defs.size());
    m_index.emplace(def.id, index);
    m_defs.push_back(std::move(def));
    return index;
}

const EntityDef* EntityTypeRegistry::findById(const char* id) const noexcept {
    auto it = m_index.find(id);
    if (it == m_index.end())
        return nullptr;
    return &m_defs[it->second];
}

uint32_t EntityTypeRegistry::indexById(const char* id) const noexcept {
    auto it = m_index.find(id);
    if (it == m_index.end())
        return std::numeric_limits<uint32_t>::max();
    return it->second;
}

const EntityDef* EntityTypeRegistry::byIndex(uint32_t index) const noexcept {
    if (index >= m_defs.size())
        return nullptr;
    return &m_defs[index];
}

void EntityTypeRegistry::clear() {
    m_defs.clear();
    m_index.clear();
}

} // namespace fl
