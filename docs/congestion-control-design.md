# Adaptive Per-Client Send-Rate & Congestion Response

Design record for the per-client congestion-response work (Epic B, [#495]). Implements [#518], the
last engine sub-task of the bandwidth/snapshot-scaling epic. Builds directly on the steady-state
scaling already landed: quantized bitstream ([#515]), priority/budget scheduler ([#516]), client-acked
delta baselines ([#517], with selective-ack identity precision [#566]), and 3D interest culling ([#402]).

[#495]: https://github.com/fighters-legacy/fighters-legacy/issues/495
[#515]: https://github.com/fighters-legacy/fighters-legacy/issues/515
[#516]: https://github.com/fighters-legacy/fighters-legacy/issues/516
[#517]: https://github.com/fighters-legacy/fighters-legacy/issues/517
[#518]: https://github.com/fighters-legacy/fighters-legacy/issues/518
[#566]: https://github.com/fighters-legacy/fighters-legacy/issues/566
[#402]: https://github.com/fighters-legacy/fighters-legacy/issues/402

## Problem

`WorldBroadcaster::onTick` sent **every connected peer a `MsgWorldSnapshot` every tick (60 Hz)** with
a **fixed** per-client byte budget. There was no feedback loop: a peer on a congested or lossy link
kept being blasted at full rate, deepening its queue, inflating latency, and wasting shared
bandwidth. The 8-core/Release reference-env data ([#505]) showed ~480 KB/s at 128 idle clients;
steady-state quantization + budgeting (#515/#516) cut the *average*, but nothing adapted **per
client** when a specific link degraded.

[#505]: https://github.com/fighters-legacy/fighters-legacy/issues/505

## Design

Each connected peer owns one `fl::CongestionController` (`engine/net/CongestionController.{h,cpp}`) —
a pure AIMD policy with no glm / engine-entity deps, unit-tested in isolation like `SnapshotScheduler`,
`SnapshotCodec`, and `JitterBuffer`. `WorldBroadcaster` steps every peer's controller once per tick
from that peer's link quality, then uses the resulting throttle to gate snapshot sends and budgets.

### Signal: ENet link stats (`INetwork::getPeerLinkStats`)

The controller consumes a `CongestionSample`:

| field | source (`ENetPeer`) | role |
|---|---|---|
| `packetLoss` (0..1) | `packetLoss / ENET_PEER_PACKET_LOSS_SCALE` | **primary** trigger (loss > threshold) |
| `rttMs` | `roundTripTime` | delay-gradient trigger (rise over running baseline) |
| `reliableBytesInFlight` | `reliableDataInTransit` | backlog trigger |

`getPeerLinkStats` was added to the `INetwork` HAL (returns all-zero for mocks / unconnected peers, so
the controller stays a no-op in unit tests and single-player loopback). The pre-existing `getPeerRtt`
is retained for its client-side callers (net_check, bot_swarm); `getPeerLinkStats` supersedes it
server-side.

> **Anti-feedback decision (the central design point).** The delay term uses **ENet RTT**, *not* the
> application's snapshot one-way delay (`PeerInputState::estimatedDelayTicks`). The snapshot delay
> *inflates as a direct result of our own decimation* — once we slow a peer's send rate, its echoed
> "last received tick" gets staler, which reads as higher delay, which would drive still more
> decimation: a self-reinforcing collapse to the floor. ENet measures RTT on its own periodic reliable
> pings, so it stays fresh regardless of our snapshot cadence. The controller therefore never reads a
> metric its own action pollutes.

### Controller: AIMD with one throttle, two levers

The controller maintains a single `throttle ∈ [throttleFloor, 1]`. Every `evalIntervalTicks` (default
6) it classifies the peer:

```
baselineRttMs = running-min(rttMs)               // slow upward drift tracks genuine path changes
congested = packetLoss > lossThreshold
         || (rttMs - baselineRttMs) > delayMarginMs    // guarded so the unsigned diff never underflows
         || reliableBytesInFlight > backlogThresholdBytes
throttle = congested ? max(throttleFloor, throttle * decreaseFactor)   // multiplicative decrease
                     : min(1,            throttle + increaseStep)        // additive increase
```

Between eval boundaries the throttle is held — built-in hysteresis. From the throttle:

- **Send-rate lever:** `sendIntervalTicks = clamp(ceil(1/throttle), 1, maxIntervalTicks)`. The
  broadcaster skips a peer's per-tick snapshot until `currentTick - lastSnapshotSentTick >=
  sendIntervalTicks`. The float result is clamped *before* the cast to `uint32_t`, and `throttleFloor`
  is kept `> 0`, so `1/throttle` can never produce `inf`/`NaN` (UBSan-safe).
- **Budget lever:** `effectiveBudget(static) = max(min(static, budgetFloorBytes), round(static *
  throttle))`. At `throttle == 1` this returns the static budget unchanged; the floor is itself capped
  at the static budget so a budget already below the floor is never *raised*. `static == 0` (unlimited)
  passes through unchanged — with an unlimited budget only the rate lever applies.

Aggregate per-second downstream scales ≈ `throttle²` (rate × packet size). `decreaseFactor = 0.7`
keeps a single back-off near ½, not ¼. A healthy peer, a disabled controller, or the zero-link-stats
case all hold `throttle == 1`, i.e. **byte-for-byte the pre-#518 behaviour** — which is why the
existing per-tick-send tests are unaffected.

### No wire-format change

Decimation needs no protocol change: the client already tolerates a variable snapshot rate (render
interpolation alpha is wall-clock; prediction replay depth comes from `estimatedDelayTicks`). The
#517 client-acked delta machinery is reused unchanged — a smaller budget simply defers more
low-relevance entities, and skipped ticks defer all per-peer state (despawn queueing, `knownGens` GC,
`lastSentTick`) to the next actual send.

## Configuration

`[world]` keys in `server.toml` (all hot-reloadable via `reload_config`):

| key | default | range | meaning |
|---|---|---|---|
| `congestion_enabled` | `true` | bool | master switch; `false` pins every peer to full 60 Hz / full budget |
| `congestion_min_send_hz` | `10.0` | [1, 60] | floor send rate under congestion → `throttleFloor`, `maxIntervalTicks` |
| `congestion_loss_threshold` | `0.02` | [0, 1] | ENet mean loss fraction that marks a peer congested |
| `congestion_budget_floor_bytes` | `400` | [0, 65535] | never scale a set byte budget below this |

The remaining AIMD constants (`delayMarginMs`, `backlogThresholdBytes`, `increaseStep`,
`decreaseFactor`, `evalIntervalTicks`) keep sensible defaults and are not exposed as config knobs.
`fl::makeCongestionParams(...)` maps the Hz-based operator knob to the controller's internal
`throttleFloor` / `maxIntervalTicks`; it is shared by startup config and the `reload_config` path.

Per-peer send rate and packet loss are surfaced in `PeerInfo` (`forEachPeer`) and shown by the `peers`
admin command (`rate=NN Hz  loss=N.N%`).

## Validation

The mechanism is proven deterministically by unit/integration tests (`test_congestion_controller`,
`test_world_broadcaster` `[congestion]`) that inject link stats via `TrackingNetwork::peerLinkStats`.

Live-link demonstration is **Linux-only and manual** (loopback has zero loss, so congestion never
triggers in a normal bot_swarm run):

```bash
sudo tc qdisc add dev lo root netem loss 5% delay 80ms
tools/bot_swarm/run_loadtest.sh build/debug 64 30 weave   # observe lower observed Hz + KB/s for clients
sudo tc qdisc del dev lo root netem                       # restore
```

(macOS: `dnctl`/`pfctl`; Windows: `clumsy`.) Honest framing — like #517, this lever's value is
worst-case congested-link behaviour, not the loopback average, which #515/#516 own. bot_swarm needs no
change: it consumes snapshots by `tickIndex` + byte count and already tolerates a variable rate.

## Interactions & follow-ons

- **ENet host bandwidth caps** (`incoming/outgoing_bandwidth_bps`): when an operator caps host
  bandwidth, ENet's own throttle raises `packetLoss`, which this controller reads and responds to —
  complementary, not conflicting.
- **Transport replacement (Epic L):** `getPeerLinkStats` reads ENet-specific peer fields. Any
  replacement `INetwork` backend **must** surface loss/RTT/in-flight, or the controller silently
  degrades to "always healthy" (full rate) — acceptable as a fallback but worth re-wiring.
- **`congestion_budget_floor_bytes` and an unlimited static budget:** with `snapshot_budget_bytes = 0`
  the budget lever is inactive and only the rate lever sheds bandwidth. Acceptable — rate alone roughly
  halves bandwidth per back-off step.
- Follow-ons: synthetic congestion in the load harness (so the response is CI-validated, not netem-
  manual), per-peer congestion telemetry in `--metrics-json`, a client-side "degraded network"
  indicator, and a finer app-level snapshot-loss estimator.
