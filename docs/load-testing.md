# Load Testing — bot_swarm

`bot_swarm` is the headless multi-client load generator for fighters-legacy. It spins up N
synthetic game clients against a running `fl-server`, sustains realistic `MsgClientInput`, and
reports the client-observable metrics that define the **128+ player scale gate**. It is the
instrument for [#505](https://github.com/fighters-legacy/fighters-legacy/issues/505)
(characterise the `enet6` ceiling) and [#520](https://github.com/fighters-legacy/fighters-legacy/issues/520)
(the CI perf/soak gate), and part of Epic I of the
[128+ multiplayer re-target](architecture.md#decision-records).

It is the multi-client companion to [`net_check`](development.md#net_check) (single-peer RTT
bench); the two share the percentile math in `tools/common/NetStats.h`.

## What it measures

`bot_swarm` is a pure client — it cannot see the server's internals — so it reports what a real
client observes, which turns out to be exactly what the scale gate needs:

| Metric | Meaning |
|---|---|
| **observed server tick-Hz** | `(lastTick − firstTick) / elapsed` from snapshot `tickIndex` progression. The client-side **proxy** (used when no `--server-metrics` is wired): when the server falls behind, this sags below 60. Report the **min** across clients as the soak signal. Superseded by the authoritative `server_tick` block below when available. |
| **downstream KB/s per client** | Snapshot payload bytes per client per second — the per-client bandwidth the gate caps. |
| **RTT (ms)** | ENet round-trip estimate / `MsgPeerDelay` per client. |
| **connect time (ms)** | From connect issue to `onConnect` (includes the ramp queue). |
| **worker loop dt (ms)** | Harness work-time per tick. If it approaches the tick interval (16.7 ms @ 60 Hz) the **harness** is the bottleneck and the numbers are suspect. |
| clients connected / refused / disconnected | Admission + stability over the run. |

### Authoritative server tick budget (`server_tick`)

When `fl-server` is run with `--metrics-json PATH` (or `[metrics] tick_json_path`) it writes an
atomic per-phase tick-budget JSON every `tick_json_interval_ms` (default 1000; the runner uses
250). Point `bot_swarm --server-metrics PATH` at that file and the report gains an authoritative
`server_tick` sibling block (the client-side `observed_server_tick_hz` proxy is retained for
comparison). `bot_swarm` bumps `schema_version` to **2** when this block can be present, and to
**3** for the `transport` key ([#649]) — the backend the swarm *actually* spoke.

The same JSON shape is the standalone `--metrics-json` file and the embedded block:

| Field | Meaning |
|---|---|
| `schema_version` | server-tick report schema. **Frozen at `6` for the rest of primary development** (#686) — see below |
| `tick_hz` | actual recent tick rate over the sampling window (ring-derived) |
| `ticks_sampled` / `ticks_total` | ticks in the rolling window / monotonic all-time |
| `window_s` | wall-clock span of the sampling window |
| `peers` / `entities` | live peer count / live entity count at write time |
| `load_factor` | overrun-governor load factor `[floor, 1]`; `1` = no degradation (#514) |
| `interest_scale` | overrun-governor interest-radius scale `[fraction floor, 1]`; `1` = full radius (#726) |
| `dropped_ticks` | all-time `GameLoop` catch-up drops (sim overrun / time dilation) (#514) |
| `rss_kb` / `rss_startup_kb` | current process RSS (KiB) / RSS captured once after init; the soak leak gate tracks the delta (#707). `0` = unavailable on this platform |
| `congestion_min_send_hz` | all-time minimum across peers of the adaptive snapshot send rate (#518); `60` = the controller never engaged over the run (#714) |
| `congestion_recovered_send_hz` | max send rate observed since the minimum was set — recovery evidence after the link cleared (#714) |
| `congestion_max_loss` | all-time max sampled transport mean loss fraction across peers (diagnostic) (#714) |
| `wire_out_kbs` / `wire_in_kbs` / `wire_out_pps` | socket-level egress/ingress rates from `INetwork::getWireStats()` — framing, compression, and encryption included; the sample kept is the one at the highest peer count seen (#772) |
| `wire_peers` / `wire_out_kbs_per_client` | peer count the kept wire sample was taken at / egress wire KB/s divided by it — the number the **150 KB/s/client ceiling** gates (#772) |
| `tick_ms` | total `onTick` wall-time stats `{min,mean,max,p95,p99}` (ms) |
| `maintenance_ms` | rate-limit prune, idle timeout, admin drains, spatial rebuild, input drain, jitter resize |
| `sensing_ms` | cone + probability checks and contact-table assembly (`SensorSystem`, #685); staggered at `[world] sensor_check_hz`, so a single tick carries only its share of the window |
| `integrate_ms` | physics integration (`stepFlightSim`) summed across entities |
| `ai_ms` | controller `sample()` summed across entities |
| `collision_ms` | `EntityManager::onTick` (damage/collision/reap) |
| `serialize_ms` | telemetry + snapshot assembly/send + weather + shutdown notices |
| `other_ms` | `tick_ms − Σ(phases)` (loop/function overhead), clamped ≥ 0 |

> **`schema_version` is frozen at 6, and a new phase does not bump it (#686).** It was being bumped
> ritually and bought nothing: **nothing has ever gated on it** — fl-server writes it, `fromJson`
> parses it into a field, and no consumer compares it against anything. Both sides of the "contract"
> live in this repo and land in the same commit (fl-server writes the file; `bot_swarm` and
> `scale_gate.py` read it), so there is no third party to stay compatible with and no old reader in
> the wild to protect — the same reasoning that keeps `kProtocolVersion` at 1 through primary
> development. And the format is **additive and name-keyed**: `toJson`/`fromJson` iterate the phase
> table and every consumer looks fields up by name, so a new phase simply *appears* and an older
> reader keeps working (both properties are pinned by tests). Bump it only if a field's **meaning**
> changes under an unchanged name, or one is removed — the cases where a reader would silently
> misread a file rather than merely miss something. Near release, when metrics files start outliving
> the binary that wrote them, this becomes a real compatibility contract again.

The scale gate ([CI scale gate](#ci-scale-gate)) asserts on `server_tick.tick_ms.p99` via
`--assert-max-tick-ms` (strict tier only) and, in the `soak` profile, on RSS growth
(`rss_kb − rss_startup_kb`) via `--assert-max-rss-growth-kb` — a portable, hard-gated memory-leak
signal that replaces the shell `ps` sampler (#707).

## Scale-gate targets

128 clients @ 60 Hz with sim tick **≤ 16.6 ms p99** (observed tick-Hz ≈ 60) on a reference
**8-core / 16 GB** instance, sustained **≤ ~150 KB/s/client** downstream after Epic B
quantization + budgeting, soak-stable for 2 h. The thresholds live in
[`tools/bot_swarm/scale-gate.json`](../tools/bot_swarm/scale-gate.json) and are enforced by the
[CI scale gate](#ci-scale-gate); `bot_swarm` provides the measurement plus the `--assert-*` hooks
the gate forwards.

The snapshot quantization codec (#515), 3D interest culling (#402), and the priority/budget snapshot
scheduler (#516) have landed — the entity record is now bit-packed (~24 B steady-state vs. the former
fixed 64 B; see [snapshot-quantization.md](snapshot-quantization.md)), and each client's snapshot is
capped at `[world] snapshot_budget_bytes` (default 1200, 0 = unlimited). That budget is the operator
knob that trades `downstream_kbs_per_client` against per-frame fidelity at high player counts: lower it
to hold the ≤150 KB/s gate as population grows, at the cost of lower-priority entities updating less
often. Re-run the `downstream_kbs_per_client` sweep with `snapshot_budget_bytes = 0` (baseline) vs.
`1200` at 64 and 128 clients to quantify the reduction against the gate while watching
`--assert-max-tick-ms` p99.

## Quick start

The runner launches an `fl-server` with a load-test config and drives the swarm:

    cmake --build --preset debug --target fl-server bot_swarm
    tools/bot_swarm/run_loadtest.sh build/debug 128 30 weave
    # -> tools/bot_swarm/results/loadtest_128c_weave_<ts>.json

Or point `bot_swarm` at an already-running server:

    bot_swarm 127.0.0.1 4778 --clients 128 --duration 30 --pattern weave --json out.json

### Server config (required)

The connect-rate-limit and per-IP caps come **only** from `server.toml`. A load-test config
must raise them (the runner writes this automatically):

    [server]
    max_peers = 144                 # >= client count (validation ceiling is 1024)
    [security]
    connect_rate_limit_count = 100000   # the default 5 rejects a rapid ramp
    connect_rate_limit_window_s = 1
    max_connections_per_ip = 0          # all bots share 127.0.0.1

> **Capacity caveat:** raising `max_peers` to 1024 is a **testing affordance, not a capacity
> guarantee** — the server *accepting* 1024 does not mean it *handles* 1024. Real high-peer
> capacity is the Epic A/B/L work.

### Sweeping sim-tick parallelism (Epic A)

The server-side sim-tick CPU parallelism is set by `[world] sim_worker_threads` (0 = auto, 1 =
serial), overridable per-run with `fl-server --sim-worker-threads <n>` — distinct from the harness's
own `--threads`. To measure the data-parallel sim ([#511], [#512]), sweep `--sim-worker-threads`
(e.g. `1, 2, 4, 8`) at a fixed client count and pattern and watch the authoritative `server_tick`
block: the `integrate`, `ai`, **and `serialize`** per-phase wall-times should drop with worker count
(`serialize` is now the parallel per-peer snapshot build, [#512]), and the weave/aggressive tick-Hz
knee should move to higher client counts. Per-peer snapshot bytes are identical regardless of worker
count, so only throughput changes.

[#511]: https://github.com/fighters-legacy/fighters-legacy/issues/511
[#512]: https://github.com/fighters-legacy/fighters-legacy/issues/512

### Validating graceful overrun ([#514])

The `run_loadtest.sh`/`.ps1` scale-gate config sets `overrun_governor_enabled = false` so the gate
measures **raw** sim/bandwidth capacity against the committed baseline (an active governor would shed
work and mask regressions). To observe the governor itself, run a *separate* overload with it enabled
(its production default) — push past the tick-Hz knee (raise the client count, or pin few cores with
`taskset -c 0-3`) and watch the authoritative `server_tick` block and `status`/`tickstats`:

- `load_factor` falls below `1.0` and `status` shows `[DEGRADED]` as the EWMA tick-ms crosses the
  high-watermark;
- effective snapshot Hz drops (per-client KB/s falls) and the AI stride rises;
- `dropped_ticks` should stay **near zero** — the governor shed work *before* the `GameLoop`
  catch-up cap engaged. A rising `dropped_ticks` means the sim is integrate-bound (the one cost the
  governor cannot shed) and is dilating time, the honest worst case.

`reload_config` toggling `overrun_governor_enabled` mid-run flips the levers live (full rate ⇄
degraded), useful for an A/B in a single session.

**CI now automates this runbook** ([#574]). The `overrun` scale-gate profile runs the governor **on**
(`FL_LOADTEST_GOVERNOR=1`, wired from the profile's `governor: true`) and deterministically overloads a
serialize-bound tick with `test_spawn_ai = 5000` at `sim_workers = 1` — on **GNS**, the shipping
transport ([#773]): the governor-off probe on the 8-core reference box measured **24.6 Hz** there
(34.8 ms tick, serialize 32.5 ms — at this entity count serialize is the engine's own encode work,
not ENet's inline send, so the overload carries across transports; see
[entity-scale-characterization.md](entity-scale-characterization.md)). It asserts the governor
*responded*:

- `--assert-max-load-factor 0.99` fails if the governor **never engaged** (`load_factor` stayed `1.0` —
  the primary bite; the [#773] reference run measured `0.25` governor-on vs `1.0` governor-off, with
  `interest_scale` shed to its `0.5` floor);
- `--assert-max-dropped-ticks 100` is the **graceful-not-spiral** bound — a small allowance absorbs the
  one-time startup-spawn + EWMA-ramp transient before the governor engages; a genuine spiral produces
  orders of magnitude more (the governor-off probe dropped ~1060 ticks in just 30 s, while the
  governor-on reference run dropped **zero** in 90 s and held 60 Hz on a 13.5 ms shed tick).

Both use a **negative-disabled** sentinel (`< 0` = off) because `0` is a real value for each. The
profile is `baselined: false` — the shed KB/s must never touch the committed bandwidth baseline — and
runs in the **reference tier only** (nightly / `workflow_dispatch`), where the collapse margin is
reliable. A one-off run with `FL_LOADTEST_GOVERNOR=0` fails the load-factor assert, proving the gate
bites. There is no `taskset` dependency: entity-count overload is portable across runner core counts.

[#514]: https://github.com/fighters-legacy/fighters-legacy/issues/514
[#572]: https://github.com/fighters-legacy/fighters-legacy/issues/572
[#574]: https://github.com/fighters-legacy/fighters-legacy/issues/574
[#575]: https://github.com/fighters-legacy/fighters-legacy/issues/575
[#773]: https://github.com/fighters-legacy/fighters-legacy/issues/773

### Entity-pool + SpatialIndex scaling ([#573])

`bot_swarm` is a pure client, so `peers == entities` in a plain run. To stress the per-tick data
structures (the `EntityManager` pool + `SpatialIndex`) at **thousands** of entities, the server has a
load-spawn affordance — `[world] test_spawn_ai_count = N` pre-spawns N cheap loiter-AI entities over
`test_spawn_spread_km` at `test_spawn_agl_m`. **A testing affordance, not a capacity guarantee.**

The runner exposes it (and the worker sweep) via env, so you can sweep entity count × worker count and
read the authoritative `server_tick` per-phase budget:

    FL_TEST_SPAWN_AI=2000 FL_SIM_WORKER_THREADS=4 FL_SNAPSHOT_BUDGET=1200 \
        tools/bot_swarm/run_loadtest.sh build/release 64 30 weave -- --assert-min-entities 2000

- `FL_TEST_SPAWN_AI` / `FL_TEST_SPAWN_SPREAD_KM` → the `[world]` load-spawn keys.
- `FL_SIM_WORKER_THREADS` → `fl-server --sim-worker-threads` (sweep `1 2 4 8`).
- `FL_SNAPSHOT_BUDGET` → `[world] snapshot_budget_bytes` (0 = unlimited); sweep `1200` vs `0` to split
  the pool/index cost from the snapshot-budget cost.
- `--assert-min-entities N` fails the run if the server did not reach N live entities (the spawn took).

The whole matrix is a driver profile (advisory, **never baselined** — its sweep would corrupt the KB/s
baseline; pins `transport: gns` since [#773], and deliberately carries **no tick-ms assert**: the
collapsing single-worker cells are the characterisation, not a regression):
`python3 tools/bot_swarm/scale_gate.py --profile entity-scale --build-dir build/release`,
or the reference-env sweep `ENTITY_COUNTS="0 2000 5000" SIM_WORKERS="1 4 8" … run-container.sh`. The
`spatial_cell_size_km` knob (`0` = auto from draw distance) tunes the index cell size — a cell much
smaller than the draw distance explodes the `queryRadius` cell count. What to watch in `server_tick`:
`serialize_ms` (the per-peer snapshot build — dominant at scale), `integrate_ms`/`ai_ms` (parallel
passes; should fall with worker count), and `maintenance_ms` (the spatial rebuild). Findings and the
run matrix live in [entity-scale-characterization.md](entity-scale-characterization.md).

### Heavier AI mix + projectile churn ([#580])

The #573 load-spawn deliberately uses **cheap static loiterers** (isolating pool+index cost), which
leaves the AI phase and spawn/reap churn unstressed. Two additive knobs fix that:

- `FL_TEST_SPAWN_MIX="loiter:60,pursuit:25,patrol:15"` → `[world] test_spawn_ai_mix`: a weighted
  controller mix for the pre-spawned entities (deterministic per-index assignment). `pursuit` does an
  `EntityManager::get()` on a moving target each tick; `patrol` is a `StateMachineController` whose
  `AnyEntityWithinRange` transitions run `SpatialIndex::queryRadius()` **every tick** — the expensive
  AI path, and the one the overrun governor's AI-sample stride (#514) decimates.
- `FL_TEST_PROJECTILE_RATE=120` (+ `FL_TEST_PROJECTILE_TTL_S=3.0`) → `[world] test_projectile_rate` /
  `test_projectile_ttl_s`: a projectile-churn generator spawning short-lived entities at that rate,
  each killed after the TTL (steady-state extra population ≈ rate × ttl) — sustained `EntityPool`
  alloc/free, O(liveCount) `forEach` under a fragmented store, and `SnapshotDespawn` TLV traffic.

The `entity-churn` driver profile runs the representative combination over the
`entity_spawn_counts × sim_worker_threads` sweep (advisory, never baselined):
`python3 tools/bot_swarm/scale_gate.py --profile entity-churn --build-dir build/release`. Compare its
`ai_ms` / `collision_ms` / `maintenance_ms` against the same points in the plain `entity-scale`
matrix. Indicative debug-build deltas at 2000 entities / 1 worker (mix + 120/s churn vs. all-loiter):
`ai_ms` +12%, `maintenance_ms` +17%, `serialize_ms` +13% (the ~360 extra churned entities are
snapshot-visible), `tick_ms` +11% — reference-environment numbers belong in
[entity-scale-characterization.md](entity-scale-characterization.md).

[#573]: https://github.com/fighters-legacy/fighters-legacy/issues/573
[#580]: https://github.com/fighters-legacy/fighters-legacy/issues/580

## CLI

    bot_swarm [host] [port] [options]
      --clients N            synthetic clients (default 32)
      --duration S           soak seconds (default 30)
      --rate HZ              MsgClientInput rate per client (default 60)
      --transport NAME       enet|gns (default enet). gns needs an FL_ENABLE_GNS=ON build and an
                             fl-server on --transport gns; it never falls back silently (#649)
      --ramp-ms MS           delay between successive connects (default 20)
      --threads N            worker threads (default auto = min(cores, ceil(clients/32)))
      --pattern NAME         weave | level | aggressive | idle | random | trace:<file> (default weave)
      --pattern-mix SPEC     weighted mix, e.g. "weave:80,aggressive:20" (supersedes --pattern)
      --json PATH            write a JSON report
      --server-metrics PATH  read fl-server --metrics-json file; embed authoritative server_tick block
      --assert-min-tick-hz X exit nonzero if observed (proxy) tick-Hz min < X
      --assert-max-kbs Y     exit nonzero if downstream KB/s/client max > Y
      --assert-max-tick-ms X exit nonzero if authoritative server tick p99 (ms) > X
      --assert-min-entities N exit nonzero if authoritative server_tick.entities < N
      --assert-max-rss-growth-kb N  exit nonzero if server RSS growth (rss_kb - rss_startup_kb) > N
      --assert-max-load-factor X  exit nonzero if server_tick.load_factor > X (governor engaged? <0 = off)
      --assert-max-dropped-ticks N  exit nonzero if server_tick.dropped_ticks > N (<0 = off)
      --degrade-duration S   lossy-proxy degraded window length; enables the proxy (default 0 = off)
      --degrade-start S      seconds from proxy start until the window opens (default 10)
      --degrade-loss F       drop fraction while degraded, [0,1] (default 0)
      --degrade-delay-ms N   added one-way delay while degraded (default 0)
      --assert-congestion-engaged-hz X    exit nonzero if congestion_min_send_hz > X (0 = off)
      --assert-congestion-recovered-hz Y  exit nonzero if congestion_recovered_send_hz < Y (0 = off)
    Env: FL_HOST, FL_PORT

## Flight patterns

Input is pluggable via `fl::IFlightPattern` (each client owns an instance). Built-ins stress
different parts of the server:

- **weave** — gentle turn/climb, per-client phase; entities spread out and move (physics +
  interest management).
- **level** — straight-and-level; near-idle movement (baseline/delta snapshotting).
- **aggressive** — high-rate rolls/pulls + afterburner; max entity churn (physics + snapshot size).
- **idle** — no input; pure connection + snapshot overhead.
- **random** — seeded per-client walk; heterogeneity.
- **trace:`<file>`** — replays a recorded real session (see [Trace replay](#trace-replay-560));
  reproduces real player behaviour at scale.

Adding a built-in pattern is a new `IFlightPattern` subclass + a branch in `makePattern()` — no
harness changes.

### Weighted pattern mix (#560)

A single pattern makes every client fly identically. `--pattern-mix` builds a heterogeneous swarm:

    bot_swarm 127.0.0.1 4778 --clients 100 --pattern-mix "weave:80,aggressive:15,idle:5"

Weights are positive integers over built-in pattern names; the assignment is **deterministic** —
client _i_ of _N_ maps to the cumulative-weight bucket containing `floor(i * totalWeight / N)`, so
the counts match the weight fractions with no RNG and reproduce across runs and platforms. The mix
spec supersedes `--pattern` and is echoed in the report's `pattern` field.

### Trace replay (#560)

`--pattern trace:<file>` replays a **recorded** input stream instead of a synthetic one, so the
harness reproduces real player behaviour (including from live multiplayer) at scale.

Traces are recorded **server-side**: set `[trace] input_trace_dir` in `server.toml` (or run the
`trace_start [dir]` / `trace_stop` admin commands), and the server appends every peer's *accepted*
(post-validation) `MsgClientInput` to a per-peer file `trace_peer<id>_<n>.flit` in that directory.
The trace is loaded once and shared read-only across all synthetic clients; each client offsets its
playback cursor by its index (so the swarm doesn't fly in lockstep) and loops at the end.

    # 1. record a real session
    #    server.toml:  [trace]\n    input_trace_dir = "traces"
    # 2. replay it at scale
    bot_swarm 127.0.0.1 4778 --clients 128 --pattern trace:traces/trace_peer0_0.flit

**FLIT trace format** (little-endian, versioned so the Phase 4 replay epic #588 can extend rather
than fork it; codec in `engine/net/InputTrace{Format,Writer,Reader}.h`):

| Section | Bytes | Fields |
| --- | --- | --- |
| Header | 10 | magic `"FLIT"` (4) · version `u16` (=1) · tickRate `u32` |
| Record | 28 | serverTick `u64` · throttle `f32` · elevator `f32` · aileron `f32` · rudder `f32` · buttons `u32` |

Records follow the header back-to-back; the record count is `(fileSize − 10) / 28`. The five
control fields map 1:1 onto `MsgClientInput`'s flight-control fields and onto the harness's
`BotControl`. `serverTick` is the authoritative tick at which the input was accepted (for
deterministic replay in #588); the load harness keys playback off wall-time × `tickRate`.

## Characterisation runbook (for #505)

To find a transport's client-count ceiling, sweep the client count under a fixed pattern and watch
the **observed server tick-Hz min** fall away from 60 (set `FL_LOADTEST_TRANSPORT=gns` to sweep the
shipping transport; the bare invocation sweeps enet6, the original #505 instrument):

    for n in 32 64 96 128 160 200 256; do
      tools/bot_swarm/run_loadtest.sh build/debug "$n" 30 weave
    done

The knee — where tick-Hz sags and the max snapshot gap spikes — is the ceiling under that load
profile. Run it for `idle` (overhead floor) and `aggressive` (worst case) too. Record the
reference machine spec alongside the numbers.

## Validating congestion response (#518 / #714)

Loopback has zero loss, so the per-client congestion controller never triggers in a normal run — a
clean run is exactly the no-regression baseline (`downstream_kbs_per_client` and held 60.0 Hz must be
unchanged from before #518). To exercise the back-off you have to degrade the link.

**CI automates this via the built-in lossy proxy** ([#714]). `bot_swarm --degrade-*` routes the
synthetic clients through a local UDP relay (`LossyProxy`, portable — no `NET_ADMIN`, no `tc`) that
drops a fraction of datagrams and adds one-way delay inside a scheduled window, then runs clean so
the controller can recover:

    bot_swarm 127.0.0.1 4778 --clients 16 --duration 90 \
      --degrade-start 25 --degrade-duration 30 --degrade-loss 0.05 --degrade-delay-ms 100 \
      --server-metrics tick.json \
      --assert-congestion-engaged-hz 30 --assert-congestion-recovered-hz 55

The server tracks **run-long watermarks** (schema v5: `congestion_min_send_hz`,
`congestion_recovered_send_hz`, `congestion_max_loss` — frozen once the load clients disconnect, so
the single end-of-run metrics read still carries the evidence), and the two asserts express
*engaged-then-recovered*: the min send rate must have fallen to ≤ the engaged threshold during the
window (stuck at 60 = the controller never responded), and must have climbed back to ≥ the recovered
threshold afterwards. The `congestion` scale-gate profile (reference/nightly tier, `baselined: false`)
runs exactly this — **on GNS, the shipping transport, since [#773]**: the proxy is a plain UDP
datagram relay, so it degrades GNS traffic the same way, and GNS feeds the controller real link stats
(`SteamNetConnectionRealTimeStatus_t` ping / connection quality via `getPeerLinkStats`), so the
RTT-over-baseline trigger sees the window exactly as it saw ENet's `roundTripTime` — verified on the
reference box: engaged to the 10 Hz controller floor inside the window, recovered to 60 Hz after it.
A healthy-link run with the same asserts **fails** the engaged gate (verified — the gate bites).
Tuning notes: the **+100 ms delay is the deterministic engage trigger** (+200 ms RTT over the
controller's 40 ms margin — neither transport's loss metric responds much to datagram drop on this
mostly-unreliable workload: ENet only counts unACKed *reliable* packets, GNS reports a smoothed
connection-quality fraction), and the loss fraction is kept at 5% — a 15% dev run tripped ENet's own
peer timeout and dropped a client, failing admission.

The manual `netem` route remains available for ad-hoc experiments (`sudo tc qdisc add dev lo root
netem loss 5% delay 80ms`, restore with `... del ...`; macOS: `dnctl`/`pfctl`; Windows: `clumsy`),
and the deterministic AIMD logic itself is covered by `test_congestion_controller` + the
`test_world_broadcaster` `[congestion]` watermark tests. See
[docs/congestion-control-design.md](congestion-control-design.md).

[#714]: https://github.com/fighters-legacy/fighters-legacy/issues/714

## The transport the gate measures (#649, #773)

`bot_swarm` defaults to **enet6** and always will: it is the enet6 regression instrument. But
**GameNetworkingSockets is the default internet transport** ([#507]) — the one most players actually
use — so since [#773] every headline and characterisation profile pins `transport: gns` and **the
published numbers describe what ships**: `reference` (the primary 128-client profile), `soak`,
`overrun`, `congestion`, `entity-scale`, and `entity-churn` all run GNS on both ends. enet6 keeps two
regression legs: `pr` (every PR, hosted runner) and `reference-enet` (128 clients on the strict tier,
mirroring `reference` so the transports stay directly comparable).

`--transport gns` points both ends at GNS:

    FL_LOADTEST_TRANSPORT=gns ./tools/bot_swarm/run_loadtest.sh build/release 128 60 weave
    python3 tools/bot_swarm/scale_gate.py --profile reference --build-dir build/release --strict

It requires an `FL_ENABLE_GNS=ON` build (the default). **A GNS run can never silently degrade into an
enet6 run** — that would be a gate that lies, which is worse than no gate — so three independent
things prevent it:

1. `bot_swarm` **refuses** `--transport gns` in an enet6-only build (exit 2) instead of taking
   `createNetwork`'s convenience fallback.
2. The report records the backend actually spoken (`"transport"`, schema v3), and `scale_gate.py`
   fails the run if it does not match the profile.
3. The workflow asserts `FL_ENABLE_GNS:BOOL=ON` in `CMakeCache.txt` after configure — because
   `cmake/dependencies.cmake` *silently* force-disables GNS when OpenSSL/protobuf are missing (the
   same trap [#653] hit). The reference runner installs them via `reference-env/vm-provision.sh`.

Baselines are keyed **per profile** (`reference/*` = GNS, `reference-enet/*` = enet6), so each
transport diffs only against itself — which the wire-byte baseline needs, because the two transports
legitimately put very different byte counts on the wire for identical payload ([#772]; the payload
baseline agrees across transports by construction). Bandwidth is hard-gated by the absolute
150 KB/s/client **wire** ceiling on both.

### Measured: GNS vs enet6 at 128 clients

Same box (8-core reference VM), same build, same session, Release, 60 s per pattern (the [#773]
re-derivation run; the original [#649] session measured the same values to within 0.3 ms):

| Pattern | enet6 tick p99 | **GNS tick p99** | payload KB/s (both) | Admission |
|---|---:|---:|---:|---|
| idle | 5.53 ms | **1.55 ms** | 71.4 | 128/128 both |
| weave | 13.07 ms | **1.54 ms** | 71.9 | 128/128 both |
| aggressive | 13.15 ms | **1.84 ms** | 73.4 | 128/128 both |

**GNS cuts server tick p99 by ~4–8×.** The reason is *where the packet work happens*: ENet's `send()`
does its per-packet work inline on the calling thread, so it lands inside the sim tick's **serialize**
phase; GNS's `SendMessageToConnection` queues to its own internal service thread and returns. The
sim thread stops paying for the packet pump.

Phase breakdown makes it concrete (weave, 128 clients, same box):

| | tick mean | serialize | integrate | serialize share |
|---|---:|---:|---:|---:|
| enet6 | 8.34 ms | 8.16 ms | 0.06 ms | 98 % |
| GNS | 1.00 ms | 0.79 ms | 0.05 ms | 79 % |

**~90 % of what the `serialize` phase cost was ENet's inline send, not our snapshot pipeline** — whose
real cost is ~0.8 ms at 128 clients. Integrate is unchanged, as it must be (transport-independent).

**That 90 % finding is a low-entity-count statement.** At thousands of AI entities the serialize
phase is dominated by the engine's own per-peer interest + scheduling + encode work
(`clients × visible entities`), which lands on the sim thread on *every* transport — at 5000 entities
/ 64 clients / 1 worker the GNS serialize phase is still 66 ms. The [#773] re-derivation of the
[#573] matrix on GNS, including the collapse curve out to 20 000 entities, lives in
[entity-scale-characterization.md](entity-scale-characterization.md); the [#572] / [#575] trigger
criteria carry the GNS-derived magnitudes in their design records.

Two honest caveats:

- **The KB/s figures cannot show GNS's wire overhead.** `downstream_kbs_per_client` counts
  *application* snapshot payload bytes, not wire bytes — so identical KB/s is expected, and is *not*
  evidence that encryption and framing are free. That is what `wire_out_kbs_per_client` measures
  (next section).
- **Loopback.** The tick win is real and mechanical (work moved off the sim thread), but the absolute
  numbers come from a loopback run on one box. GNS also spends CPU on its own service threads, which
  an 8-core box pays for somewhere; the tick thread simply stops paying it.

RTT reads 0 ms on loopback under GNS — that is honest, not broken: GNS reports integer-millisecond
ping and loopback is sub-millisecond (enet6's coarser estimator floors near 16 ms). Through the
lossy proxy's +50 ms one-way delay, GNS correctly reports 100 ms.

[#649]: https://github.com/fighters-legacy/fighters-legacy/issues/649
[#653]: https://github.com/fighters-legacy/fighters-legacy/issues/653
[#507]: https://github.com/fighters-legacy/fighters-legacy/issues/507

## Wire bytes vs payload bytes (#772) — read this before quoting a bandwidth number

There are **two** bandwidth numbers and they are not interchangeable:

| Metric | What it counts | Transport-dependent? |
|---|---|---|
| `downstream_kbs_per_client` | **Application** snapshot payload the client received | **No** — identical on enet6 and GNS by construction |
| `server_tick.wire_out_kbs_per_client` | **Socket** bytes the server actually sent — transport framing, ENet's range-coder compression, GNS's AES-GCM overhead | **Yes** |

Payload KB/s is the right signal for *protocol* regressions (a snapshot that got fatter), and it is
what the committed `kbs` baseline tracks. But it **cannot see what a transport costs to run** — which
is the number an operator's bandwidth bill is denominated in. That is `wire_kbs`, and it is the
**hard 150 KB/s/client ceiling** the gate enforces.

### Measured: enet6 vs GNS at 128 clients (8-core reference box)

| | payload KB/s/cl | **wire KB/s/cl** | vs payload | datagrams/s |
|---|---:|---:|---:|---:|
| enet6 / idle | 71.41 | **17.56** | −75.4 % | 7 884 |
| enet6 / weave | 71.94 | **58.89** | −18.1 % | 7 926 |
| enet6 / aggressive | 73.24 | **63.02** | −14.0 % | 7 884 |
| GNS / idle | 71.42 | **75.64** | +5.9 % | 8 456 |
| GNS / weave | 71.93 | **78.00** | +8.4 % | 13 089 |
| GNS / aggressive | 73.40 | **80.45** | +9.6 % | 15 319 |

**GNS puts 1.3×–4.3× more bytes on the wire than enet6 for identical application payload**, for two
independent reasons: `ENetNetwork` enables ENet's **range coder** (`enet_host_compress_with_range_coder`)
and **GNS does not compress at all** — it encrypts; and GNS sends ~1.8× the **datagrams** on active
patterns, paying per-packet framing more often. Idle traffic is the extreme case: highly repetitive
snapshots compress 75 % on enet6 and 0 % on GNS.

Note the two rows are the same engine and the same snapshots — **the payload columns agree to within
0.1 KB/s**. That agreement is not a result; it is the tautology that made this cost invisible until
wire bytes were counted.

### Engine-layer snapshot compression closed the gap ([#775])

`[network] compress_snapshots` (default **on**) zstd-compresses each snapshot body at the engine
layer — transport-agnostic, so a transport swap cannot lose it. Measured on the reference VM at
128 clients, compression off → on, **GNS**:

| Pattern | wire KB/s/cl | datagrams/s | serialize mean | payload KB/s/cl |
|---|---:|---:|---:|---:|
| idle | 75.5 → **16.9** (−78 %) | 8 438 → 7 639 | 0.77 → 0.87 ms | 71.4 → 14.4 |
| weave | 77.5 → **66.5** (−14 %) | 12 156 → **7 673** | 0.83 → 0.94 ms | 71.8 → 60.9 |
| aggressive | 80.4 → **67.8** (−16 %) | 15 315 → **7 681** | 1.56 → 0.99 ms | 73.5 → 63.0 |

- **GNS idle now beats enet6's compressed wire** (16.9 vs 17.6 KB/s) — the 4.3× headline gap is
  gone; the active-pattern gap closes from ~1.3× to ~1.1× (GNS still pays AES-GCM + framing that
  no codec can remove).
- **The ~1.8× datagram multiplier was MTU fragmentation, not ack overhead**: ~1.2 KB snapshots +
  GNS framing straddled the 1300-byte MTU on active patterns; compressed snapshots fit one
  datagram, so pps collapses to the 60 Hz × peers data floor — *below* enet6's. The
  `[network] gns_nagle_time_us` coalescing knob therefore ships defaulted to GNS's own 5 ms
  (`0` = untouched): with fragmentation gone there is nothing left to coalesce, and raising it
  would only add delivery latency.
- **The CPU cost is ~0.1 ms of `serialize_ms`** at 128 clients across 8 workers (≈0.6 % of the
  16.6 ms budget) — bandwidth bought, not paid for out of tick time. The congestion profile
  engages and recovers identically with compression on.
- **enet6 is the one caveat**: its range coder cannot compress zstd output, and its whole-packet
  compression was slightly better than zstd's payload-only form — enet6 weave wire measured
  58.7 → 64.9 KB/s (+10 %) with engine compression on. The default stays on (GNS is what ships,
  and enet6 is the LAN/loopback backend where wire bytes matter least); a bandwidth-sensitive
  enet6 operator can set `compress_snapshots = false` and let the range coder do the work. The
  runners expose `FL_LOADTEST_COMPRESSION=0` for raw A/B legs, and the committed `wire_kbs`
  baselines need regenerating on the reference runner now that the production default changed
  the byte profile of every leg.

### How it is measured

`INetwork::getWireStats()` returns **rates**, not cumulative counters — the honest intersection of
what the backends can report: GNS exposes per-connection rates (`m_flOutBytesPerSec`) and no lifetime
totals, ENet exposes cumulative host totals from which a rate is a clean delta
(`platform/net/WireRateSampler.h`, wrap-safe: ENet's `uint32` counters wrap roughly every 8 minutes at
128 clients, so a soak run wraps repeatedly).

`WorldBroadcaster` samples every 30 ticks on the sim thread and publishes to relaxed atomics; the
sample kept is the one taken at the **highest peer count** seen, and `wire_peers` travels with it.
That matters: fl-server keeps rewriting `--metrics-json` while the swarm ramps up and drains away, and
the gate reads only the final snapshot — so a live rate reports an *idle* server (0 peers → 0 KB/s),
and even "the last sample with any peers" catches the disconnect drain (a 16-client run reported its
wire rate at 2 peers). The per-client figure divides by the peers that produced the traffic, never by
whoever happens to be connected when the file is written. (Same class of trap the [#714] congestion
watermarks freeze to avoid.)

[#772]: https://github.com/fighters-legacy/fighters-legacy/issues/772
[#775]: https://github.com/fighters-legacy/fighters-legacy/issues/775

## Platform notes

- **macOS / Linux:** each client is a UDP socket; `bot_swarm` raises `RLIMIT_NOFILE` and the
  runner bumps `ulimit -n`. For high counts raise UDP buffers (`net.core.rmem_default` on Linux,
  `net.inet.udp.recvspace` on macOS) or RTT/bandwidth numbers skew.
- **Windows:** the run raises the timer resolution (`timeBeginPeriod(1)`) so 60 Hz pacing is
  accurate.

## CI scale gate

The smoke layer: the Linux/macOS "Smoke test tools" CI step runs `run_loadtest.sh build/debug 8 3
weave` and fails if any of the 8 clients are refused or dropped. The pure-logic unit tests
(`test_bot_swarm`, `test_scale_gate.py`) run on all platforms including Windows.

The gate layer is [`.github/workflows/scale-gate.yml`](../.github/workflows/scale-gate.yml), driven
by [`tools/bot_swarm/scale_gate.py`](../tools/bot_swarm/scale_gate.py) reading thresholds from
[`scale-gate.json`](../tools/bot_swarm/scale-gate.json). The driver runs `run_loadtest.sh`/`.ps1`
once per pattern with the profile's `--assert-*` flags wired in (a distinct port per pattern avoids
the UDP rebind race; the report path is pinned via `FL_LOADTEST_REPORT`), evaluates each report,
checks the machine-independent `downstream_kbs_per_client` against a committed baseline, and writes a
Markdown summary to `$GITHUB_STEP_SUMMARY`.

**Two tiers, split by runner — hosted for PRs, the self-hosted reference VM for strict:**

| Tier | Trigger | Profile | Hard gates | Advisory |
|---|---|---|---|---|
| **PR** | every PR + push to `main` (Linux, Release) | `pr` (64 clients, weave, **enet6** — the regression instrument) | wire bandwidth ≤150 KB/s/client, admission (no refused/dropped), KB/s + wire baseline regression, tick-Hz collapse tripwire (≥30) | tick-ms p99 (disabled) |
| **Reference** | manual `workflow_dispatch` on the self-hosted `fl-reference` runner | `reference` (128 clients; idle/weave/aggressive; **both ends GNS** — the shipping transport, [#773]) | transport identity + wire bandwidth + admission + baselines + **tick-ms p99 ≤16.6 (`--strict`, unconditional)** | — |
| **Reference-enet** | manual `workflow_dispatch` (`profile=reference-enet`, or `nightly` set) on `fl-reference` | `reference-enet` (128 clients; idle/weave/aggressive; **enet6** — the LAN/single-player backend's full-scale regression leg, [#773]) | same gates as Reference, on its own baseline keys | — |
| **Soak** | manual `workflow_dispatch` (`profile=soak`) on `fl-reference` | `soak` (128 clients, weave, 2 h, **GNS**) | strict gates + RSS-growth leak (`--assert-max-rss-growth-kb`, from the server's self-reported `rss_kb`, [#707](https://github.com/fighters-legacy/fighters-legacy/issues/707)) | — |
| **Overrun** | manual `workflow_dispatch` (`profile=overrun`, or `nightly` set) on `fl-reference` | `overrun` (32 clients, weave, governor **on**, `test_spawn_ai=5000` @ `sim_workers=1`, **GNS**) | governor engaged (`--assert-max-load-factor 0.99`) + graceful-not-spiral (`--assert-max-dropped-ticks 100`) + admission; **not baselined** ([#574](https://github.com/fighters-legacy/fighters-legacy/issues/574)) | — |
| **Congestion** | manual `workflow_dispatch` (`profile=congestion`, or `nightly` set) on `fl-reference` | `congestion` (16 clients, weave, lossy proxy: 5% loss + 100 ms delay in [25 s, 55 s), **GNS**) | controller engaged (`--assert-congestion-engaged-hz 30`) + recovered (`--assert-congestion-recovered-hz 55`) + admission; **not baselined** ([#714](https://github.com/fighters-legacy/fighters-legacy/issues/714)) | — |

The PR tier hard-gates only machine-independent metrics: `bot_swarm`'s `--assert-min-tick-hz` reads
the *client-side proxy*, which sags when the harness itself is CPU-starved on a shared runner — a
false failure. So tick-Hz is only a total-collapse tripwire and tick-ms is advisory on PRs. The
strict `16.6 ms` p99 is meaningful only on the 8‑core/16 GB
[reference-env](../tools/bot_swarm/reference-env/README.md), so it is enforced there: the
[strict tier](../tools/bot_swarm/reference-env/README.md#self-hosted-reference-runner-ci-strict-tier)
runs on a self-hosted runner registered on that VM and is triggered **manually** via
`workflow_dispatch` (`gh workflow run scale-gate.yml -f profile=reference`), with `--strict`
unconditional. Scheduled cron is deferred until an always-on box exists — the runner is a dev VM and
GitHub skips missed crons. A Windows job smoke-runs `run_loadtest.ps1` (8 clients) on every PR so the
PowerShell launcher can't bitrot.

**Baseline.** [`scale-gate-baseline.json`](../tools/bot_swarm/scale-gate-baseline.json) holds the
committed `downstream_kbs_per_client` mean (`kbs`) and egress wire KB/s (`wire_kbs`, [#772]) per
`<profile>/<pattern>` — since the transport is a profile property, each transport gets its own keys
(`reference/*` = GNS, `reference-enet/*` + `pr/weave` = enet6, [#773]). Only these protocol-stable
byte metrics are baselined — CPU-timing numbers are too noisy on shared runners. The gate fails on a
regression beyond `kbs_baseline_tolerance_pct` (10%). Regenerate after an intentional bandwidth
change (e.g. Epic B budgeting) with:

    python3 tools/bot_swarm/scale_gate.py --profile pr             --build-dir build/release --update-baseline
    python3 tools/bot_swarm/scale_gate.py --profile reference      --build-dir build/release --update-baseline
    python3 tools/bot_swarm/scale_gate.py --profile reference-enet --build-dir build/release --update-baseline

The payload KB/s baseline is machine-independent, so it can be regenerated from any box (a failed run
aborts the update rather than committing a partial baseline). That independence is measured, not
assumed: the values committed in #766 were produced on the 8-core reference VM and the hosted PR
runner independently measured the same `pr/weave` figure to within 0.1 KB/s (71.4 vs 71.379). Prefer
the reference VM anyway, so every key in the file comes from one box (the wire baseline additionally
*depends* on the loopback path being comparable, so treat it as reference-VM-only).

**When the gate fires, decide which kind of change it caught.** The tolerance band is a *regression
detector*, not a capacity limit — the real capacity gate is the wire ceiling (`assert_max_wire_kbs`,
150 KB/s/client, [#772]), and current runs sit around 76–81 KB/s wire on GNS (18–62 on enet6), i.e.
roughly 2× headroom on the shipping transport. So a baseline breach means "bytes moved", not "we are
out of budget". If the move is unintended, fix the code; if it is a reviewed, accepted
cost (as #725's shared-origin encode-once was), regenerate the baseline — otherwise the stale band
keeps firing on *later, unrelated* PRs and stops being a signal.
