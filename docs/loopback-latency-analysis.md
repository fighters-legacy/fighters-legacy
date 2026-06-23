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

**Decision date:** *(fill in after running the analysis)*
**Approach: Accept + Compensate**

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

### Fast-path rejected

Introducing an `ISimDirectBridge` to bypass ENet in single-player was evaluated and
rejected:

- ENet socket overhead is sub-millisecond — the complexity is not warranted
- Bypassing ENet would break the single-player = multiplayer code-path parity that is
  an explicit architectural principle
- It would require `WorldBroadcaster` to expose a parallel direct-call API surface

---

## Baseline results

*(Fill in after running `compare.py` on results from all three platforms.)*

| Platform         | OS UDP RTT | ENet RTT (mean) | ENet RTT (p99) | Sim-tick delay |
|------------------|-----------|-----------------|----------------|----------------|
| Fedora Linux     | —         | —               | —              | 1t (16.7 ms)   |
| macOS            | —         | —               | —              | 1t (16.7 ms)   |
| Windows 10/11    | —         | —               | —              | 1t (16.7 ms)   |

The sim-tick delay is confirmed at 1 tick on all platforms via `estimatedDelayTicks`
(measured informally in #348 before this formal analysis ran).

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
| < 2 ms | Normal; ENet overhead is imperceptible relative to the tick boundary |
| 2–5 ms | Investigate OS scheduling (see kernel-level inspection steps in the scripts) |
| > 5 ms | Likely scheduling anomaly; check `SCHED_OTHER` CFS jitter or Windows timer resolution |

The round-dt metric (wall-clock duration of each `service()` call) indicates OS timer
granularity. On Linux with HZ=1000, expect ~1 ms jitter. On macOS (100 Hz clock),
expect ~10 ms jitter. On Windows with `timeBeginPeriod(1)`, expect 1–2 ms jitter.

---

## Related issues

- [#128](https://github.com/fighters-legacy/fighters-legacy/issues/128) — Lua haptic API (Phase 3); Compensate path wired here
- [#179](https://github.com/fighters-legacy/fighters-legacy/issues/179) — This analysis (source of truth for the decision record)
- [#381](https://github.com/fighters-legacy/fighters-legacy/issues/381) — Client-side prediction; reduces perceived input lag independently of haptic timing
