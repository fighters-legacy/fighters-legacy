// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "SnapshotScheduler.h" // SnapshotCandidate, SchedulerWeights, the retention/despawn constants
#include "entity/EntityId.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace fl {

class EntityManager;
class INetwork;
class WorldBroadcaster;
struct EntityState;
struct PeerInputState;        // engine/net/WorldBroadcaster.h — held by pointer only
struct WorldBroadcasterHooks; // engine/net/WorldBroadcaster.h — the host seams (#1082, D12)

// The per-tick snapshot path (#1086, D13): what each peer is told about the world.
//
// Extracted from `WorldBroadcaster::onTick` as a PURE relocation — the serial encode-once pass, the
// replay tap, the per-peer interest/scheduler/stitch pass, congestion and jitter stepping, the ack
// windows, despawn queues, compression and the decimation gates, together with the state only they
// touch. This is the best-engineered part of the server and it comes through the move untouched: no
// threshold, default or algorithm changes, so a byte difference during Stage 8 can only mean a
// mistake. The behavioural work on this path (scratch reuse, interest defaults) landed in Stage 9.
//
// A CONCRETE class owned by value, not an interface (D13) — same shape and the same
// `WorldBroadcaster&` back-reference as [[PeerAdmission]]; see that header for why the seam is a
// friend back-reference rather than a widened public surface or another bag of std::functions.
//
// Threading: `run()` is called on the sim thread. Inside it, the per-peer build is DATA-PARALLEL
// across the broadcaster's JobSystem — each worker writes only its own `PeerSnapWork` and the
// peer-private maps that entry points at, and the sends are flushed serially afterwards on the sim
// thread, which owns the ENetHost. That is what makes the path serial-equivalent by construction,
// and it is the property the byte-compare suite exists to keep.
class SnapshotPipeline {
  public:
    SnapshotPipeline(WorldBroadcaster& wb, EntityManager& entityManager, INetwork& net,
                     const WorldBroadcasterHooks& hooks) noexcept;

    // One tick of the whole path: telemetry + articulation tables, the shared entity snapshot map,
    // the encode-once blobs, the replay tap, the per-peer parallel build, the serial flush, and the
    // spectate-delay drain. Called from the broadcaster's serialize phase.
    //
    // The three governor values are passed in rather than read from the governor here: the broadcaster
    // samples them once per tick, before this runs, and also publishes them to its cross-thread
    // mirrors — so taking them as arguments is what guarantees the pass and the mirrors describe the
    // same tick.
    void run(uint64_t tickIndex, uint32_t govSnapInterval, float govInterestScale, float govLoadFactor);

    // Drop a departing peer's baseline and despawn state. ENet reuses peer ids, so a rejoiner must
    // not inherit either.
    void onDisconnect(uint32_t peerId);

    void setBudgetBytes(uint32_t bytes) noexcept;
    [[nodiscard]] uint32_t budgetBytes() const noexcept;
    void setCompression(bool enabled) noexcept;
    // 0 = the documented 120-tick default. The first tick after construction is always a keyframe, so
    // a recording never opens with deltas whose baseline is not in the file.
    void setReplayKeyframeInterval(uint32_t ticks) noexcept;

  private:
    // The parallel-for seam: dispatch `count` per-peer units through the broadcaster's JobSystem, or
    // run them inline when none is injected (unit tests, single-threaded servers).
    void runPeerPass(std::size_t count, const std::function<void(std::size_t, std::size_t)>& fn);

    WorldBroadcaster& m_wb; // the world being described; see the class comment
    EntityManager& m_entityManager;
    INetwork& m_net;
    const WorldBroadcasterHooks& m_hooks;

    // Per-peer entity tracking: peerId → (entityIdx → record). Drives client-acked delta baselines:
    //   * gen           — full vs delta on respawn (generation change forces a full).
    //   * lastSentTick  — scheduler recency term + the kSnapshotRetentionTicks force-full (the
    //                     interest-out / client-evicted re-entry case) + the knownGens GC prune.
    //   * fullStreakTick— tick the CURRENT contiguous run of full records started on (0 = never sent
    //                     a full). The entity is sent full every tick until the peer confirms it decoded
    //                     fullStreakTick (selective-ack, #566); freezing the streak start (rather than
    //                     advancing it each tick) lets it converge to deltas in one RTT rather than
    //                     re-fulling forever (the confirm target must be a fixed tick the ack can catch).
    //   * lastWasFull   — whether the last record sent for this entity was a full (detects a
    //                     contiguous full run together with lastSentTick).
    // Erased in full on peer disconnect; pruned per-tick once stale past kSnapshotRetentionTicks.
    struct PeerEntityRec {
        uint16_t gen{0};
        uint64_t lastSentTick{0};
        uint64_t fullStreakTick{0};
        bool lastWasFull{false};
        // Articulation send policy (#843): the last channel set this peer was sent for this entity,
        // and when. Sent on CHANGE plus a periodic refresh, so a steady-state aircraft costs zero
        // articulation bytes between refreshes. artSentTick == 0 means "never sent".
        uint32_t artHash{0};
        uint64_t artSentTick{0};
    };
    std::unordered_map<uint32_t, std::unordered_map<uint32_t, PeerEntityRec>> m_peerKnownGens;

