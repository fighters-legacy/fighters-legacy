// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Per-client priority/budget snapshot scheduler (#516). Pure ranking + budgeting policy, isolated
// from WorldBroadcaster the same way SnapshotCodec / JitterBuffer / AuthTracker are — no glm, no
// engine-entity deps, fully unit-testable in isolation.
//
// Each tick the broadcaster builds a SnapshotCandidate per entity visible to a peer (after the 3D
// interest cull), then calls selectSnapshotRecords() to choose the highest-relevance set that fits a
// per-client byte budget. Lower-priority entities are deferred to a later tick; a recency term
// (ticksSinceSent) guarantees no entity is starved indefinitely. The peer's own entity is always
// admitted first (client-side prediction reconciliation needs it every tick).

#include <cstdint>
#include <vector>

namespace fl {

// One entity considered for inclusion in a peer's snapshot this tick.
struct SnapshotCandidate {
    uint32_t idx{0};
    double distSq{0.0};         // squared distance to the peer (closer ranks higher)
    float closingSpeed{0.f};    // m/s toward the peer along the line of sight (threat proxy; >0 = approaching)
    uint64_t ticksSinceSent{0}; // tickIndex - lastSentTick (anti-starvation; huge/UINT64_MAX for never-sent)
    uint32_t estBytes{0};       // estimated encoded record size (SnapshotCodec::estimateRecordBytes)
    bool isOwn{false};          // the peer's own entity — admitted unconditionally, ahead of the budget
    bool playerOwned{false};    // player-controlled entity — relevance boost over AI/scenery
};

// Relevance weighting. Defaults are tuned constexpr; promotable to config later (#518). The score is
//   wDistance*(1 - dist/drawDist) + wRecency*min(ticksSinceSent, recencyCapTicks)
//     + wThreat*max(0, closingSpeed)/closingSpeedRef + wPlayer*playerOwned
struct SchedulerWeights {
    float wDistance{1.0f};
    float wRecency{0.04f};
    float wThreat{0.5f};
    float wPlayer{2.0f};
    float closingSpeedRef{300.f};  // m/s that maps the threat term to ~1.0
    uint64_t recencyCapTicks{600}; // clamp on the recency term so it can't dwarf everything else
};

// Shared server/client agreement constant (both binaries compile this single definition): the number
// of ticks the client retains a not-recently-updated entity before evicting it, and the gap after
// which the server force-sends a full record so a returning entity is decodable. ~3 s at 60 Hz.
inline constexpr uint64_t kSnapshotRetentionTicks = 180;

// Number of ticks an explicit despawn is repeated on the unreliable snapshot channel (drop-tolerant).
inline constexpr uint8_t kDespawnRepeatTicks = 4;

// Articulation refresh interval in ticks (#843). The channel set is sent on CHANGE; this is the
// periodic re-send that covers a dropped packet and a peer whose interest set just gained the entity.
// Half a second at 60 Hz: an actuator takes seconds to travel, so a worst-case half-second of stale
// pose is invisible, while a shorter interval would spend bytes on nothing.
inline constexpr uint64_t kArtRefreshTicks = 30;

// Rank candidates by relevance and return the admitted entity indices that fit recordByteBudget, in
// priority (descending-score) order. The caller re-sorts the result ascending before encoding (the
// codec's idx-delta varints require ascending idx). recordByteBudget == 0 means unlimited (admit all).
// The peer's own entity (isOwn) is always admitted first regardless of budget. Ties break by ascending
// idx so the selection is deterministic (preserves serial-equivalence of the parallel sim tick).
std::vector<uint32_t> selectSnapshotRecords(std::vector<SnapshotCandidate>& cands, uint32_t recordByteBudget,
                                            const SchedulerWeights& weights, double drawDistM);

} // namespace fl
