# Server Job System — Data-Parallel Tick

Design record for the server-simulation scalability work (Epic A, [#494]). Resolves the design
spike [#510] and documents the implementation that lands in [#511]. Follow-on parallelisation of
snapshot assembly ([#512]) and graceful overrun handling ([#514]) build on the seam described here.

[#494]: https://github.com/fighters-legacy/fighters-legacy/issues/494
[#510]: https://github.com/fighters-legacy/fighters-legacy/issues/510
[#511]: https://github.com/fighters-legacy/fighters-legacy/issues/511
[#512]: https://github.com/fighters-legacy/fighters-legacy/issues/512
[#514]: https://github.com/fighters-legacy/fighters-legacy/issues/514

## Problem

`WorldBroadcaster::onTick` — the authoritative 60 Hz server step — ran entirely on one thread: it
stepped every `FlightIntegrator` + AI controller sequentially, then built every per-peer snapshot
sequentially. The 8-core/16 GB Release reference-env characterisation ([#505]) showed this is the
128+ ceiling:

- Observed server tick-Hz collapses (idle ~57 Hz @128 → ~20 @256; weave/aggressive ~47 @96 → ~26
  @128) **while the load harness stays idle** (worker loop-dt ~2.5 ms) — i.e. the server is
  CPU-bound on the single-threaded sim + per-peer snapshot build, not on the `enet6` transport.
- Physics is a first-class cost (idle→weave drops the ceiling ~128 → ~96), so this is a
  *parallel-sim* problem, not merely faster serialization.

[#505]: https://github.com/fighters-legacy/fighters-legacy/issues/505

## Decision: data-parallel over a single authoritative tick — not spatial sharding

Two designs were considered for using multiple cores:

1. **Data-parallel single tick** (chosen): keep one authoritative world state; parallelise the
   per-entity work *within* a single tick across a worker pool.
2. **Spatial sharding**: partition the world into regions, each authoritative on its own
   thread/process, and stitch boundaries.

Data-parallel wins for the near term because:

- The per-entity work is already embarrassingly parallel — each entity owns its own
  `FlightIntegrator`, and the `SpatialIndex` + controllers are read-only mid-tick.
- One authoritative tick keeps a single consistent world: no cross-shard hand-off, no
  boundary-entity duplication, no per-shard snapshot stitching, no migration when an entity
  crosses a region edge.
- It composes with the existing per-peer interest management and delta compression unchanged.

Spatial sharding remains the **next scaling axis** if a single tick's parallel headroom is
exhausted; it is deliberately deferred (tracked as a contingent follow-on under Epic A).

## The `engine-job` library

A new, dependency-free (`namespace fl`, pure ISO C++20 stdlib) library: `engine/job/JobSystem.h`
+ `JobSystem.cpp`, linked **PUBLIC** by `engine-net`.

`class fl::JobSystem`:

- `explicit JobSystem(unsigned workerCount = 0)` — `workerCount` is the desired **total**
  parallelism including the calling thread. `0` = auto (`hardware_concurrency()`), `1` =
  serial/inline. Worker-count resolution is a pure free function
  `resolveWorkerCount(requested, detected)` so the auto/fallback branches are unit-testable
  without spawning threads.
- Background workers **block on a `condition_variable` while idle** — no busy-spin, so they do not
  steal CPU between sim ticks.
- `parallel_for(count, grain, fn)` splits `[0, count)` into chunks of up to `grain` indices,
  **claimed dynamically via a shared atomic cursor** (dynamic load-balancing — AI cost is
  heterogeneous: a Lua controller is far pricier than a `PeerController`). The **calling thread
  participates**, and the call blocks until all chunks finish. A `JobSystem(1)` has zero background
  workers and runs everything inline with no synchronisation. The first exception thrown by any
  chunk is captured and rethrown on the calling thread; the pool stays reusable.

No work-stealing in v1 — the dynamic cursor already balances heterogeneous chunk cost. A
work-stealing deque is a possible future optimisation if load-test profiling shows imbalance.

## The two-phase parallel tick

The original loop interleaved `sample()` then `step()` **per entity**. That cannot be parallelised
directly: a controller's `sample()` reads *other* entities' `EntityState.transform` (e.g.
`PursuitController` reads the target's position; `StateMachineController` conditions;
`LuaController.get_entity`), while `stepFlightSim` **writes** its own entity's transform — a
read/write data race on shared transforms.

`onTick` now splits the per-entity work into two passes over a gathered, contiguous range of the
live controlled entities (`WorldBroadcaster::runEntityPass`, which dispatches via the injected
`JobSystem` or runs inline):

1. **AI pass** — `inputs[i] = controller->sample(state, tick, dt, &spatialIndex)`. Read-only on all
   shared world state. Each controller reads a **consistent pre-step snapshot** of every entity's
   position; the `SpatialIndex` is read-only after the tick-start rebuild; each controller's own
   mutable state (`StateMachineController::m_dwellTime`, the per-instance `lua_State`) is
   per-entity / disjoint; `EntityManager::get()` reads stable pool slots (no reaping until the
   serial tail).
2. **Integrate pass** — `stepFlightSim(...)`. Each worker writes only its own entity's
   `FlightState` + `EntityState.transform`. `m_groundQuery` (`TerrainStreamer::heightAt`) is
   concurrent-read-safe via its `shared_mutex`.

The serial tail (`EntityManager::onTick` reap, weather, shutdown, `m_net.service`) is unchanged; the
snapshot serialize is itself a third parallel pass (below).

## Parallel snapshot assembly ([#512])

The Serialize phase builds one interest-managed, delta-compressed `MsgWorldSnapshot` per peer. Each
peer's build is **per-`peerId`-isolated**: it reads a shared read-only entity map (`snapMap`) + the
`SpatialIndex`, and writes only that peer's own state (`m_peerKnownGens[peerId]`,
`m_peerPendingDespawn[peerId]`, a per-peer buffer). It is therefore a natural third `parallel_for`,
over peers rather than entities (`WorldBroadcaster::runPeerPass`, grain 1 — each peer is a heavy,
heterogeneous-cost unit: a draw-distance interest query + priority/budget scheduler + quantized
bitstream encode). The phase is structured in three steps:

1. **Serial gather** (sim thread) — resolve each sending peer's stable per-peer state pointers once
   into a reusable `m_peerWork` vector. All `unordered_map::operator[]` insertions (and any rehash)
   happen here, so the parallel region sees a frozen map structure (and `unordered_map` keeps element
   pointers valid across later rehashes anyway). Decimated peers (the #518 send-rate gate) are
   excluded from the work set — a decimated tick mutates no per-peer state, exactly as the previous
   in-loop `continue` did.
2. **Parallel build** — `runPeerPass` over `m_peerWork`: each worker assembles its peer's snapshot
   into its own `buf` and mutates only that peer's private maps. Shared reads (`snapMap`,
   `SpatialIndex`, `EntityManager::get`, `m_drawDistanceM`, `m_snapshotBudgetBytes`,
   `m_schedulerWeights`, `m_congestionParams`) are read-only for the whole region; the hot-reloadable
   scalars are written only via `enqueueSimCallback`, which drains earlier in the same tick, so they
   are stable here. **No `m_net.send`** — the server `ENetHost*` is sim-thread-owned.
3. **Serial flush** (sim thread) — send each built `buf` and record the per-peer send-cadence
   bookkeeping (`lastSnapshotSentTick`/`sentSnapshot`) the decimation gate reads next tick.

### Determinism

The per-peer build performs no cross-peer writes and no RNG, so each peer's snapshot bytes are
**serial-equivalent across worker counts on a given binary** — `tests/test_world_broadcaster.cpp`
asserts byte-identical per-peer packet streams for `workerCount ∈ {1, 2, 8}` against the inline
reference (including a mid-run entity kill, exercising the parallel despawn-detection path), and
`tsan.yml` proves the region is race-free. Unlike the per-entity transforms, this is **not** a
cross-*platform* bit-identity claim: the priority/budget scheduler ranks candidates by floating-point
relevance scores (distance / closing-speed), whose rounding can differ across compilers/ISAs and
reorder ties. Parallelisation introduces no such divergence (the work is partitioned, not reduced);
cross-platform server determinism is irrelevant in an authoritative model where the server is the
sole source of truth (clients do not recompute snapshots). The **wire format is unchanged** — this
is a sim-thread-internal restructuring, byte-identical to the serial output.

> Possible future tuning lever (not yet needed, like "no work-stealing in v1"): `kPeerPassGrain` is
> 1 today; if load-test profiling ever shows a single huge interest query dominating a worker, the
> per-peer work could be split sub-peer (e.g. parallel interest query feeding a serial encode).

> Note: the previous serial loop iterated in `unordered_map` order, so it was *already*
> nondeterministic about which entities were pre/post-stepped when a controller sampled. The
> two-phase split replaces that with a well-defined consistent pre-step snapshot — strictly more
> deterministic.

### Data-safety fixes required for serial-equivalence

Three cross-entity writes in the old per-entity loop had to be removed so the parallel region
contains no shared writes:

- **Turbulence RNG** — was a single `m_turbRng` LCG mutated by every entity inside `stepFlightSim`
  (order-dependent shared write). Now seeded **per (entityIdx, tickIndex)** and advanced locally,
  so the perturbation is independent of evaluation order and identical across worker counts and
  platforms. This also removes the prior run-to-run nondeterminism.
- **Terrain-steer cache** (`m_entityX`/`m_entityZ`) — relaxed-atomic stores that the main thread
  reads to steer terrain streaming (only meaningful in the single-entity sandbox). Moved out of
  `stepFlightSim` into `updateTerrainSteerCache()`, run once after the integrate pass and storing
  the **lowest-index live entity** (a stable choice — `unordered_map` order is not).
- **Tick profiler scopes** — the per-entity `TickPhaseScope` calls mutated the profiler's per-tick
  scratch (a race under parallelism). Replaced by **one wall-clock scope around each whole pass**,
  which is also the more meaningful budget metric (wall-time per phase, not summed-across-cores
  CPU).

### Determinism

The parallel path is **serial-equivalent by construction**: the parallel region performs no
cross-entity writes; each entity integrates independently with no parallel floating-point
reduction; turbulence is a deterministic integer LCG. Results are therefore **bit-identical**
across worker counts and platforms. `tests/test_world_broadcaster.cpp` asserts exact equality of
final entity transforms for `workerCount ∈ {1, 2, 8}` against the inline reference, under Storm
turbulence; `tsan.yml` proves the region is race-free.

## Configuration

- `[world] sim_worker_threads` in `server.toml` — total sim-tick parallelism including the sim
  thread. `0` = auto, `1` = serial, `N` = fixed. Range `[0, 256]`. **A CPU-parallelism knob, not a
  capacity guarantee.**
- `fl-server --sim-worker-threads <n>` overrides the config (mirrors `--metrics-json`), so the
  load harness can sweep worker counts without rewriting config.
- Single-player: `LocalServer` launches the embedded `fl-server` with `--sim-worker-threads 1`
  (one entity needs no pool).

`fl-server` owns the `JobSystem` (constructed in `main.cpp` from the resolved config, outliving the
`GameLoop`) and injects it via `WorldBroadcaster::setJobSystem()` before `gameLoop.start()`. When no
`JobSystem` is injected (unit tests), both passes run inline.

## Platform notes

- ThreadSanitizer (the `tsan` preset + `tsan.yml`) is **Linux/macOS only** — MSVC has no TSan, so
  Windows correctness rests on the `ci.yml` MSVC build leg + the serial-equivalence test.
- `std::thread`/`hardware_concurrency` on Windows are limited to the current processor group
  (≤ 64 logical processors) without `SetThreadGroupAffinity`; a 128-core Windows host is
  under-utilised. Acceptable for v1 (Linux is the primary 128+ self-host target); tracked as a
  follow-on.
- No in-process thread pinning on any OS — CPU pinning stays external (`taskset` / the reference
  env).

## Graceful overrun handling ([#514])

When the authoritative tick can no longer complete inside its fixed-step budget (~16.667 ms at 60 Hz)
even at full parallelism, the server must **shed work and degrade gracefully** rather than spiral or
silently dilate time. This lands as a server-wide overrun **governor** plus an instrumented catch-up
backstop.

### The `TickGovernor` (`engine/net/TickGovernor.{h,cpp}`)

A pure AIMD policy with no glm / engine-entity deps, unit-tested in isolation exactly like
`CongestionController` / `SnapshotScheduler`. `WorldBroadcaster` owns one, steps it once per `onTick`
from the **previous tick's measured wall-time** (`TickProfiler::lastTotalMs()`) against the budget,
and freezes its lever values for the tick.

- **Control signal = an EWMA of per-tick wall-ms**, NOT the `TickProfiler`'s 60 s p99. p99 over the
  full window is far too laggy to *recover* from; the EWMA reacts in ~10 ticks and recovers
  symmetrically (the same reasoning as the congestion RTT baseline). p99 stays an operator-facing
  *display* metric.
- **AIMD:** every `evalIntervalTicks` (hysteresis) it classifies the tick — `ewma > budget·high`
  (0.90) → multiplicative decrease (`×0.7`); `ewma < budget·low` (0.60) → additive increase (`+0.125`);
  the dead-band between holds. A single `loadFactor ∈ [floor, 1]` results (1 = no degradation).
- **Three composing levers**, folded in on top of the per-client `CongestionController`:
  1. **Send-rate** — `snapshotIntervalTicks()`; the gather-time decimation gate uses
     `max(perPeerCongestionInterval, governorInterval)`, so a peer is decimated by whichever lever
     spaces it out more. (This is "reduce broadcast Hz", applied server-wide.)
  2. **Budget** — the static per-client byte budget is pre-scaled by `effectiveBudget()` before the
     per-peer congestion budget lever; the #516 scheduler then defers more low-relevance entities. No
     new encode path.
  3. **AI-sample stride** — `aiSampleStride()`; a *decimatable* (non-player) entity reuses its last
     sampled `ControlInput` on ticks where `(tickIndex + idx) % stride != 0`. This is the only lever
     that cuts the **AI phase**. The skip predicate is a pure function of `(idx, tick, stride)` and
     each worker writes only its own entity's cached input, so it is **serial-equivalent** across
     worker counts (`test_world_broadcaster` asserts bit-identical transforms for `{1,2,8}` under an
     over-budget clock) and TSan-clean.

A healthy server, a disabled governor, or any never-overrun tick holds `loadFactor == 1` → all three
levers are no-ops → **byte-for-byte the pre-#514 behaviour**.

> **Why the integrate pass is never decimated.** Physics uses a fixed `dt`; skipping integration steps
> destabilises the integrators and desyncs client prediction (which hardcodes 60 Hz). Only AI *sampling*
> is decimated — the entity still integrates every tick on its last command. If the integrate pass
> itself is the bottleneck, snapshot/AI shedding cannot help and the catch-up backstop (below) absorbs
> it as bounded time dilation; a substepping / LOD-physics lever for that case is a follow-on.

> **Why not auto-reduce the sim Hz.** Dynamically changing the physics tick rate would ripple into the
> client-prediction, jitter-buffer, and congestion math that all hardcode 60 Hz. Rejected; the governor
> sheds *work*, never the physics rate.

### The catch-up backstop (`GameLoop`)

`GameLoop` still caps catch-up at `max_catchup_ticks` (default 8, `[world]`-configurable) per loop
iteration — the spiral-of-death backstop. The cap + drop accounting is now a pure free function
`clampCatchupTicks(rawTicks, maxCatchup, &dropped)` (unit-testable without the sim thread, like
`resolveWorkerCount`), and the discarded count is exposed via `GameLoop::totalDroppedTicks()`.

### Observability

`WorldBroadcaster::getOverrunStatus()` publishes `loadFactor` / `snapshotIntervalTicks` / `aiStride`
as relaxed atoms (cross-thread-safe). The `status` and `tickstats` admin commands show them, and
`--metrics-json` (`ServerTickReport` schema v2) adds `load_factor` + `dropped_ticks`. fl-server logs a
`Warn` when `totalDroppedTicks()` rises (the sim is falling behind even after shedding).

### Configuration

`[world]` keys (all hot-reloadable via `reload_config` except `max_catchup_ticks`):
`overrun_governor_enabled` (default `true`), `overrun_high_watermark` (0.90), `overrun_low_watermark`
(0.60), `overrun_min_snapshot_hz` (15 → floor/interval cap), `overrun_max_ai_stride` (4),
`overrun_budget_floor_bytes` (400), `max_catchup_ticks` (8). `makeTickGovernorParams(...)` maps the
operator knobs to the controller internals, shared by startup + `reload_config`.

## Entity-pool + SpatialIndex scaling validation ([#573])

The per-tick data structures were characterised at thousands of entities (peers + AI) via the
`[world] test_spawn_ai_count` load-spawn affordance and the `entity-scale` sweep profile. Findings
(full matrix in [entity-scale-characterization.md](entity-scale-characterization.md)):

- The **entity pool and `SpatialIndex` are not the cliff** — `integrate`/`ai`/`collision` stay cheap
  even at 5000 entities; the dominant per-tick cost at scale is the **Serialize phase** (per-peer,
  interest-managed, budgeted snapshot build ∝ `clients × visible entities`), which the parallel
  per-peer pass ([#512]) scales down with worker count.
- Two tunings landed from this: `EntityPool::forEach` is now **O(liveCount)** (dense live-index list,
  no cost for dead slots under spawn/reap churn), and `SpatialIndex` gained a **configurable/auto cell
  size** (`[world] spatial_cell_size_km`; a cell far smaller than the draw distance explodes the query
  cell count) plus a **recycled-buffer `clear()`** (no per-tick bucket reallocation).
- The graceful degradation path for the serialize/AI-bound case is the [#514] overrun governor; the
  next scaling axis once a single tick is serialize-bound at max workers is spatial sharding ([#572]).

[#573]: https://github.com/fighters-legacy/fighters-legacy/issues/573
[#572]: https://github.com/fighters-legacy/fighters-legacy/issues/572

## Follow-ons

- **Collision phase** — collision detection (not yet implemented) is cross-entity and will need its
  own parallel-safe model rather than the per-entity disjoint-write pattern used here.
- **Re-evaluate the transport ceiling** after A/B raise the sim ceiling (feeds the transport
  spike).

## Validation runbook

On the 8-core reference-env (`tools/bot_swarm/reference-env/`, Release), sweep `idle`/`weave`/
`aggressive` at 96/128/160/192 clients, varying `--sim-worker-threads`, and read the authoritative
`server_tick` per-phase budget (`fl-server --metrics-json`, [#513]). Expect the Integrate+Ai **and
Serialize** wall-times to drop with worker count (the Serialize phase is now the parallel per-peer
snapshot build, [#512]) and the weave/aggressive tick-Hz knee to move right of the current
~96-client ceiling. See `docs/load-testing.md` for the methodology.

[#513]: https://github.com/fighters-legacy/fighters-legacy/issues/513
