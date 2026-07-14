// SPDX-License-Identifier: GPL-3.0-or-later
#include "weapon/WeaponRegistry.h"

#include <limits>

namespace fl {

uint32_t WeaponRegistry::registerWeapon(WeaponDef def) {
    if (m_index.count(def.id))
        return std::numeric_limits<uint32_t>::max();

    uint32_t index = static_cast<uint32_t>(m_defs.size());
    m_index.emplace(def.id, index);
    m_defs.push_back(std::move(def));
    return index;
}

const WeaponDef* WeaponRegistry::findById(const char* id) const noexcept {
    auto it = m_index.find(id);
    if (it == m_index.end())
        return nullptr;
    return &m_defs[it->second];
}

uint32_t WeaponRegistry::indexById(const char* id) const noexcept {
    auto it = m_index.find(id);
    if (it == m_index.end())
        return std::numeric_limits<uint32_t>::max();
    return it->second;
}

const WeaponDef* WeaponRegistry::byIndex(uint32_t index) const noexcept {
    if (index >= m_defs.size())
        return nullptr;
    return &m_defs[index];
}

void WeaponRegistry::clear() {
    m_defs.clear();
    m_index.clear();
}

} // namespace fl
