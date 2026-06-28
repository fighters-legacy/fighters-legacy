// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "world/FactionDef.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace fl {

// O(1)-by-index faction store for the hot sim path. EntityState::factionIndex indexes
// into this registry. Loaded once at mission/server init, then queried every tick.
//
// Threading contract (three tiers):
//   * m_defs / m_index   — immutable after load() (called before gameLoop.start());
//                          lock-free reads from any thread. A FactionDef* returned by
//                          get() stays valid for the session; do NOT retain it across
//                          a load() reload.
//   * m_relations        — sim-thread-only, no lock. setRelationship() is only ever
//                          driven by Lua/mission world.set_relationship() (sim thread);
//                          relationship() is read on the sim thread (AlertSystem, #162).
//                          Never call setRelationship() from another thread.
//   * m_alertLevels      — mutex-guarded. setAlertLevel() may be called from the
//                          main/network thread (#162) while onTick()/Lua read on the
//                          sim thread, so both alertLevel() and setAlertLevel() lock.
//
// The std::mutex member makes FactionRegistry non-copyable/non-movable (intentional:
// single-owner registry held by reference, e.g. AlertSystem(FactionRegistry&)).
class FactionRegistry {
  public:
    // Replaces all state. Builds the id->index map, a count*count relationship matrix
    // (default Neutral off-diagonal, Friendly on the diagonal), and seeds alert levels
    // from each def's startingAlertLevel. Call once before gameLoop.start().
    void load(std::vector<FactionDef> defs);

    uint16_t indexOf(const std::string& factionId) const noexcept; // UINT16_MAX if not found
    const FactionDef* get(uint16_t index) const noexcept;          // nullptr if out of range
    uint16_t count() const noexcept;

    FactionRelation relationship(uint16_t a, uint16_t b) const noexcept; // Neutral if either OOB
    void setRelationship(uint16_t a, uint16_t b, FactionRelation rel);   // symmetric

    AlertLevel alertLevel(uint16_t index) const noexcept; // Peacetime if OOB
    void setAlertLevel(uint16_t index, AlertLevel level);

  private:
    std::vector<FactionDef> m_defs;
    std::unordered_map<std::string, uint16_t> m_index;
    std::vector<FactionRelation> m_relations; // m_relations[a * count + b]
    std::vector<AlertLevel> m_alertLevels;
    mutable std::mutex m_alertMutex;
};

} // namespace fl
