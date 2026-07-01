# Entity-pool + SpatialIndex scaling characterisation (#573)

Follow-on to Epic A ([#494](https://github.com/fighters-legacy/fighters-legacy/issues/494)). This
characterises the per-tick data structures — the `EntityManager` object pool and the `SpatialIndex`
— under entity counts well beyond the ~128-player `bot_swarm` range (peers + AI), and records the
tuning it justified. It feeds the Phase 4 128-client acceptance gate.

## How to reproduce

The load comes from a server-side **load-spawn affordance** (a *testing affordance, not a capacity
guarantee*): `[world] test_spawn_ai_count = N` pre-spawns N cheap loiter-AI entities spread over
`test_spawn_spread_km` at `test_spawn_agl_m`, exercising the spatial rebuild, interest queries, and
the parallel integrate pass without needing N real clients (`bot_swarm` is a pure client).

Sweep entity count × sim worker count and read the authoritative `server_tick` budget:

    # one-off cell
    FL_TEST_SPAWN_AI=2000 FL_SIM_WORKER_THREADS=4 \
        tools/bot_swarm/run_loadtest.sh build/release 64 30 weave -- --assert-min-entities 2000

    # full sweep (advisory, never baselined)
    python3 tools/bot_swarm/scale_gate.py --profile entity-scale --build-dir build/release

    # reference-env matrix (authoritative — 8-core/16 GB Release)
    ENTITY_COUNTS="0 2000 5000" SIM_WORKERS="1 4 8" PATTERNS=weave CLIENTS=64 \
        tools/bot_swarm/reference-env/run-container.sh

Isolated (machine-independent) microbenchmarks for the two structures in isolation:

    ctest --preset release -R test_entity_scale     # or: ./tests/test_entity_scale "[scale]"

## Results (local dev box, Release, 32 clients, weave, 8 s, governor OFF, draw distance 200 km,
snapshot budget 1200 B)

Indicative only — CPU timing is machine-dependent; the authoritative numbers come from the 8-core
reference-env. Per-phase columns are `server_tick` mean wall-ms; `p99` is total `tick_ms.p99`.

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
| 5000 | 8 | ~60 | — | — | — | — | — | — |

*(5000×8 not separately captured in this local run; consistent with the 5000×4 → 60 Hz recovery. The
5000×1 row shows the single-thread collapse — 36.8 Hz — where the harness stayed idle: the sim is
CPU-bound, not the transport.)*

Isolated microbench (Debug, same box): `EntityPool.forEach` over 2000 live entities in a
20 000-capacity (90 %-reaped) pool is ~0.01 ms — O(liveCount), independent of the dead-slot count.
`SpatialIndex.queryRadius` over a 200 km interest sphere at 5000 entities: ~0.06 ms (clustered, 10 km
cell), ~0.09 ms (distributed, 10 km cell), but **~6.4 ms with a 1 km cell** — a too-small cell makes
the query iterate ~160 000 mostly-empty cells.

## Findings

1. **The entity pool and SpatialIndex are NOT the cliff.** Across the whole matrix the integrate, AI,
   and collision phases stay sub-millisecond-to-low-single-digit; the pool rebuild + range queries are
   cheap. The dominant per-tick cost at scale is the **Serialize phase** — the per-peer,
   interest-managed, budgeted snapshot build (`clients × visible entities`).
2. **`EntityPool::forEach` is O(liveCount)** after the dense-iteration change — the per-tick spatial
   rebuild no longer pays for dead slots under spawn/reap churn.
3. **SpatialIndex cell size is a real knob.** A cell much smaller than the draw distance explodes the
   `queryRadius` cell count (the 6.4 ms case). The new configurable/auto cell size (`spatial_cell_size_km`,
   `0` = `clamp(drawDist/32, 500 m, 10 km)`) bounds a full-radius query to ~64² cells; recycled-buffer
   `clear()` removes the per-tick bucket reallocation.
4. **The data-parallel passes (#511/#512) carry the load.** Serialize drops 11.9→3.6 ms (2000) and
   22.4→9.1 ms (5000) from 1→8 workers; the 5000-entity single-worker collapse (36.8 Hz) recovers to a
   held 60 Hz at 4 workers.
5. **The graceful path is the overrun governor (#514)**, disabled here to measure raw capacity. Under
   an over-budget tick it sheds serialize/AI work (send-rate + budget + AI-stride levers) before the
   `GameLoop` catch-up cap engages — validated separately (`test_tick_governor`, `docs/load-testing.md`).

## Conclusion / follow-ons

At thousands of entities the bottleneck is snapshot serialization (peers × interest set), addressed by
the existing budget (#516), congestion (#518), and overrun-governor (#514) levers — not the pool or
index, which this work confirmed are cheap and now scale cleanly. If a single authoritative tick
becomes serialize-bound even at maximum worker count, **spatial sharding ([#572](https://github.com/fighters-legacy/fighters-legacy/issues/572))**
is the next scaling axis. No further pool/index restructuring is required beyond what landed here
(O(liveCount) iteration, configurable/auto cell size, recycled clear).
