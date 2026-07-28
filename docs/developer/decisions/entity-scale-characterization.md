# Entity-pool + SpatialIndex scaling characterisation (#573)

> **Frozen decision record.** This page records a decision as it was made and is not
> maintained against current behaviour. For how the engine works today, see the
> [Developer Guide](../index.md).

Follow-on to Epic A ([#494](https://github.com/fighters-legacy/fighters-legacy/issues/494)). This
characterises the per-tick data structures — the `EntityManager` object pool and the `SpatialIndex`
— under entity counts well beyond the ~128-player `bot_swarm` range (peers + AI), and records the
tuning it justified. It feeds the Phase 4 128-client acceptance gate.

**Transport note ([#773](https://github.com/fighters-legacy/fighters-legacy/issues/773)):** the
authoritative tables below were re-derived on **GameNetworkingSockets** — the default internet
transport — on the 8-core/16 GB reference VM. The original enet6 local-dev table is retained at the
end as the historical record; enet6 remains the LAN/single-player backend with its own regression
legs (`pr`, `reference-enet`).

## How to reproduce

The load comes from a server-side **load-spawn affordance** (a *testing affordance, not a capacity
guarantee*): `[world] test_spawn_ai_count = N` pre-spawns N cheap loiter-AI entities spread over
`test_spawn_spread_km` at `test_spawn_agl_m`, exercising the spatial rebuild, interest queries, and
the parallel integrate pass without needing N real clients (`bot_swarm` is a pure client).

Sweep entity count × sim worker count and read the authoritative `server_tick` budget (the
committed profiles pin `transport: gns`, so the build needs `FL_ENABLE_GNS=ON` — the default):

    # one-off cell
    FL_LOADTEST_TRANSPORT=gns FL_TEST_SPAWN_AI=2000 FL_SIM_WORKER_THREADS=4 \
        tools/bot_swarm/run_loadtest.sh build/release 64 30 weave -- --assert-min-entities 2000

    # full sweep (advisory, never baselined)
    python3 tools/bot_swarm/scale_gate.py --profile entity-scale --build-dir build/release

    # reference-env matrix (authoritative — 8-core/16 GB Release)
    ENTITY_COUNTS="0 2000 5000" SIM_WORKERS="1 4 8" PATTERNS=weave CLIENTS=64 \
        tools/bot_swarm/reference-env/run-container.sh

Isolated (machine-independent) microbenchmarks for the two structures in isolation:

    ctest --preset release -R test_entity_scale     # or: ./tests/test_entity_scale "[scale]"

## Results (8-core reference VM, Release, GNS, 64 clients, weave, 60 s, governor OFF, draw distance 200 km, snapshot budget 1200 B)

The authoritative matrix ([#773]). Per-phase columns are `server_tick` mean wall-ms; `p99` is total
`tick_ms.p99`; `tick Hz` is the observed minimum over the run.

| AI entities | sim workers | tick Hz | tick p99 (ms) | integrate | ai | collision | serialize | KB/s/cl |
|---|---|---|---|---|---|---|---|---|
| 0 | 1 | 60.0 | 2.37 | 0.038 | 0.001 | 0.000 | 1.202 | 72.2 |
| 0 | 2 | 60.0 | 1.38 | 0.034 | 0.033 | 0.000 | 0.716 | 72.0 |
| 0 | 4 | 60.0 | 0.97 | 0.033 | 0.046 | 0.000 | 0.459 | 72.2 |
| 0 | 8 | 60.0 | 0.82 | 0.034 | 0.061 | 0.000 | 0.365 | 72.2 |
| 500 | 1 | 60.0 | 10.18 | 0.217 | 0.041 | 0.000 | 7.078 | 74.6 |
| 500 | 2 | 60.0 | 6.42 | 0.161 | 0.049 | 0.000 | 4.046 | 74.6 |
| 500 | 4 | 60.0 | 3.54 | 0.101 | 0.055 | 0.000 | 2.264 | 74.6 |
| 500 | 8 | 60.0 | 2.57 | 0.089 | 0.079 | 0.000 | 1.522 | 74.6 |
| 2000 | 1 | 33.1 | 32.62 | 0.795 | 0.147 | 0.000 | 27.132 | 41.0 |
| 2000 | 2 | 59.2 | 22.52 | 0.491 | 0.116 | 0.000 | 15.420 | 73.4 |
| 2000 | 4 | 60.0 | 11.32 | 0.292 | 0.084 | 0.000 | 8.420 | 74.3 |
| 2000 | 8 | 60.0 | 7.58 | 0.233 | 0.100 | 0.000 | 5.230 | 74.4 |
| 5000 | 1 | 12.6 | 89.66 | 1.729 | 0.349 | 0.000 | 66.378 | 15.6 |
| 5000 | 2 | 23.8 | 47.18 | 1.089 | 0.249 | 0.000 | 36.868 | 29.3 |
| 5000 | 4 | 42.5 | 27.27 | 0.645 | 0.172 | 0.000 | 21.044 | 52.3 |
| 5000 | 8 | 60.0 | 18.65 | 0.514 | 0.148 | 0.000 | 13.645 | 73.9 |

The single-worker collapse curve extends cleanly past the matrix (32 clients, 30 s, workers = 1 —
the `overrun` profile's shape):

| AI entities | tick Hz | tick mean (ms) | integrate | ai | serialize |
|---|---|---|---|---|---|
| 5000 | 24.6 | 34.8 | 1.68 | 0.34 | 32.5 |
| 8000 | 14.6 | 53.5 | 2.62 | 0.54 | 49.9 |
| 12000 | 9.7 | 75.2 | 3.90 | 0.80 | 69.7 |
| 16000 | 7.3 | 95.0 | 4.98 | 1.05 | 88.0 |
| 20000 | 5.9 | 112.4 | 6.22 | 1.30 | 103.7 |

Even at 20 000 entities on one worker, integrate + AI + the spatial rebuild total under 8 ms — the
tick is drowning in serialize alone.

Isolated microbench (Debug, dev box): `EntityPool.forEach` over 2000 live entities in a
20 000-capacity (90 %-reaped) pool is ~0.01 ms — O(liveCount), independent of the dead-slot count.
`SpatialIndex.queryRadius` over a 200 km interest sphere at 5000 entities: ~0.06 ms (clustered, 10 km
cell), ~0.09 ms (distributed, 10 km cell), but **~6.4 ms with a 1 km cell** — a too-small cell makes
the query iterate ~160 000 mostly-empty cells.

## Findings

1. **The entity pool and SpatialIndex are NOT the cliff — on either transport.** Across the whole
   matrix the integrate, AI, and collision phases stay sub-millisecond-to-low-single-digit; the pool
   rebuild + range queries are cheap. The dominant per-tick cost at scale is the **Serialize phase**
   — the per-peer, interest-managed, budgeted snapshot build (`clients × visible entities`).
2. **At high entity counts, serialize is the engine's own encode/schedule cost, not the transport's.**
   [#649] showed that at 128 clients / ~128 entities, ~90 % of the enet6 serialize phase was ENet's
   *inline per-packet send* (GNS: 0.81 ms vs enet6: 8.03 ms). That finding does **not** extend to
   thousands of entities: at 5000 entities the GNS serialize phase is still 66 ms (64 clients,
   1 worker) — genuine per-peer interest + scheduling + encode work that scales with
   `clients × visible entities` and lands on the sim thread on every transport. The [#725]
   shared-encode ladder item is sized against *this* number, not the 0.8 ms low-entity one.
3. **`EntityPool::forEach` is O(liveCount)** after the dense-iteration change — the per-tick spatial
   rebuild no longer pays for dead slots under spawn/reap churn.
4. **SpatialIndex cell size is a real knob.** A cell much smaller than the draw distance explodes the
   `queryRadius` cell count (the 6.4 ms case). The new configurable/auto cell size (`spatial_cell_size_km`,
   `0` = `clamp(drawDist/32, 500 m, 10 km)`) bounds a full-radius query to ~64² cells; recycled-buffer
   `clear()` removes the per-tick bucket reallocation.
5. **The data-parallel passes (#511/#512) carry the load.** Serialize drops 27.1 → 5.2 ms (2000) and
   66.4 → 13.6 ms (5000) from 1 → 8 workers; the 5000-entity single-worker collapse (12.6 Hz at
   64 clients) recovers to a held 60 Hz at 8 workers.
6. **The graceful path is the overrun governor (#514)**, disabled here to measure raw capacity. At
   the `overrun` profile's load (32 clients, 5000 entities, 1 worker) the governor-off tick runs at
   24.6 Hz with ~1060 dropped ticks in 30 s; governor-on it sheds to `load_factor 0.25` /
   `interest_scale 0.5` and **holds 60 Hz with zero dropped ticks** (validated per-run by the
   `overrun` scale-gate profile; `test_tick_governor`, `docs/developer/load-testing.md`).

## Conclusion / follow-ons

At thousands of entities the bottleneck is snapshot serialization (peers × interest set), addressed by
the existing budget (#516), congestion (#518), and overrun-governor (#514) levers — not the pool or
index, which this work confirmed are cheap and now scale cleanly. If a single authoritative tick
becomes serialize-bound even at maximum worker count, **spatial sharding ([#572](https://github.com/fighters-legacy/fighters-legacy/issues/572))**
is the next scaling axis — the #572 spike resolved that contingency as **defer with an explicit
trigger criterion** (see [spatial-sharding-design.md](spatial-sharding-design.md)); this
characterisation's serialize-bound conclusion is its primary evidence. No further pool/index restructuring is required beyond what landed here
(O(liveCount) iteration, configurable/auto cell size, recycled clear).

## Follow-on: heavier AI mix + projectile churn ([#580](https://github.com/fighters-legacy/fighters-legacy/issues/580))

The matrix above deliberately isolated pool+index cost with **cheap static loiterers** — leaving the
AI phase and spawn/reap churn unstressed. #580 adds the affordances to load them:
`[world] test_spawn_ai_mix` (weighted loiter/pursuit/patrol controller mix; `patrol` is a
`StateMachineController` whose `AnyEntityWithinRange` transitions run `SpatialIndex::queryRadius()`
every tick) and `[world] test_projectile_rate`/`test_projectile_ttl_s` (short-lived spawn+reap churn
through the pool free-list, the O(liveCount) `forEach`, and the `SnapshotDespawn` TLV path). The
`entity-churn` scale-gate profile sweeps the representative combination
(`loiter:60,pursuit:25,patrol:15` + 120 spawns/s × 3 s TTL) over
`entity_spawn_counts × sim_worker_threads`.

Reference-environment matrix ([#773]; same rig as above — GNS, 64 clients, weave, 60 s; `entities`
is the steady-state population = pre-spawned + ~360 live projectiles):

| AI entities (nominal → steady) | sim workers | tick Hz | tick p99 (ms) | integrate | ai | serialize | maintenance |
|---|---|---|---|---|---|---|---|
| 500 → 926 | 1 | 60.0 | 18.47 | 0.257 | 0.055 | 12.559 | 0.077 |
| 500 → 926 | 8 | 60.0 | 4.03 | 0.095 | 0.076 | 2.630 | 0.096 |
| 2000 → 2362 | 1 | 26.7 | 47.37 | 0.814 | 0.202 | 33.174 | 0.092 |
| 2000 → 2362 | 8 | 60.0 | 9.80 | 0.253 | 0.109 | 7.053 | 0.136 |
| 5000 → 5362 | 1 | 11.2 | 108.89 | 1.804 | 0.497 | 74.056 | 0.205 |
| 5000 → 5426 | 8 | 56.4 | 21.18 | 0.545 | 0.186 | 15.445 | 0.136 |

Versus the same nominal points in the plain matrix: the mix moves `ai_ms` (+37 % at 2000×1 — the
`queryRadius` patrol path, as intended), churn lands mostly in serialize via the extra
snapshot-visible population (serialize +22 % at 2000×1, against +18 % population), maintenance grows
but stays trivially small, and the churn generator holds a stable population (no unbounded growth).
**Serialize remains dominant**, consistent with the conclusion above. The only cell that no longer
holds a clean 60 Hz at full workers is 5000×8 (56.4 Hz min, 206 drops over 60 s) — the churn tax at
the far corner of the matrix, and the point at which the governor (off here) would begin shedding in
production.

## Historical: original enet6 matrix (local dev box, Release, 32 clients, weave, 8 s)

The original #573 run, kept for the record. Indicative only — local box, enet6 transport (its
serialize column includes ENet's inline send; see [#649]/[#773]).

| AI entities | sim workers | tick Hz | tick p99 (ms) | integrate | ai | collision | serialize | KB/s/cl |
|---|---|---|---|---|---|---|---|---|
| 0 | 1 | 60.0 | 2.83 | 0.008 | 0.001 | 0.000 | 1.304 | 47.4 |
| 0 | 4 | 60.0 | 2.48 | 0.015 | 0.012 | 0.000 | 1.155 | 47.2 |
| 0 | 8 | 60.0 | 2.19 | 0.013 | 0.016 | 0.000 | 1.034 | 47.3 |
| 2000 | 1 | 59.6 | 19.97 | 0.444 | 0.079 | 0.000 | 11.869 | 65.3 |
| 2000 | 4 | 60.0 | 8.74 | 0.183 | 0.043 | 0.000 | 4.763 | 66.4 |
| 2000 | 8 | 60.0 | 6.42 | 0.151 | 0.040 | 0.000 | 3.621 | 66.7 |
| 5000 | 1 | 36.8 | 40.35 | 1.038 | 0.170 | 0.000 | 22.440 | 29.3 |
| 5000 | 4 | 60.0 | 15.89 | 0.428 | 0.087 | 0.000 | 9.147 | 66.8 |

[#649]: https://github.com/fighters-legacy/fighters-legacy/issues/649
[#725]: https://github.com/fighters-legacy/fighters-legacy/issues/725
[#773]: https://github.com/fighters-legacy/fighters-legacy/issues/773
