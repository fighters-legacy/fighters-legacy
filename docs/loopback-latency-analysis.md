# ENet Loopback Latency Analysis

This document explains the latency characteristics of the single-player ENet loopback
path, documents the decision made for haptic and input timing, and describes how to
re-run the analysis when conditions change.

Related issue: [#179](https://github.com/fighters-legacy/fighters-legacy/issues/179)

---

## Why this analysis exists

Single-player uses an embedded `fl-server` on `127.0.0.1:4778` connected to the game
client via standard ENet. This is intentional — it keeps single-player and multiplayer
on identical code paths with no special-casing. The cost is that every world-state
packet travels a full loopback network round-trip, even in solo play.

Before Phase 3 haptic work (#128), this analysis was needed to confirm whether that
loopback round-trip materially affects haptic timing or input response.

---

## Latency model

Two independent latency components exist on the loopback path:

| Component | Description | Typical magnitude |
|-----------|-------------|-------------------|
| **ENet socket overhead** | Raw UDP loopback from `send()` to `onReceive()`, including ENet protocol framing | < 1 ms on all platforms |
| **Sim-tick boundary delay** | Input received between ticks waits up to one full tick before the server processes it and sends a `MsgWorldSnapshot` | 0–16.7 ms at 60 Hz (1 tick average) |

The sim-tick boundary dominates. ENet loopback is effectively free.

---

## Decision record

**Decision date:** 2026-06-24
**Approach: Accept + Compensate**

### Measured data

Three runs on each platform using `net_check --bench 600 --bench-rate 60`.

| Platform      | OS loopback RTT        | ENet RTT mean | ENet RTT p99 | Sim-tick delay |
|---------------|------------------------|--------------|--------------|----------------|
| Fedora Linux  | 0.007 ms (UDP/sockperf) | 12 ms       | 17 ms        | 1t (16.7 ms)   |
| macOS         | 1.1 ms (ICMP)          | 15 ms        | 21 ms        | 1t (16.7 ms)   |
| Windows 10/11 | < 1 ms (ICMP)          | 15 ms        | 20 ms        | 1t (16.7 ms)   |

**What the bench measured — and what it did not:**

`net_check --bench` reads `ENetPeer::roundTripTime`, which tracks how long ENet's
internal PING/ACK exchange takes. The server's `enet_host_service()` is called at the
sim rate (60 Hz), so every PING waits up to 16.7 ms for the server's next service
call. The measured RTT (8–21 ms) is therefore the **sim-tick boundary delay**, not
raw ENet socket overhead.

The raw socket overhead is confirmed by the OS-level baselines: Linux UDP loopback is
~7 µs (sockperf), macOS ICMP ~1 ms, Windows ICMP < 1 ms. All are completely invisible
against the ~16 ms tick-boundary cost.

The 500 ms values that appear as `RTT max` in the raw `compare.py` output are
`ENET_PEER_DEFAULT_ROUND_TRIP_TIME` — ENet's startup placeholder before the first real
PING/ACK completes. They appear only in the first 1–2 samples of each run and are
already excluded by the p99 column. They are not a real latency observation.

**Conclusion:** ENet loopback adds no perceptible latency beyond the sim-tick
boundary. The tick boundary itself (0–16.7 ms, p99 ~17–21 ms across platforms) is
within the accepted human-perception threshold for a flight sim. The platform spread
(Linux p99 17 ms vs macOS/Windows p99 20–21 ms) reflects OS timer granularity: Linux
HZ=1000 gives 1 ms sleep precision; macOS and Windows default to 10–15 ms.

### Accept — one-tick lag for reactive events

Reactive haptic events (hit taken, stall warning, terrain proximity alert, transonic
buffet) fire from server-delivered `WorldSnapshot` state, arriving one tick (≤ 16.7 ms)
after the event occurs in the sim. This is within the accepted human-perception
threshold for a flight sim. No special handling is required.

### Compensate — predictive haptics for command-driven events

For events the client already knows about at the moment of input, fire the haptic
immediately without waiting for server confirmation:

| Event | Current path | After #128 |
|-------|-------------|------------|
| Gun burst | `weaponFired` flag in `HapticController::update()` — already client-side ✓ | No change needed |
| Afterburner engage | Fired on `EntityRenderEntry::abEngaged` (one tick late) | Fire on `Afterburner` input binding press in `FlightInputCollector` |
| Gear command | Not yet wired | Fire on gear key press in `FlightInputCollector` |

### Compensate — client-side prediction for flight inputs (#381)

The "Compensate" leg for flight inputs is implemented by `ClientPrediction`
(`game/fighters-legacy/`): a local `FlightIntegrator` steps each sent `MsgClientInput`
immediately, and reconciles against each received `MsgWorldSnapshot` using
`estimatedDelayTicks` (carried losslessly via `ExtTag::SnapshotPeerDelayTicks`) as the
replay-window depth. This eliminates the full server round-trip delay from the player's
perspective for all continuous flight controls.

### Fast-path rejected

Introducing an `ISimDirectBridge` to bypass ENet in single-player was evaluated and
rejected:

- ENet socket overhead is sub-millisecond — the complexity is not warranted
- Bypassing ENet would break the single-player = multiplayer code-path parity that is
  an explicit architectural principle
- It would require `WorldBroadcaster` to expose a parallel direct-call API surface

---

## Baseline results

Measured 2026-06-24. Three runs per platform; values below represent the median run.
Full run-by-run data: `tools/latency_analysis/results/`.

| Platform      | OS loopback RTT         | ENet RTT (mean) | ENet RTT (p99) | Sim-tick delay |
|---------------|-------------------------|-----------------|----------------|----------------|
| Fedora Linux  | 0.007 ms (UDP, sockperf) | 12 ms          | 17 ms          | 1t (16.7 ms)   |
| macOS         | 1.1 ms (ICMP)           | 15 ms           | 21 ms          | 1t (16.7 ms)   |
| Windows 10/11 | < 1 ms (ICMP)           | 15 ms           | 20 ms          | 1t (16.7 ms)   |

Note: "ENet RTT" here is the sim-tick-paced PING/ACK round-trip, not raw socket
latency. See the Decision record section for the full interpretation.

---

## How to re-run the analysis

Re-run when any of the following change:
- ENet version bump (see `cmake/dependencies.cmake`)
- Sim tick rate changes from 60 Hz
- Major platform OS upgrade (new kernel, new IOCP behaviour, etc.)

**Step 1 — Build**

```bash
cmake --build --preset debug        # Linux / macOS
cmake --build --preset debug-msvc   # Windows
```

**Step 2 — Run measurement scripts**

```bash
# Fedora Linux (primary platform)
bash tools/latency_analysis/measure_linux.sh

# macOS
bash tools/latency_analysis/measure_macos.sh

# Windows (PowerShell)
.\tools\latency_analysis\measure_windows.ps1
```

Requires platform-specific baseline tools. Install once:

```
Fedora/RHEL:   sudo dnf install sockperf
Ubuntu/Debian: sudo apt install sockperf
macOS:         brew install iperf3
Windows:       (no extra tools required)
```

**Step 3 — Tabulate results**

```bash
python3 tools/latency_analysis/compare.py
```

Prints a Markdown table. Copy it into the "Baseline results" section above.

**Step 4 — Update this document** with the new table and today's date in the decision
record if the results change materially.

---

## Interpreting results

| ENet RTT p99 | Interpretation |
|---|---|
| 15–22 ms | **Normal** — tick-boundary wait dominates; ENet socket overhead is invisible |
| 22–33 ms | Investigate OS scheduling; may indicate sim thread running at reduced priority or unexpected context switching |
| > 33 ms | Anomalous — check `SCHED_OTHER` CFS jitter on Linux, Windows timer resolution, or macOS efficiency-core scheduling |

The round-dt metric (wall-clock duration of each `service()` call) indicates OS timer
granularity. On Linux with HZ=1000, expect ~1 ms jitter. On macOS (100 Hz clock),
expect ~10 ms jitter. On Windows with `timeBeginPeriod(1)`, expect 1–2 ms jitter.

---

## Multi-client load testing

`net_check --bench` measures single-peer loopback RTT. Its multi-client companion is
**`bot_swarm`** (`tools/bot_swarm/`), which drives N synthetic clients against `fl-server` to
characterise behaviour at scale (observed server tick-Hz, per-client bandwidth) for the 128+
player target — see [docs/load-testing.md](load-testing.md). Both tools share the percentile
math in `tools/common/NetStats.h`.

## Related issues

- [#128](https://github.com/fighters-legacy/fighters-legacy/issues/128) — Lua haptic API (Phase 3); Compensate path wired here
- [#179](https://github.com/fighters-legacy/fighters-legacy/issues/179) — This analysis (source of truth for the decision record)
- [#381](https://github.com/fighters-legacy/fighters-legacy/issues/381) — Client-side prediction; reduces perceived input lag independently of haptic timing
