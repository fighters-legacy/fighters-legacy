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
comparison). `bot_swarm` bumps `schema_version` to **2** when this block can be present.

The same JSON shape is the standalone `--metrics-json` file and the embedded block:

| Field | Meaning |
|---|---|
| `schema_version` | server-tick report schema (currently `1`) |
| `tick_hz` | actual recent tick rate over the sampling window (ring-derived) |
| `ticks_sampled` / `ticks_total` | ticks in the rolling window / monotonic all-time |
| `window_s` | wall-clock span of the sampling window |
| `peers` / `entities` | live peer count / live entity count at write time |
| `tick_ms` | total `onTick` wall-time stats `{min,mean,max,p95,p99}` (ms) |
| `maintenance_ms` | rate-limit prune, idle timeout, admin drains, spatial rebuild, input drain, jitter resize |
| `integrate_ms` | physics integration (`stepFlightSim`) summed across entities |
| `ai_ms` | controller `sample()` summed across entities |
| `collision_ms` | `EntityManager::onTick` (damage/collision/reap) |
| `serialize_ms` | telemetry + snapshot assembly/send + weather + shutdown notices |
| `other_ms` | `tick_ms − Σ(phases)` (loop/function overhead), clamped ≥ 0 |

The scale gate ([CI scale gate](#ci-scale-gate)) asserts on `server_tick.tick_ms.p99` via
`--assert-max-tick-ms` (strict tier only).

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

## CLI

    bot_swarm [host] [port] [options]
      --clients N            synthetic clients (default 32)
      --duration S           soak seconds (default 30)
      --rate HZ              MsgClientInput rate per client (default 60)
      --ramp-ms MS           delay between successive connects (default 20)
      --threads N            worker threads (default auto = min(cores, ceil(clients/32)))
      --pattern NAME         weave | level | aggressive | idle | random (default weave)
      --json PATH            write a JSON report
      --server-metrics PATH  read fl-server --metrics-json file; embed authoritative server_tick block
      --assert-min-tick-hz X exit nonzero if observed (proxy) tick-Hz min < X
      --assert-max-kbs Y     exit nonzero if downstream KB/s/client max > Y
      --assert-max-tick-ms X exit nonzero if authoritative server tick p99 (ms) > X
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

Adding a pattern (e.g. a `trace:<file>` replay of recorded input, or a weighted mix) is a new
`IFlightPattern` subclass + a branch in `makePattern()` — no harness changes.

## Characterisation runbook (for #505)

To find the `enet6` ceiling, sweep the client count under a fixed pattern and watch the
**observed server tick-Hz min** fall away from 60:

    for n in 32 64 96 128 160 200 256; do
      tools/bot_swarm/run_loadtest.sh build/debug "$n" 30 weave
    done

The knee — where tick-Hz sags and the max snapshot gap spikes — is the ceiling under that load
profile. Run it for `idle` (overhead floor) and `aggressive` (worst case) too. Record the
reference machine spec alongside the numbers.

## Validating congestion response (#518)

Loopback has zero loss, so the per-client congestion controller never triggers in a normal run — a
clean run is exactly the no-regression baseline (`downstream_kbs_per_client` and held 60.0 Hz must be
unchanged from before #518). To exercise the back-off you have to degrade the link. On Linux, inject
loss/latency on the loopback device with `netem`:

    sudo tc qdisc add dev lo root netem loss 5% delay 80ms
    tools/bot_swarm/run_loadtest.sh build/debug 64 30 weave
    sudo tc qdisc del dev lo root netem    # always restore

Under the degraded link, congested clients show a **lower observed snapshot Hz** (toward the
`congestion_min_send_hz` floor) and **lower `downstream_kbs_per_client`**, while the authoritative
`server_tick` p99 stays healthy. (macOS: `dnctl`/`pfctl`; Windows: `clumsy`.) This is a manual,
NET_ADMIN-only step and is **not** run in CI; the deterministic proof is the
`test_world_broadcaster` `[congestion]` link-stats-injection tests. The deterministic AIMD logic itself
is covered by `test_congestion_controller`. See [docs/congestion-control-design.md](congestion-control-design.md).

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

**Two tiers, because hosted runners are not the reference box:**

| Tier | Trigger | Profile | Hard gates | Advisory |
|---|---|---|---|---|
| **PR** | every PR + push to `main` (Linux, Release) | `pr` (64 clients, weave) | bandwidth ≤150 KB/s/client, admission (no refused/dropped), KB/s baseline regression, tick-Hz collapse tripwire (≥30) | tick-ms p99 (disabled) |
| **Reference** | nightly cron + `workflow_dispatch` | `reference` (128 clients; idle/weave/aggressive) | bandwidth + admission + baseline | tick-ms p99 ≤16.6 (enforced only with `--strict`) |
| **Soak** | `workflow_dispatch` (`profile=soak`) | `soak` (128 clients, weave, 2 h) | + RSS-growth leak signal from the runner sampler | tick-ms p99 (advisory) |

The PR tier hard-gates only machine-independent metrics: `bot_swarm`'s `--assert-min-tick-hz` reads
the *client-side proxy*, which sags when the harness itself is CPU-starved on a shared runner — a
false failure. So tick-Hz is only a total-collapse tripwire and tick-ms is advisory on PRs. The
strict `16.6 ms` p99 is meaningful only on the 8‑core/16 GB
[reference-env](../tools/bot_swarm/reference-env/README.md) (or a self-hosted runner), where the
scheduled job is run with `--strict`. A Windows job smoke-runs `run_loadtest.ps1` (8 clients) on
every PR so the PowerShell launcher can't bitrot.

**Baseline.** [`scale-gate-baseline.json`](../tools/bot_swarm/scale-gate-baseline.json) holds the
committed `downstream_kbs_per_client` mean per `<profile>/<pattern>`. Only this protocol-stable
metric is baselined — CPU-timing numbers are too noisy on shared runners. The gate fails on a
regression beyond `kbs_baseline_tolerance_pct` (10%). Regenerate after an intentional bandwidth
change (e.g. Epic B budgeting) with:

    python3 tools/bot_swarm/scale_gate.py --profile pr        --build-dir build/release --update-baseline
    python3 tools/bot_swarm/scale_gate.py --profile reference --build-dir build/release --update-baseline

The KB/s baseline is machine-independent, so it can be regenerated from any box (a failed run aborts
the update rather than committing a partial baseline).
