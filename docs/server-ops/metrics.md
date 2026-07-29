# Metrics

The server can write a machine-readable health report on an interval. It is what to scrape into a
dashboard, what `bot_swarm --server-metrics` reads during a load test, and the first thing to look
at when players say the server feels bad.

```toml
[metrics]
tick_json_path        = "metrics.json"   # empty = disabled
tick_json_interval_ms = 1000             # [100, 60000]
```

`--metrics-json <path>` overrides the path from the command line. The file is written atomically
(temp file plus rename), so a reader never sees a half-written document and no locking is needed.

## Schema stability

`schema_version` is **frozen at 6** for the rest of primary development. It is not bumped for new
fields or new phases: the format is additive and name-keyed, both producer and consumer live in
this repository and ship together, and nothing gates on the number. It would only move if an
existing field's *meaning* changed under an unchanged name, or a field were removed.

So write consumers that look up fields by name and tolerate unknown ones. A consumer that asserts
an exact field set will break on the next release; one that reads `tick_hz` by name will not.

## The report

Timing fields ending in `_ms` are objects with `min`, `mean`, `max`, `p95`, `p99` and `stddev`,
computed over a rolling window of about 3600 ticks (60 seconds at the nominal rate).

### Health at a glance

| Field | Meaning | What healthy looks like |
|---|---|---|
| `tick_hz` | Measured simulation rate | ~60 |
| `tick_ms` | Total wall time per tick | `p99` comfortably under 16.6 |
| `load_factor` | Overrun governor's load factor | `1.0` |
| `interest_scale` | Governor's interest-radius scale | `1.0` |
| `dropped_ticks` | Ticks shed by the governor plus loop catch-up | `0`, and not growing |
| `window_s` / `ticks_sampled` / `ticks_total` | The measurement window itself | — |

**Read `load_factor` before anything else.** Below `1.0` the server has decided it cannot keep up
and is deliberately shedding work — decimating snapshots, striding the AI pass, shrinking interest
radii. That is the server degrading gracefully rather than collapsing, but it means players are
already getting less than the configuration promises.

`interest_scale` below `1.0` means the governor has shrunk how far each player can see. Entities
outside the reduced radius are interest-filtered, never despawned, so this shows up as pop-in
rather than as things vanishing.

### Per-phase tick budget

| Field | Phase |
|---|---|
| `maintenance_ms` | Start-of-tick bookkeeping, governor step, periodic pruning |
| `sensing_ms` | Sensor and contact-table pass |
| `ai_ms` | Controller `sample()` calls |
| `integrate_ms` | Flight integration |
| `collision_ms` | Collision detection |
| `weapons_ms` | Fire control and projectiles |
| `serialize_ms` | Per-peer snapshot assembly and encoding |
| `other_ms` | Total minus the sum of the named phases |

This is the breakdown that tells you *what* is slow, not just *that* something is. `serialize_ms`
dominating points at player count and snapshot budget; `ai_ms` dominating points at entity count
and AI behaviour mix; a large `other_ms` means the cost is somewhere not yet instrumented and is
worth reporting.

### Network

| Field | Meaning |
|---|---|
| `wire_out_kbs` / `wire_in_kbs` | Aggregate bandwidth, kilobytes per second |
| `wire_out_kbs_per_client` | Outbound per connected client — the number the scale gate regresses against |
| `wire_out_pps` | Outbound packets per second |
| `wire_peers` | Peers counted in the wire figures |

### Congestion

| Field | Meaning |
|---|---|
| `congestion_max_loss` | Worst per-peer packet loss seen in the window |
| `congestion_min_send_hz` | Lowest snapshot rate any peer was decimated to |
| `congestion_recovered_send_hz` | Rate peers recovered to afterwards |

### Process

| Field | Meaning |
|---|---|
| `peers` | Connected peers |
| `entities` | Live world entities |
| `entity_soft_cap` | The `[world] entity_soft_cap` ceiling in force; `0` = unlimited |
| `entity_cap_refusals` | Spawns the cap has refused since startup (monotonic) |
| `rss_kb` / `rss_startup_kb` | Resident memory now, and at startup |

`rss_kb` against `rss_startup_kb` is the leak signal over a soak run. A working set that grows and
plateaus is normal; one that grows without plateauing is not.

`entity_cap_refusals` rising means the world is at its ceiling and spawns are being dropped —
projectiles that never appear, AI that never launches, players held out. Read it alongside
`entities`/`entity_soft_cap` before believing any other count on a capped server: a low entity
count next to a rising refusal counter is a truncated world, not a quiet one.

### Per-peer throttle attribution

When the overrun governor is the binding constraint on a peer, a `peer_throttle` array appears
naming which lever is limiting each one. **It is omitted entirely when no peer is being throttled**,
so a healthy report simply does not have the key — absence is the healthy case, not missing data.

This distinction is the whole point of the field. A peer receiving snapshots late because *its own
link* is congested and a peer receiving them late because *the server* is shedding work look
identical from the client, and they call for opposite responses: one is the player's problem, the
other is yours.

## Reading it

```bash
# Is the server keeping up?
jq '{tick_hz, load: .load_factor, p99: .tick_ms.p99, dropped: .dropped_ticks}' metrics.json

# Where is the time going?
jq 'to_entries | map(select(.key | endswith("_ms")))
    | map({phase: .key, mean: .value.mean}) | sort_by(-.mean)' metrics.json
```

A rough triage order:

1. `load_factor` < 1 or `dropped_ticks` climbing → the server is over budget. Go to step 2.
2. Largest `*_ms` mean names the phase. Reduce what feeds it: entity count for `ai_ms`, player
   count or `snapshot_budget_bytes` for `serialize_ms`.
3. `tick_hz` at 60 with high `congestion_max_loss` → the server is fine and the network is not.
   Check `/peers` in the [admin API](admin-api.md) to see whether it is one peer or all of them.
4. `rss_kb` growing without plateau across a long run → report it with the metrics file attached.

## See also

- [Server configuration](server-config.md) — `[metrics]`, and the `overrun_*` governor keys
- [Admin API](admin-api.md) — `/status` and `/peers` for a live look
- [Load testing](../developer/load-testing.md) — driving the server hard enough to make these move

!!! note "Capacity planning"
    Turning these numbers into "how many players can this box hold" is tracked separately in
    [#551](https://github.com/fighters-legacy/fighters-legacy/issues/551) and belongs with the
    at-scale work. This page is the field reference.
