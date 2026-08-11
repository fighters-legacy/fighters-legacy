// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/FactionRegistry.h"

namespace fl {

void FactionRegistry::load(std::vector<FactionDef> defs) {
    // Tier 1: load-once. Every reader of m_defs/m_index reads it without a lock, which is only sound
    // while nothing writes it — so this must land before the sim thread exists.
    assert(!SimThreadOwnership::simThreadActive() &&
           "FactionRegistry::load() must run before the sim thread starts: its readers are lock-free");
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

// Tier 2 READS are deliberately unasserted, and finding out why is what #1094's assertions were for.
// The header used to call this tier "sim-thread-only" outright; the first debug fl-server run under the
// assertion aborted on tick 2, from a JobSystem worker inside SensorSystem::evaluateObserver. The
// parallel per-observer detection pass reads relationships from every worker thread BY DESIGN, through
// the hostile(registry, a, b) seam. That is sound and not a race: JobSystem::dispatch is a blocking
// parallel_for in which the owner thread participates and then waits, so the sim thread is inside the
// batch and cannot be writing while workers read, and concurrent reads of unmutated data need no lock.
// An assertion here would have condemned the design rather than the bug.
FactionRelation FactionRegistry::relationship(uint16_t a, uint16_t b) const noexcept {
    const auto n = count();
    if (a >= n || b >= n) {
        return FactionRelation::Neutral;
    }
    return m_relations[static_cast<size_t>(a) * n + b];
}

// Tier 2 WRITES are where the race would be, so this is the assertion that matters. Two callers it
// catches: a network or admin thread writing directly instead of going through
// GameLoop::enqueueSimCallback, and a Lua controller writing from a JobSystem worker during a parallel
// pass -- the second being exactly the "corrupted faction table under load" failure, since workers are
// reading m_relations unlocked at that moment. The sim thread itself cannot write during a batch: it
// is blocked inside dispatch() until every worker is done.
void FactionRegistry::setRelationship(uint16_t a, uint16_t b, FactionRelation rel) {
    assert(SimThreadOwnership::onSimThreadOrSingleThreaded() &&
           "FactionRegistry::setRelationship() is sim-thread-only (m_relations is unlocked and read "
           "from JobSystem workers); route it through GameLoop::enqueueSimCallback");
    const auto n = count();
    if (a >= n || b >= n) {
        return;
    }
    m_relations[static_cast<size_t>(a) * n + b] = rel;
    m_relations[static_cast<size_t>(b) * n + a] = rel; // symmetric
}

bool FactionRegistry::areHostile(uint16_t a, uint16_t b) const noexcept {
    if (a == 0 || b == 0 || a == b) {
        return false; // neutral index / self is never hostile
    }
    return relationship(a, b) == FactionRelation::Hostile;
}

// Tier 3: mutex-guarded, callable from ANY thread — deliberately no thread assertion. The network
// and main threads set alert levels (#162) while the sim thread reads them, which is why this tier
// has a lock at all.
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
