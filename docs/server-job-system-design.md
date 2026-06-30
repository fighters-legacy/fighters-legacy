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

## Follow-ons

- **[#514] Graceful overrun handling** — today `GameLoop` caps catch-up at
  `kMaxTicksPerIteration = 8`. A `TickProfiler`-budget-driven degradation policy (shed/space-out
  snapshot cadence, reduce broadcast Hz, or drop time-rate when p99 tick-ms exceeds budget) is the
  planned graceful-degradation path.
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