    // Per-peer pending explicit despawns (#516): peerId → (entityIdx → remaining repeat ticks). An
    // entity the peer knew that left the sim entirely (kill/despawn) is queued here and emitted in the
    // SnapshotDespawn TLV for kDespawnRepeatTicks ticks (drop tolerance on the unreliable channel).
    // Erased in full on peer disconnect.
    std::unordered_map<uint32_t, std::unordered_map<uint32_t, uint8_t>> m_peerPendingDespawn;

    // Per-tick scratch for the data-parallel per-peer snapshot build. Each entry resolves a peer's
    // stable per-peer state pointers once (serially, in the gather), so the parallel build performs
    // no map operator[] / rehash; the worker writes only into its own buf and the peer-private maps
    // it points at. The sim thread then flushes buf via m_net.send. buf retains capacity across ticks.
    struct PeerSnapWork {
        uint32_t peerId{};
        EntityId peerEid;                      // invalid for an observer (no entity) (#857)
        const EntityState* peerState{nullptr}; // null for an observer
        double center[3]{};                    // interest center: the pilot's entity, or the observer's point
        bool spectator{false};                 // #403: observer or dead pilot — eligible for the snapshot delay

        // #576: which lever is actually spacing THIS peer's snapshots, frozen with the rest of the
        // per-peer work in the serial gather. The governor's numbers are server-wide, but whether
        // they BIND for a given peer depends on that peer's own congestion interval, so the
        // comparison has to be made per peer and cannot be read off the governor alone.
        uint32_t sendIntervalTicks{1}; // the composed interval this peer is actually being sent at
        bool governorBinding{false};   // true when the SERVER's governor is what widened it
        PeerInputState* pin{nullptr};
        std::unordered_map<uint32_t, PeerEntityRec>* knownGens{nullptr};
        std::unordered_map<uint32_t, uint8_t>* pending{nullptr};
        std::vector<uint8_t> buf;
        std::vector<uint8_t> compressScratch; // zstd output scratch (#775); reused across ticks

        // Per-peer snapshot scratch, REUSED across ticks (#1092). These ten were locals inside the
        // per-peer pass, so the pass allocated ten fresh vectors per peer per tick — roughly 77,000
        // allocations/second at 128 peers and 60 Hz, inside the PARALLEL region, which makes it
        // allocator contention across worker threads as well as raw allocation cost. `buf` and
        // `compressScratch` above already demonstrated the fix; these simply never got it.
        //
        // Every one is cleared before use, never read across ticks. That matters: buffer reuse that
        // leaks stale contents would change the wire, which the serial-equivalence byte-compare
        // exists to catch.
        std::vector<uint32_t> visible;                  // interest-query hits, exact-gated
        std::vector<uint32_t> selected;                 // budget-scheduled subset of `visible`
        std::vector<SnapshotCandidate> cands;           // priority ranking input
        std::vector<std::array<double, 3>> originTable; // shared quantization origins, deduped
        std::vector<uint8_t> recordStream;              // stitched entity records
        std::vector<uint8_t> ownBlob;                   // the own-entity re-encode
        std::vector<uint32_t> despawnIds;               // SnapshotDespawn TLV payload
        std::vector<uint8_t> effectsBlob;               // SnapshotEffects TLV payload
        std::vector<uint8_t> articulationBlob;          // SnapshotArticulation TLV payload
        std::vector<uint8_t> crewBlob;                  // SnapshotCrew TLV body
        std::vector<uint8_t> payload;                   // assembled (possibly compressed) payload
    };
    std::vector<PeerSnapWork> m_peerWork;

    // Replay tap baselines (#643): the recorder's own known-generation map, independent of any peer's.
    std::unordered_map<uint32_t, uint16_t> m_replayKnownGens;
    uint32_t m_replayKeyframeInterval{120};
    bool m_replayForceKeyframe{true};

    // Atomics: written by the sim thread, read inside the parallel per-peer region.
    std::atomic<uint32_t> m_snapshotBudgetBytes{0};
    std::atomic<bool> m_compressSnapshots{false};
    SchedulerWeights m_schedulerWeights{}; // relevance weights (tuned defaults; sim-thread only)
};

} // namespace fl
