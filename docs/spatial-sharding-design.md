# Spatial Sharding — Design Record and Trigger Criterion

Design record for the spatial-sharding contingency (Epic A, [#494]). Resolves the design spike
[#572]. The job-system spike ([#510], [server-job-system-design.md](server-job-system-design.md))
chose a data-parallel single authoritative tick and deferred spatial sharding as *"the next scaling
axis if a single tick's parallel headroom is exhausted"*. This document answers whether and how to
shard, records the contingent design so a future implementation starts from a worked plan rather
than a blank page, and — most importantly — defines the **measurable trigger** that says "now".

**Recommendation: defer.** Do not implement spatial sharding now. On a single machine it is
strictly dominated by the data-parallel tick that already landed; the flight-sim workload is
unusually hostile to spatial partitioning; and the one thing sharding genuinely buys —
multi-machine scale — is beyond the 128+ single-box product target and is a *product* decision,
not a performance contingency. A ladder of cheaper levers (below) attacks the measured bottleneck
first. The corresponding dated decision record is in
[architecture.md](architecture.md#decision-records).

[#494]: https://github.com/fighters-legacy/fighters-legacy/issues/494
[#510]: https://github.com/fighters-legacy/fighters-legacy/issues/510
[#511]: https://github.com/fighters-legacy/fighters-legacy/issues/511
[#512]: https://github.com/fighters-legacy/fighters-legacy/issues/512
[#514]: https://github.com/fighters-legacy/fighters-legacy/issues/514
[#516]: https://github.com/fighters-legacy/fighters-legacy/issues/516
[#517]: https://github.com/fighters-legacy/fighters-legacy/issues/517
[#518]: https://github.com/fighters-legacy/fighters-legacy/issues/518
[#520]: https://github.com/fighters-legacy/fighters-legacy/issues/520
[#569]: https://github.com/fighters-legacy/fighters-legacy/issues/569
[#572]: https://github.com/fighters-legacy/fighters-legacy/issues/572
[#573]: https://github.com/fighters-legacy/fighters-legacy/issues/573
[#574]: https://github.com/fighters-legacy/fighters-legacy/issues/574
[#575]: https://github.com/fighters-legacy/fighters-legacy/issues/575
[#725]: https://github.com/fighters-legacy/fighters-legacy/issues/725
[#726]: https://github.com/fighters-legacy/fighters-legacy/issues/726

## Problem and evidence

Once a single authoritative tick is serialize-bound at full worker-count parallelism, the only
remaining scaling axis is to partition the world so multiple ticks run concurrently. The question
is contingent — it only matters if the data-parallel headroom actually runs out — so the evidence
about *where the cost is* decides everything.

The entity-scale characterisation ([#573],
[entity-scale-characterization.md](entity-scale-characterization.md)) established:

- The **entity pool and `SpatialIndex` are not the ceiling** — integrate/AI/collision stay cheap
  even at 5000 entities. The dominant per-tick cost at scale is the **Serialize phase**: the
  per-peer, interest-managed, budgeted snapshot build, ∝ `clients × visible entities`.
- The Serialize phase **already parallelises with worker count** (the [#512] per-peer pass):
  22.4 → 9.1 ms at 5000 entities from 1 → 4 workers; the 5000-entity single-worker collapse
  (36.8 Hz) recovers to a held 60 Hz at 4 workers.
- The graceful path when the tick is over budget anyway is the [#514] overrun governor
  (send-rate + byte-budget + AI-stride shedding), and the `GameLoop` catch-up cap bounds the
  failure mode as time dilation with a visible `dropped_ticks` counter.

So the honest framing is not "when do we shard" but "what, if anything, does sharding buy that
the existing design does not — and what would it cost".

## What sharding can and cannot buy

`WorldBroadcaster::onTick` today decomposes as:

| Phase | Parallel? | Notes |
|---|---|---|
| Maintenance (drain callbacks, spatial rebuild, governor step) | serial | cheap (sub-ms at 5000 entities) |
| AI pass (`controller->sample()`) | **parallel** ([#511]) | read-only on a consistent pre-step snapshot |
| Integrate pass (`stepFlightSim`) | **parallel** ([#511]) | disjoint per-entity writes |
| Serialize gather (resolve per-peer state pointers) | serial | pointer resolution only |
| Serialize build (interest query + scheduler + encode per peer) | **parallel** ([#512]) | the dominant cost at scale |
| Serialize flush (`m_net.send` per peer) + `service(0)` | serial | transport is sim-thread-owned |
| Serial tail (reap, weather, shutdown, rate-limit prune) | serial | cheap |

**On one machine, spatial sharding is strictly dominated by this design.** Shards and the
`JobSystem` consume the same cores. Everything expensive is already a `parallel_for`; sharding
re-partitions the same work while *adding* seam costs — ghost exchange, authority hand-off,
cross-shard interest queries, snapshot stitching — and weakening the serial-equivalence guarantee
that makes the current parallelism testable. A sharded server on N cores does at best the same
compute as the data-parallel tick on N cores, plus overhead.

The only strictly-serial remnants are the gather, the flush + `service(0)`, and the serial tail —
all small today. If the *flush/service* tail ever binds at very high peer counts, the cheaper
decomposition is **peer/transport sharding** — multiple `INetwork` instances, each on its own
thread owning a subset of peers, over one authoritative world (a natural extension of the [#512]
build/flush split, and compatible with the `INetwork` single-owner rule since each host has
exactly one owning thread) — not world partitioning.

What spatial sharding *uniquely* buys is **more machines**: multiple boxes each running an
authoritative tick over a region. That is real, but it is beyond the 128+ single-box target
(which the reference environment already meets with worker headroom) and squarely a Phase 5+
product decision — see [Threads vs processes](#threads-vs-processes).

## Workload analysis: a combat flight sim is hostile to spatial partitioning

Three properties of this workload conspire against region partitioning; MMO-style sharding
folklore does not transfer.

**Interest radii are enormous.** The default per-peer interest radius is 200 km
(`[world] draw_distance_km`). Any entity within the radius of a seam must be visible from both
sides, so each shard must mirror a **ghost halo** of one interest radius around its region. For a
square region of width `W` with halo `R = 200 km`, the mirrored band area is `(W + 2R)² − W²`;
for the halo to be no larger than the owned area, `W ≥ 2R/(√2 − 1) ≈ 4.8 R ≈ ~1000 km`. Regions
smaller than that mirror more world than they own — at `W = 400 km` a shard mirrors **3×** its
own area. Usable regions are therefore ~1000 km-class objects, and a typical theater
(~500 × 500 km) fits inside *one*.

**Aircraft are fast.** At cruise (~300 m/s) to Mach 2 (~680 m/s), a 1000 km region is crossed in
24–55 minutes — tolerable — but entities *fight along seams*: a dogfight drifting across a
boundary would migrate authority repeatedly without a hysteresis band, and every migration is a
full state hand-off (inventory below).

**Combat converges.** The whole point of a mission is that 128 players meet at an objective. At
peak load the interest graph is fully connected and every entity is inside every peer's radius —
exactly the moment sharding degenerates to one hot shard doing all the work while the others
idle. Sharding helps *dispersed* load; the design-driving scenario here is the furball, which is
maximally concentrated. (This is also why the [#516] priority/budget scheduler, which bounds
per-peer bytes regardless of visible count, is the right primary tool for the furball.)

## Partitioning-model comparison

Recorded for the contingent design; none of this changes the defer recommendation.

| Model | Sketch | Verdict |
|---|---|---|
| **Static uniform grid** | fixed `W ≥ ~1000 km` cells, entity→cell by position | Simplest; deterministic assignment. Loses badly to the furball (all load in one cell, rebalancing impossible by construction). Acceptable only for a persistent planet-scale world with naturally dispersed population. |
| **Dynamic coarse regions** (recommended if ever needed) | start with one region; split (kd-style, along the long axis) when a region's tick cost exceeds a threshold, merge when it falls; entity-count/cost-balanced split point; migration hysteresis band at seams | Balances real load; splits only pay off when the population is actually separable (two distant theaters). Degenerates gracefully: an inseparable furball stays one region — i.e. the sharded server behaves exactly like today's, which is the correct floor. Re-balancing churn is bounded by split/merge hysteresis (minutes, not ticks). |
| **Interest-graph partitioning** | partition the peer/entity interest graph (min-cut) rather than space | Theoretically minimises cross-shard edges, but the graph mutates every tick at combat speeds, min-cut is expensive, and assignments are unstable (an entity's shard changes because *someone else* moved). Rejected. |

**Contingent pick: dynamic coarse regions** — with the explicit caveat that for the product's
actual scenarios (one theater, converging combat) the dynamic model usually resolves to a single
region, which is the strongest empirical argument that the mechanism isn't worth its complexity
on one box.

## Seam protocol sketch

For the contingent design (either threading model):

- **Authority** — every entity has exactly one owner shard. Authority hand-off happens only at a
  tick boundary, when the entity's position crosses the seam plus a **hysteresis band** (e.g.
  10 km ≈ 15–30 s of flight) so seam-hugging entities do not oscillate.
- **Ghosts** — each shard maintains read-only **ghost replicas** of foreign entities within one
  interest radius of its region, refreshed once per lockstep tick from the owner's post-integrate
  state. Ghosts feed AI `sample()` reads (`EntityManager::get`, `nearby_entities`) and snapshot
  assembly. A ghost is deterministically **exactly one tick stale** — the same staleness class
  clients already absorb from the network, and far tighter than the jitter-buffer depth.
- **Cross-shard interest queries** — each shard's `SpatialIndex` indexes owned entities + ghosts;
  a peer's interest query never leaves its home shard because the ghost halo guarantees local
  coverage of the full radius.
- **Snapshot stitching** — none needed as a separate mechanism: the peer's home shard (the shard
  owning the peer's entity) assembles that peer's whole snapshot from owned + ghost state. The
  quantized record stream, per-peer budget, delta baselines, and `SnapshotDespawn` TLVs work
  unchanged; despawn detection must consult the ghost table so an interest-out is not
  misclassified as a sim removal.

## Entity-migration state inventory

What must move when an entity crosses a shard boundary — the issue's explicit ask. Names are the
current `WorldBroadcaster` members.

**Entity-scoped (moves with every migrating entity):**

| State | Where | Notes |
|---|---|---|
| Pool slot: transform, hp, damage, `factionIndex`, flags | `EntityManager` / `EntityState` | threads model: no move (single shared pool, ownership tag flips); process model: serialize + respawn, **breaking `EntityId{index, gen}` stability** (see below) |
| Flight integrator | `ControlledEntity::sim` (`FlightIntegrator`, incl. `FlightState` vel/omega/fuel/`current_sweep_deg`) | threads: `unique_ptr` move; process: full `FlightState` serialization |
| Controller | `ControlledEntity::controller` (`IEntityController`) | internal mutable state: `StateMachineController` current state + dwell timers, ACM phase timers, **`LuaController`'s `lua_State` — not serializable**; a process-model migration must re-instantiate Lua AI from script + a coarse behavioural checkpoint, an accepted fidelity loss |
| Governor decimation cache | `ControlledEntity::lastInput` / `lastInputValid` / `decimatable` | trivially copyable |
| Spatial index membership | per-shard `SpatialIndex` | rebuilt each tick anyway — free |

**Peer-scoped (moves when a *player's* entity migrates, i.e. the peer's home shard changes):**

| State | Where | Notes |
|---|---|---|
| Transport peer association | `INetwork` peer ↔ shard | **the hard one** — threads model avoids moving it entirely (transport stays on its owning thread); process model needs connection hand-off, a gateway, or client reconnect |
| Input/ack state | `PeerInputState`: `jitterBuffer`, `lastSeqNum`/`hasSeq`, `ackedTick`+`ackMask`, `estimatedDelayTicks`, `ewmaDelayTicks`/`ewmaJitterTicks`, `lastActivityTick`, drained control fields | must move atomically with the entity or inputs are lost/duplicated for a tick |
| Congestion controller | `PeerInputState::congestion` ([#518] `CongestionController` AIMD state), `sentSnapshot`/`lastSnapshotSentTick` | resettable at low cost (re-converges in ~seconds), but moving it avoids a send-rate transient |
| Delta baselines | `m_peerKnownGens[peerId]` — per-entity `PeerEntityRec{gen, lastSentTick, fullStreakTick, lastWasFull}` ([#517]) | a **peer × entity cross product**; on the destination shard, entries referencing entities the new shard cannot see must be dropped (the `kSnapshotRetentionTicks` force-full backstop already covers re-entry) |
| Pending despawns | `m_peerPendingDespawn[peerId]` | small; must move or the client leaks a stale entity until retention timeout |
| Flood/limits bookkeeping | `m_peerFloodState[peerId]`, `m_peerEntities[peerId]` | trivial |

**Global (replicate to all shards or keep on a coordinator):** the tick index (lockstep — shared
by construction), `WeatherController`, shutdown countdown state, `AuthTracker` +
ban/allow lists + `m_connectRecords`, spawn points, MOTD, `FactionRegistry` (mutable alert
levels — needs a single writer or per-tick reconciliation), and the `TickGovernor` (per-shard
`loadFactor` with globally-agreed floors, else one overloaded shard silently degrades only its
own peers).

**Timing rule:** all migration happens between ticks, on the lockstep barrier — never mid-pass —
so the serial-equivalence guarantees survive within each shard.

**`EntityId` stability is the wire-protocol fault line.** `EntityId{index, generation}` is
pool-local, and `entityIdx` is what the wire carries (`SnapshotCodec` records) and what clients
key their caches on (`m_entityCache`/`m_knownEntities`). A **single shared `EntityManager`**
(threads model) keeps IDs — and therefore the entire wire protocol and client — unchanged. A
per-shard pool (process model) forces a global entity-ID allocation scheme and a wire change.
Pre-freeze that is allowed (`kProtocolVersion` stays 1 during primary development), but it is a
real cost the process model carries and the threads model does not.

## Threads vs processes

**Threads — the contingent on-box design.** One `fl-server` process; each shard is a *tick
domain* on its own thread; one shared `EntityManager` partitioned by an ownership tag; ghosts are
read via double-buffered post-integrate snapshots (no locks in the hot path); the shards run in
**lockstep**: integrate ∥ → barrier → ghost exchange + migrations → barrier → serialize ∥ →
flush. The transport host keeps a single owning thread (the `INetwork` rule for both enet6 and
GNS): shard threads *build* per-peer buffers and the owning thread *flushes* — which is exactly
the [#512] build/flush shape, generalised. Zero wire change, `EntityId`s stable, `LocalServer` /
single-player unaffected (`--sim-worker-threads 1` ≙ one shard).

The honest observation: barriered-lockstep shard threads over one process are functionally the
existing `JobSystem` partitioning with extra steps — the barriers are where `parallel_for`
already joins, and the "shards" are chunk assignments with sticky affinity. This is the strongest
form of the domination argument, and it is why no intra-machine implementation is recommended at
any trigger level: if the tick is over budget on one box at max workers, shard threads on the
same box cannot fix it.

**Processes — the multi-machine path.** Shard-per-process is what actually adds compute, and it
is a different product: a gateway/router tier (or client reconnect-on-migrate), an inter-shard
replication bus for ghosts and hand-offs, global entity IDs (wire change), full state
serialization for every migration (including the Lua constraint above), snapshot assembly on the
peer's home shard from bus-replicated ghosts, and an ops surface — which is precisely Epic K
(`fl-operator`, Agones fleets) plus the Phase 5 observability tier. It interacts *cleanly* with
the transport factory: each process owns its `INetwork` on its own sim thread, unchanged. If a
persistent-world / >256-player product goal is ever adopted, this lands as its own Phase 5+ epic
with this document as the starting inventory; it should not be built speculatively.

## Determinism

- Within a shard, the existing guarantees hold unchanged: no cross-entity writes in parallel
  regions, per-`(entityIdx, tickIndex)` turbulence seeding, per-peer-isolated snapshot builds —
  bit-identical across worker counts (`test_world_broadcaster`).
- Across shards under lockstep, ghost reads are **deterministically one tick stale** — a defined
  data dependency, not a race. For a *fixed* region assignment, results are bit-identical across
  shard-internal worker counts; they are **not** identical across different shard *layouts*
  (an entity reading a neighbour via a ghost sees `t−1` state where the unsharded sim sees `t−1`
  pre-step state — the same tick's pre-step snapshot, so in fact AI `sample()` semantics are
  preserved exactly; the divergence appears only for *cross-shard physical interactions*, e.g.
  future collision/damage resolution, which would need an explicit cross-seam protocol).
- The serial-equivalence test strategy extends naturally: assert bit-identical transforms and
  per-peer packet streams for `shardCount ∈ {1, N}` on workloads with no cross-seam
  interactions, plus TSan over the barrier/exchange machinery.

## Load-harness interaction

How Epic I ([#520]/[#569]) measures a sharded server, recorded for the contingency:

- **Patterns**: `bot_swarm` needs spatial-distribution `IFlightPattern`s — a `spread` pattern
  placing clients across N distant theaters (the separable best case) and a `converge` pattern
  flying everyone to one point (the furball floor). The furball profile must show **no
  regression vs the unsharded baseline** — that is the acceptance test for "degenerates
  gracefully".
- **Metrics**: `ServerTickReport` grows per-shard phase blocks plus `migrations_per_s` and
  `ghost_count` (schema bump); the scale gate baselines only the machine-independent KB/s, as
  today.
- **Infra**: the threads model is measurable on the existing 8-core reference runner; the
  process model is **not** — it needs a multi-box harness and orchestration, which is another
  cost item the process model carries (and another reason it belongs with Epic K, where that
  infrastructure exists anyway).

## The pre-sharding ladder

Cheaper levers that attack the measured bottleneck, in order. Each has its own trigger; sharding
is last.

1. **Scale up cores.** Serialize scales with worker count today; an operator with a bigger box
   gets capacity linearly-ish (`[world] sim_worker_threads = 0` auto-sizes). Caveat: on Windows,
   `std::thread` is confined to one processor group (≤ 64 logical processors) — the known v1
   limitation; Linux is the primary self-host target.
2. **Shared snapshot quantization — encode once per entity** ([#725], filed from this
   spike). Today `MsgWorldSnapshotHeader::frameOrigin` is the *receiving peer's* position, so
   every record is quantized and bit-packed **per peer** — the `clients × visible` cost is
   partly an encode constant we chose, not a law. Moving to shared quantization origins (e.g.
   per-spatial-cell origins carried in the header or a TLV) and position-independent records
   (absolute-idx or byte-aligned records instead of the current `prevIdx` delta-varint) lets the
   sim encode each entity **once per tick** (a full and a delta variant), reducing per-peer work
   to interest query + scheduler + record stitching: `O(entities)` encode + `O(peers × visible)`
   memcpy. Wire-affecting; fine pre-freeze.
3. **Governor interest-radius lever** ([#726], filed from this spike). A fourth
   [#514] shedding lever scaling each peer's *effective* draw distance under overrun (floor knob,
   hot-reloadable, composed like the others). Uniquely, it shrinks the **visible set itself** —
   the input to every downstream cost — where the existing budget lever only trims the encoded
   output.
4. **LOD physics for distant AI** ([#575], open spike) — the *integrate-bound* case, which
   sharding also cannot help (same cores).
5. **Peer/transport sharding** — multiple `INetwork` instances on their own threads over one
   authoritative world, if the serial flush/`service()` tail ever binds. No world partitioning,
   no migration, no ghosts.
6. **Spatial sharding** — only on the trigger below, and only in its process/multi-machine form
   (the threads form is dominated — see above).

## ⚠ Measurement caveat: these numbers were taken on enet6, not the default transport ([#649])

**Everything measured in this document was measured on enet6. The default internet transport is
GameNetworkingSockets** ([#507]), and it changes the picture by an order of magnitude. Same box
(8-core reference), same build, 128 clients, weave:

| | tick mean | serialize | integrate | serialize share |
|---|---:|---:|---:|---:|
| enet6 | 8.20 ms | 8.03 ms | 0.06 ms | 98 % |
| **GNS** | **1.01 ms** | **0.81 ms** | 0.05 ms | 80 % |

**~90 % of what the `serialize` phase cost was ENet's inline per-packet send, not this engine's
snapshot pipeline** — ENet does that work on the calling thread, so it lands inside the sim tick;
GNS queues to its own service thread and returns. Our actual encode + schedule cost is ~0.8 ms at
128 clients.

What this does **not** change: serialize is still the dominant phase on GNS (80 %), so the
phase-routing clause below (clause 3) still routes correctly, and integrate is unchanged.

What this **does** change is the magnitude, and therefore the urgency:

- On the transport that ships, the tick runs **~16× under its 16.6 ms budget**, not ~2×. Clauses 1
  and 2 (governor pinned at floor, `dropped_ticks` rising) are correspondingly much further from
  firing — **this deferral is better justified than the enet6 numbers suggest**, not worse.
- **Any optimisation on the pre-sharding ladder justified against an "8 ms serialize phase" is sized
  against a number that is really ~0.8 ms on the default transport.** Re-derive the prize before
  spending effort on it.

The characterization is being re-derived on GNS ([#649] follow-up); until then, treat every absolute
figure in this document as an **enet6 upper bound**.

## Trigger criterion

All quantities are already produced by `fl-server --metrics-json` (`ServerTickReport` schema
v2) and surfaced through the scale gate ([#520]), the overrun profile ([#574]), and the 8-core
reference runner ([#569]).

> **Implement spatial sharding only when, on the 8-core reference environment at a
> product-target workload (≤ 128 real clients + mission-scale AI), with
> `sim_worker_threads = 8` and the overrun governor enabled, a sustained window (≥ 60 s)
> shows *all* of:**
>
> 1. `server_tick.load_factor` pinned at its configured floor — the governor has shed
>    everything it can;
> 2. `server_tick.dropped_ticks` still rising monotonically — the `GameLoop` catch-up cap is
>    engaged despite the shedding;
> 3. `server_tick.serialize_ms.mean > 0.5 × server_tick.tick_ms.mean` — the tick is
>    serialize-bound, not integrate-bound (an integrate-dominant overrun routes to [#575]
>    instead);
> 4. ladder items 2 and 3 (shared encode, interest-radius lever) have already landed and been
>    re-measured.
>
> **Independently**, a product decision to exceed one machine (persistent world, or a target
> materially beyond ~256 players) triggers the **process-model** epic regardless of single-box
> headroom — as a Phase 5+ initiative alongside Epic K, using this document's seam protocol and
> migration inventory as the starting design.

## Recommendation

Defer. File no implementation epic. Adopt the ladder: land the shared-encode optimization
([#725]) and the governor interest-radius lever ([#726]), keep [#575] as the
integrate-bound contingency, and re-measure on the reference runner. Revisit only on the trigger
above — and if the trigger that fires is the product one, implement the **process model** as a
Phase 5+ epic; do not build the threads model at all.

[#649]: https://github.com/fighters-legacy/fighters-legacy/issues/649
[#507]: https://github.com/fighters-legacy/fighters-legacy/issues/507
