// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "util/SimThreadOwnership.h" // #1094: the mechanism behind the threading contract below
#include "world/FactionDef.h"

#include <cassert>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace fl {

// O(1)-by-index faction store for the hot sim path. EntityState::factionIndex indexes
// into this registry. Loaded once at mission/server init, then queried every tick.
//
// Threading contract (three tiers), ASSERTED in debug builds since #1094 — the tiers below are
// mechanisms now, not promises. Mixed locked and unlocked members in one class is exactly the shape
// TSan finds at 128 players and never at 8: a sim-thread-only relationship write from an admin
// command surfaces as a corrupted faction table under load, long after the change that caused it.
// Writing the assertions also corrected the contract itself — see the m_relations tier.
//   * m_defs / m_index   — immutable after load() (called before gameLoop.start());
//                          lock-free reads from any thread. A FactionDef* returned by
//                          get() stays valid for the session; do NOT retain it across
//                          a load() reload. load() asserts no sim thread is running.
//   * m_relations        — unlocked; WRITES are sim-thread-only, READS are not. Corrected
//                          under #1094: this tier's comment used to say "sim-thread-only"
//                          flat out, and the first debug fl-server run under the new
//                          assertion aborted on tick 2 inside a JobSystem worker. The
//                          parallel per-observer sensor pass reads relationships from every
//                          worker through the hostile(registry, a, b) seam, by design.
//                          That is sound: JobSystem::dispatch is a BLOCKING parallel_for in
//                          which the owner thread participates and then waits, so no write
//                          can be in flight while workers read, and concurrent reads of
//                          unmutated data need no lock. setRelationship() asserts the sim
//                          thread; relationship() deliberately asserts nothing, because an
//                          assertion there would condemn the design rather than a bug.
//                          A write from a worker -- a Lua controller calling
//                          world.set_relationship during a parallel pass -- IS the race, and
//                          is what the write-side assertion exists to catch. Route any
//                          off-thread write through GameLoop::enqueueSimCallback.
//   * m_alertLevels      — mutex-guarded. setAlertLevel() may be called from the
//                          main/network thread (#162) while onTick()/Lua read on the
//                          sim thread, so both alertLevel() and setAlertLevel() lock,
//                          and NEITHER asserts a thread: any thread is legal here, which
//                          is the whole reason this tier has a lock and the others do not.
//
// "No sim thread is running" satisfies the sim-thread tier: before start() and after stop() the
// process is single-threaded, and an assertion that fired there would condemn load() itself, plus
// every unit test that drives the registry with no GameLoop at all.
//
// The std::mutex member makes FactionRegistry non-copyable/non-movable (intentional:
// single-owner registry held by reference, e.g. AlertSystem(FactionRegistry&)).
class FactionRegistry {
  public:
    // Replaces all state. Builds the id->index map, a count*count relationship matrix
    // (default Neutral off-diagonal, Friendly on the diagonal), and seeds alert levels
    // from each def's startingAlertLevel. Call once before gameLoop.start() — asserted,
    // because every lock-free reader of m_defs/m_index depends on it.
    void load(std::vector<FactionDef> defs);

    uint16_t indexOf(const std::string& factionId) const noexcept; // UINT16_MAX if not found
    const FactionDef* get(uint16_t index) const noexcept;          // nullptr if out of range
    uint16_t count() const noexcept;

    FactionRelation relationship(uint16_t a, uint16_t b) const noexcept; // Neutral if either OOB
    void setRelationship(uint16_t a, uint16_t b, FactionRelation rel);   // symmetric

    // Coalition-aware hostility: true when a and b are enemies per the relationship matrix. Index 0
    // is the reserved neutral faction (no enemies) and a faction is never hostile to itself — the
    // same guards as the affiliation rule areFactionsHostile() (FactionDef.h). Missions populate the
    // matrix so distinct non-allied sides are Hostile (MissionSetup.h); a Friendly (allied) or
    // Neutral relationship is not hostile. Sim-thread-only (reads m_relations, like relationship()).
    [[nodiscard]] bool areHostile(uint16_t a, uint16_t b) const noexcept;

    AlertLevel alertLevel(uint16_t index) const noexcept; // Peacetime if OOB
    void setAlertLevel(uint16_t index, AlertLevel level);

  private:
    std::vector<FactionDef> m_defs;
    std::unordered_map<std::string, uint16_t> m_index;
    std::vector<FactionRelation> m_relations; // m_relations[a * count + b]
    std::vector<AlertLevel> m_alertLevels;
    mutable std::mutex m_alertMutex;
};

// Resolve hostility through a registry when one is available, else fall back to the affiliation rule
// (areFactionsHostile). The nullable registry is the AiTickContext seam: null = "not evaluated" (the
// pre-mission behavior — distinct non-zero factions are hostile), non-null = coalition-aware
// relationships (a mission-loaded registry, #632). Header-only so engine-ai and engine-net share it
// with no new link dependency.
[[nodiscard]] inline bool hostile(const FactionRegistry* reg, uint16_t a, uint16_t b) noexcept {
    return reg ? reg->areHostile(a, b) : areFactionsHostile(a, b);
}

} // namespace fl
