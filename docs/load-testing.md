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
| **observed server tick-Hz** | `(lastTick − firstTick) / elapsed` from snapshot `tickIndex` progression. The headline **server-health proxy**: when the server falls behind, this sags below 60. Report the **min** across clients as the soak signal. |
| **downstream KB/s per client** | Snapshot payload bytes per client per second — the per-client bandwidth the gate caps. |
| **RTT (ms)** | ENet round-trip estimate / `MsgPeerDelay` per client. |
| **connect time (ms)** | From connect issue to `onConnect` (includes the ramp queue). |
| **worker loop dt (ms)** | Harness work-time per tick. If it approaches the tick interval (16.7 ms @ 60 Hz) the **harness** is the bottleneck and the numbers are suspect. |
| clients connected / refused / disconnected | Admission + stability over the run. |

The server exposes no authoritative per-tick metric yet; when Epic A (#513) adds one, the JSON
report gains a server-side block (extend, don't replace) and the gate switches to it.

## Scale-gate targets

128 clients @ 60 Hz with sim tick **≤ 16.6 ms p99** (observed tick-Hz ≈ 60) on a reference
**8-core / 16 GB** instance, sustained **≤ ~150 KB/s/client** downstream after Epic B
quantization + budgeting, soak-stable for 2 h. (#520 owns the pass/fail thresholds; `bot_swarm`
provides the measurement plus the optional `--assert-*` hooks.)

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

## CLI

    bot_swarm [host] [port] [options]
      --clients N            synthetic clients (default 32)
      --duration S           soak seconds (default 30)
      --rate HZ              MsgClientInput rate per client (default 60)
      --ramp-ms MS           delay between successive connects (default 20)
      --threads N            worker threads (default auto = min(cores, ceil(clients/32)))
      --pattern NAME         weave | level | aggressive | idle | random (default weave)
      --json PATH            write a JSON report
      --assert-min-tick-hz X exit nonzero if observed tick-Hz min < X
      --assert-max-kbs Y     exit nonzero if downstream KB/s/client max > Y
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

## Platform notes

- **macOS / Linux:** each client is a UDP socket; `bot_swarm` raises `RLIMIT_NOFILE` and the
  runner bumps `ulimit -n`. For high counts raise UDP buffers (`net.core.rmem_default` on Linux,
  `net.inet.udp.recvspace` on macOS) or RTT/bandwidth numbers skew.
- **Windows:** the run raises the timer resolution (`timeBeginPeriod(1)`) so 60 Hz pacing is
  accurate.

## CI

The Linux/macOS "Smoke test tools" CI step runs `run_loadtest.sh build/debug 8 3 weave` and
fails if any of the 8 clients are refused or dropped. The pure-logic unit tests
(`test_bot_swarm`) run on all platforms including Windows.
