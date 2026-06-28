// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/FactionRegistry.h"

namespace fl {

void FactionRegistry::load(std::vector<FactionDef> defs) {
    std::scoped_lock lock(m_alertMutex);

    m_defs = std::move(defs);
    m_index.clear();
    m_alertLevels.clear();

    const auto n = static_cast<uint16_t>(m_defs.size());
    m_alertLevels.reserve(n);
    for (uint16_t i = 0; i < n; ++i) {
        m_index[m_defs[i].id] = i;
        m_alertLevels.push_back(m_defs[i].startingAlertLevel);
    }

    // Relationship matrix: Neutral off-diagonal, Friendly on the diagonal (a faction is
    // always friendly with itself).
    m_relations.assign(static_cast<size_t>(n) * n, FactionRelation::Neutral);
    for (uint16_t i = 0; i < n; ++i) {
        m_relations[static_cast<size_t>(i) * n + i] = FactionRelation::Friendly;
    }
}

uint16_t FactionRegistry::indexOf(const std::string& factionId) const noexcept {
    const auto it = m_index.find(factionId);
    return it == m_index.end() ? UINT16_MAX : it->second;
}

const FactionDef* FactionRegistry::get(uint16_t index) const noexcept {
    return index < m_defs.size() ? &m_defs[index] : nullptr;
}

uint16_t FactionRegistry::count() const noexcept {
    return static_cast<uint16_t>(m_defs.size());
}

FactionRelation FactionRegistry::relationship(uint16_t a, uint16_t b) const noexcept {
    const auto n = count();
    if (a >= n || b >= n) {
        return FactionRelation::Neutral;
    }
    return m_relations[static_cast<size_t>(a) * n + b];
}

void FactionRegistry::setRelationship(uint16_t a, uint16_t b, FactionRelation rel) {
    const auto n = count();
    if (a >= n || b >= n) {
        return;
    }
    m_relations[static_cast<size_t>(a) * n + b] = rel;
    m_relations[static_cast<size_t>(b) * n + a] = rel; // symmetric
}

AlertLevel FactionRegistry::alertLevel(uint16_t index) const noexcept {
    std::scoped_lock lock(m_alertMutex);
    return index < m_alertLevels.size() ? m_alertLevels[index] : AlertLevel::Peacetime;
}

void FactionRegistry::setAlertLevel(uint16_t index, AlertLevel level) {
    std::scoped_lock lock(m_alertMutex);
    if (index < m_alertLevels.size()) {
        m_alertLevels[index] = level;
    }
}

} // namespace fl
