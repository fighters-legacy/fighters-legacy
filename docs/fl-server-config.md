# fl-server Operator Configuration Reference

`fl-server` reads its settings from a TOML configuration file (`server.toml` by default).
If the file is absent when the server starts, it is created automatically with commented
defaults — a safe starting point for new deployments.

---

## Configuration precedence

Settings are resolved in three tiers. Later tiers override earlier ones.

| Tier | Source | Example |
|---|---|---|
| 1 (lowest) | `server.toml` (path from `FL_CONFIG`, default `./server.toml`) | `[server] port = 9000` |
| 2 | CLI positional args and named flags | `fl-server 9000 32 --bind 127.0.0.1` |
| 3 (highest) | Environment variables | `FL_PORT=9000` |

All TOML sections are tier-1 only (env vars and CLI do not cover arrays or
multi-key sections). See [Environment variables](#environment-variables) for the full
`FL_*` list.

### CLI flags

| Flag | Argument | Description |
|---|---|---|
| `--help`, `-h` | — | Print usage and exit |
| `--version`, `-v` | — | Print version and exit |
| `--persistent` | — | Enable persistent world mode (Phase 2 — not yet active) |
| `--bind <addr>` | IP or hostname | Override `server.bind_address` from the command line; takes precedence over `server.toml` and `FL_BIND_ADDRESS`. Used by the game client when spawning fl-server for single-player mode (`--bind 127.0.0.1`). |
| `--metrics-json <path>` | file path | Write the per-phase tick-budget JSON to `<path>`; overrides `[metrics] tick_json_path`. See [metrics](#metrics--tick-budget-export). |
| `--mission <name>` | mission asset name | Load a mission at startup; overrides `[rotation]`. See [rotation](#rotation--scenario-rotation). |

CLI positional arguments (Tier 2): `fl-server [port] [maxPeers]`

---

## Full annotated example

Copy this file as a starting point and uncomment or modify what you need.

```toml
[server]
name         = "Unnamed Server"
port         = 4778
bind_address = "0.0.0.0"
max_peers    = 32
game_modes   = ["campaign", "mission", "sandbox"]
motd           = ""
motd_display_s = 0
password     = ""

[rotation]
order          = "sequential"
items          = []
time_limit_min = 0

[lobby]
register   = false
url        = "https://lobby.fighters-legacy.org"
visibility = "public"

[mods]
stack = []
# required = ["fl-base", "theater@1.2"]  # packs a client must have ("id" or "id@version") (#872)
# required_policy = "warn"            # warn | refuse | allow_placeholder -- action on a missing pack (#872)

[world]
player_faction = 1  # faction stamped on every player; MUST be non-zero for combat (see below)
# player_entity_type = "builtin:debug-entity"  # aircraft a connecting pilot flies when the client requests none (#834)
# allow_observers    = true          # false = refuse observer-role (spectator) connections (#857)
save_path          = "world.sav"
autosave_interval_s = 300
time_scale         = 10.0        # game seconds per real second; 10 = full day/night ≈ 2.4 real hours
# planet_radius_m         = 6371000  # planet sphere radius (m); Earth default
# draw_distance_km        = 200.0    # per-peer interest management radius (km); [1, 100000]
# sensor_check_hz         = 10.0     # sensor geometry checks/sec; the reference cadence pods are tuned to; [1, 60]
# spatial_cell_size_km    = 10.0     # SpatialIndex cell size (km); 0 = auto from draw distance; [0, 1000]; restart
# snapshot_budget_bytes   = 1200     # per-client snapshot byte budget; 0 = unlimited; [0, 65535]
# jitter_buffer_depth           = 4    # per-peer input queue depth (ticks); global cap for adaptive sizing; [1, 32]
# jitter_buffer_adapt_window    = 60   # EWMA smoothing window in ticks; alpha = 1/window; [10, 3600]
# jitter_buffer_hysteresis      = 2    # resize dead-band in ticks; [0, 8]
# jitter_buffer_jitter_multiplier = 2.0  # k factor: depth = ceil(ewma_delay + k*jitter); [0.0, 8.0]
# congestion_enabled            = true   # adaptive per-client send-rate / congestion response (#518)
# congestion_min_send_hz        = 10.0   # floor snapshot rate under congestion; [1, 60]
# congestion_loss_threshold     = 0.02   # ENet mean loss fraction marking a peer congested; [0, 1]
# congestion_budget_floor_bytes = 400    # never scale a set snapshot budget below this; [0, 65535]
# overrun_governor_enabled    = true   # graceful tick-overrun governor: shed work over budget (#514)
# overrun_high_watermark      = 0.90   # EWMA tick-ms / budget that triggers shedding; [0.1, 1.0]
# overrun_low_watermark       = 0.60   # recovery threshold (dead-band below high); [0.0, high)
# overrun_min_snapshot_hz     = 15.0   # floor broadcast rate under overrun; [1, 60]
# overrun_max_ai_stride       = 4      # deepest AI-sample decimation for non-player entities; [1, 32]
# overrun_budget_floor_bytes  = 400    # never scale the snapshot budget below this under overrun; [0, 65535]
# overrun_min_interest_fraction = 0.5  # interest-radius floor fraction under overrun; [0.1, 1.0]; 1.0 = lever off
# max_catchup_ticks       = 8        # GameLoop catch-up cap (spiral backstop); [1, 64]; needs restart
# sim_worker_threads      = 0        # sim-tick CPU parallelism; 0 = auto, 1 = serial; [0, 256]
# --- load-test affordance (#573); A TESTING AFFORDANCE, NOT A CAPACITY GUARANTEE; leave at 0 normally ---
# test_spawn_ai_count     = 0        # pre-spawn N server-side AI entities at startup; [0, 1000000]; restart
# test_spawn_spread_km    = 50.0     # phyllotaxis spread radius (km); [0, 100000]; restart
# test_spawn_agl_m        = 500.0    # spawn/loiter altitude above origin ground (m); [0, 50000]; restart
# test_spawn_ai_mix       = ""       # weighted controller mix, e.g. "loiter:60,pursuit:25,patrol:15"; restart (#580)
# test_projectile_rate    = 0.0      # short-lived entities spawned per second (churn); [0, 100000]; restart (#580)
# test_projectile_ttl_s   = 3.0      # churned-entity lifetime (s); [0.05, 600]; restart (#580)

[ai]
difficulty       = "pilot"        # what the server runs: AI radar range + reaction time; cadet|pilot|ace
difficulty_floor = "recruit"

[security]
pre_handshake_rate_limit_count = 20   # max CONNECT attempts per IP per window; 0 = disabled
pre_handshake_window_ms        = 1000 # sliding window in milliseconds
admin_auth_max_failures        = 5    # wrong operator passwords before per-IP lockout [1,100]
admin_auth_lockout_s           = 300  # per-IP lockout duration in seconds [1,86400]
idle_timeout_s                 = 0    # disconnect inactive peers after N seconds; 0 = disabled [0,86400]

[rcon]
enabled           = false
port              = 27015
password          = ""
max_auth_failures = 5    # lock out IP after N consecutive failed auth attempts
lockout_seconds   = 60   # per-IP lockout duration in seconds

[trace]
input_trace_dir = ""  # empty = disabled; per-peer FLIT input traces written here

[spawn]
agl_offset = 500.0  # metres AGL above terrain for all spawn points

# [[spawn.points]]
# x = 0.0
# z = 0.0

[flight]
size = 0  # AI wingmen spawned per connecting player; 0 = players fly alone

[network]
transport          = "gns"  # "gns" (GameNetworkingSockets, default) or "enet" (enet6)
allow_insecure     = true   # GNS only: accept unauthenticated peers (no Steam PKI)
compress_snapshots = true   # zstd snapshot payload compression (#775); hot-reloadable
gns_nagle_time_us  = 0      # GNS only: datagram-coalescing window, us; 0 = GNS default (5000)
```

---

## [server] — Server identity and player capacity

### `name`

| Type | Default |
|---|---|
| string | `"Unnamed Server"` |

Human-readable name shown in the lobby browser and startup log.

### `port`

| Type | Default | Valid range |
|---|---|---|
| integer | `4778` | 1–65535 |

UDP port `fl-server` binds on. Port 4778 is the fighters-legacy default. See the IANA
registration note in [docs/architecture.md](architecture.md).

### `bind_address`

| Type | Default |
|---|---|
| string | `"0.0.0.0"` |

Network interface to bind on.

- `"0.0.0.0"` — all interfaces; the standard setting for an internet-accessible server.
- `"127.0.0.1"` — localhost only; used by the game client when launching `fl-server` for
  single-player mode (`max_peers = 1`). See the single-player topology note in
  [docs/architecture.md](architecture.md).
- A specific IP — bind to one interface on a multi-homed host.

> **Phase 2:** Bind address enforcement requires `INetwork::bind()` to be extended to
> accept an address parameter. The value is parsed and stored now so config files remain
> stable; the restriction takes effect when that work lands.

### `max_peers`

| Type | Default | Valid range |
|---|---|---|
| integer | `32` | 1–1024 |

Maximum number of simultaneous connected peers. Values outside `[1, 1024]` are rejected
with a warning and the default is used instead. The ceiling was raised from 128 to 1024 by the
128+ multiplayer re-target so the [bot_swarm load harness](load-testing.md) can drive the server
past 128 to characterise the transport ceiling. **Note:** accepting a high `max_peers` is a
testing affordance, not a capacity guarantee — real high-peer capacity is the Phase 3–4 scaling
work.

### `game_modes`

| Type | Default |
|---|---|
| array of strings | `["campaign", "mission", "sandbox"]` |

Scenario types this server will host. Clients attempting to start a mode not in this list
are rejected. An empty array is treated as all modes allowed (equivalent to the default).

| Value | Description |
|---|---|
| `"campaign"` | Dynamic campaign — frontlines advance, story missions inject. |
| `"mission"` | Single scripted scenario loaded from a mission file. |
| `"sandbox"` | Free play, no win condition, session can save and resume. |

Whether a session is cooperative or adversarial depends on which faction players join,
not on a separate server-level flag.

### `motd`

| Type | Default |
|---|---|
| string | `""` (no message) |

Message delivered to each client immediately after `MsgConnectAck` via `MsgMotd` (0x08).
Empty string disables the MOTD. Multi-line MOTDs are supported; use a TOML triple-quoted string:

```toml
motd = """
Welcome to the server!
Rule 1: no teamkilling.
"""
```

Each line is printed separately in the client's game console prefixed with `[server]`.
The first line is also shown in the server notice banner; the banner fades out over the final
2 seconds before auto-dismissing. Display duration is set by `motd_display_s` (see below); when
`motd_display_s = 0` the client's own `[client].motd_display_s` in `user.toml` is used instead.

### `motd_display_s`

| Type | Default | Range |
|---|---|---|
| integer | `0` | 0 – 65535 |

How long (in seconds) the MOTD banner remains visible on each connecting client.

`0` (default) — the client uses its own `[client].motd_display_s` setting (default 15 s in
`user.toml`). A non-zero value overrides the client setting for this connection. Takes effect
immediately for each new connection; `reload_config` applies it to subsequent connections.

### `password`

| Type | Default |
|---|---|
| string | `""` (no password) |

Server password. Clients must supply this password to join. Empty string means the server
is open to all.

> **Security note:** Store passwords in `server.toml` only — do not use an environment
> variable. Environment variables appear in process listings (`ps`, `/proc/environ`) and
> are visible to other users on the same host. Use `FL_CONFIG` to point to a
> secrets-managed config file in container environments.

---

## [rotation] — Scenario rotation

> **Phase 4 (#854):** The server now **loads the first rotation item's mission** at startup
> (parsed by the engine mission runtime, spawns/factions/weather set up before the first tick).
> `--mission <name>` on the command line overrides this. Automatic cycling to later items over a
> running server (rotation *timing*) lands incrementally; the first item is live today.

### `order`

| Type | Default | Valid values |
|---|---|---|
| string | `"sequential"` | `"sequential"`, `"random"` |

Cycle order for rotation items.

### `items`

| Type | Default |
|---|---|
| array of strings | `[]` (no rotation) |

Ordered list of mission, campaign, or sandbox theater IDs to cycle through. IDs must
match those defined in the corresponding content files. Empty array means no automatic
rotation — the server stays on the current scenario.

### `time_limit_min`

| Type | Default |
|---|---|
| integer | `0` (no limit) |

Sandbox session time limit in minutes. When elapsed, the server advances to the next
rotation item. `0` disables the limit.

This value applies **only to sandbox sessions**. Mission and campaign sessions end when
their win/loss conditions are met, which are defined in the mission YAML or campaign TOML
content files — not here.

---

## [lobby] — Lobby registration

> **Phase 2:** Lobby registration is not yet active. These keys are parsed and stored;
> no requests are sent to `fl-lobby`. Tracked in issue #143.

### `register`

| Type | Default |
|---|---|
| boolean | `false` |

Set to `true` to advertise this server to the `fl-lobby` matchmaking service.

### `url`

| Type | Default |
|---|---|
| string | `"https://lobby.fighters-legacy.org"` |

`fl-lobby` REST base URL. Ignored unless `register = true`.

### `visibility`

| Type | Default | Valid values |
|---|---|---|
| string | `"public"` | `"public"`, `"private"` |

Server visibility in the lobby browser.

- `"public"` — visible to all players browsing the lobby.
- `"private"` — token-gated; only players with the correct invite token can see or join.
  Token-gating is a Phase 2 feature.

---

## [mods] — Mod stack

`fl-server` loads content packs automatically from the `mods/` subdirectory of its working
directory on startup. Packs are sorted by their declared `priority` field (higher = higher
priority). The `stack` key below is reserved for a future explicit-ordering feature and is
not yet used.

### `stack`

| Type | Default |
|---|---|
| array of strings | `[]` |

Reserved for a future explicit mod-ordering feature. When active, index 0 will be the
highest-priority mod ID; later entries will be lower priority. IDs must match the `[mod].id`
field in each mod's `manifest.toml`. See [docs/architecture.md](architecture.md) for the mod
manifest format.

Example:

```toml
[mods]
stack = ["fl-base-pack", "my-theater-mod"]
```

### `required`

| Type | Default |
|---|---|
| array of strings | `[]` |

Content packs a connecting client must have mounted (#872). Each entry is `"id"` or `"id@version"`
(an id-only entry accepts any version). The connect handshake carries the client's mounted-pack
manifest, and the server compares it against this list. IDs match the `[mod].id` field in each pack's
`manifest.toml`. **Restart-only.**

```toml
[mods]
required = ["fl-base", "my-theater@1.2"]
```

### `required_policy`

| Type | Default |
|---|---|
| string (`warn` \| `refuse` \| `allow_placeholder`) | `"warn"` |

What the server does when a connecting client is missing one of the `required` packs:

- **`warn`** — log the miss server-side (`peer N is missing required content pack '…'`) and send the
  admitted client a notice listing what it lacks, so a content mismatch is visible instead of silent
  placeholders. The client is **admitted**.
- **`refuse`** — disconnect the client with a `MsgConnectRefusal` whose reason names the missing
  pack(s). The client shows "You are missing a content pack this server requires." and prints the
  specific list to its console.
- **`allow_placeholder`** — admit silently and serve placeholders (today's implicit fallback, opted
  into). No client-facing notice.

An empty `required` list disables the policy entirely (no behavior change for existing servers).
An unrecognized `required_policy` value logs a warning and falls back to `warn`. **Restart-only.**

```toml
[mods]
required = ["fl-base"]
required_policy = "refuse"
```

---

## [world] — Persistent world settings

> **Phase 2:** These settings are only active when `fl-server` is launched with the
> `--persistent` flag (or `FL_PERSISTENT=true`). The keys are parsed and stored;
> persistent-world logic is not yet implemented.

### `save_path`

| Type | Default |
|---|---|
| string | `"world.sav"` |

Path to the persistent world save file, relative to the working directory or absolute.

### `autosave_interval_s`

| Type | Default |
|---|---|
| integer | `300` (5 minutes) |

Autosave interval in seconds. `0` disables autosaving.

### `time_scale`

| Type | Default |
|---|---|
| float | `10.0` |

Game seconds per real second. Controls the speed of the in-game day/night cycle.

| Value | Real-min → game-min | Full day/night cycle |
|---|---|---|
| `1` | 1:1 (real-time) | 24 real hours |
| `6` | 1:6 | 4 real hours |
| **`10` (default)** | **1:10** | **~2.4 real hours** |
| `20` | 1:20 | 72 real minutes |

At the default of 10×, a 30-minute mission passes ~5 game hours — enough to experience meaningful
lighting changes (e.g. afternoon → golden hour). Per-mission overrides are available via the
`time_scale` field in mission YAML files.

### `planet_radius_m`

| Type | Default | Range |
|---|---|---|
| float | `6371000.0` (Earth radius in metres) | `[1000, 1e9]` |

Planet sphere radius in metres. The engine always uses spherical-Earth physics and terrain curvature; this field sets the radius for non-Earth planets. `MsgConnectAck.planetRadiusKm` is set to `planet_radius_m / 1000` so clients match server physics. Out-of-range values are rejected with a warning and the default is used.

### `player_faction`

| Type | Default | Range |
|---|---|---|
| integer | `1` | `[0, 65535]` |

Faction stamped onto every player entity when it spawns on connect (#610).

**This must be non-zero for combat to work at all.** `fl::areFactionsHostile` treats faction 0 as
*neutral* — an entity with **no enemies**. With `player_faction = 0`, nothing in the world is hostile
to a player: an AI wingman's `engage_bandits` and `cover_me` orders can never trigger, and
`attack_my_target` can never designate a target. fl-server warns at startup if `[flight] size > 0` is
configured alongside `player_faction = 0`, because that combination is a silently broken wingman
rather than a configuration.

> **Behaviour change, introduced with the default of 1.** An AI spawned with a *different* faction now
> reacts to players where it previously could not see them. Concretely: a `spawn --faction 2 --ai
> escort <idx>` AI will now break-turn on an approaching player (its `escort` template triggers on
> `AnyHostileEntityWithinRange`). That is what #465 intended when it added factions. `patrol_attack`
> is **unaffected** — its transitions are keyed on a specific target entity, not on faction.
>
> All players share one faction, i.e. everyone is on the same side and no player is hostile to another
> player. That is the right default for a co-op sandbox and is not a regression (players could not
> harm each other before either). Per-team factions arrive with the multiplayer game-mode framework
> (Epic E).

Setting `0` restores the pre-#610 behaviour exactly.

### `player_entity_type`

| Type | Default | Range |
|---|---|---|
| string | `"builtin:debug-entity"` | any registered entity type id |

The aircraft a connecting **pilot** flies when the client requests no specific type (#834). A client
may request a specific type via `MsgConnectRequest` (game client flag `--aircraft <id>`); the server
**clamps** the request to a *registered* type — an unregistered request falls back to this default
(logged at Info), which itself falls back to `builtin:debug-entity` if unregistered. This makes "boot
a server, connect, look at the aeroplane" a config change instead of an engine patch: point it at a
pack aircraft (e.g. `player_entity_type = "fl-base:f5e"`) and every connecting pilot flies it.

### `allow_observers`

| Type | Default | Range |
|---|---|---|
| boolean | `true` | `true` / `false` |

Whether the server accepts **observer-role** (spectator) connections (#857). An observer joins with
no aircraft, `FlightIntegrator`, or controller and still receives world snapshots. When `false`, an
observer connect request is refused with `ConnectRefusalCode::RoleDenied`. A peer's role is also
switchable mid-session with the `set_role <peerId> <pilot|observer>` admin command.

### `draw_distance_km`

| Type | Default | Range |
|---|---|---|
| float | `200.0` | `[1, 100000]` |

Per-peer interest management radius in kilometres. Only entities within this XZ-plane radius of a peer's own entity are included in that peer's `MsgWorldSnapshot`. The default of 200 km covers any current Phase 2 theater. Out-of-range values are rejected with a Warn and the default is used. **Hot-reloadable** via `reload_config`.

> **Delta-baseline recovery is automatic (client-acked, #517).** There is no baseline-interval knob.
> The server keys full-vs-delta off the last snapshot tick each client echoes in
> `MsgClientInput`/`MsgHeartbeat`: an entity is re-sent as a full record every tick until that client
> acknowledges it, then it converges to deltas. A dropped full recovers in ~1 RTT, and there is no
> periodic cross-peer full-resync spike. See [network-protocol.md](network-protocol.md) → *Scaling to 128+*.

### `sensor_check_hz`

| Type | Default | Range |
|---|---|---|
| float | `10.0` | `[1, 60]` |

How many times per second each sensor runs its geometry + probability check (#685). Converted to a
tick stride, with checks **staggered** across it — observers do not all fire on the same tick, so the
cost spreads evenly instead of one tick in six carrying the whole world's sensing.

**10 Hz is the reference cadence every authored `pod` is tuned against.** A probability of detection
is meaningless without a rate: the same `0.35` is a different sensor at 1 Hz than at 60 Hz. Raising
this makes every sensor in the world acquire *faster*, and lowering it makes them slower — that is the
honest consequence of the knob, and it is **not** silently renormalized behind your back. If you
change it, you are re-tuning every content pack on the server, not just paying for more CPU.

Note this affects **acquisition** only. A contact already held is maintained by geometry, not by
re-rolling the die (see the sensor decision record in [architecture.md](architecture.md)), so the
cadence does not make locks flicker — and a coast still runs out in real seconds regardless of it.

**Hot-reloadable** via `reload_config`.

### `spatial_cell_size_km`

| Type | Default | Range |
|---|---|---|
| float | `10.0` | `[0, 1000]` |

`SpatialIndex` cell size in kilometres for per-peer interest queries and AI range queries. `0` selects
an auto heuristic derived from the draw distance (`clamp(draw_distance / 32, 500 m, 10 km)`) so a
full-radius query spans a bounded number of cells rather than degenerating toward O(N) at high density.
A cell much smaller than the draw distance is counter-productive — the query then iterates many
mostly-empty cells (see [entity-scale-characterization.md](entity-scale-characterization.md)).
Out-of-range values are rejected with a Warn and the default is used. **Restart-only** (reassigns the
index; not hot-reloaded).

### `snapshot_budget_bytes`

| Type | Default | Range |
|---|---|---|
| integer | `1200` | `[0, 65535]` |

Per-client snapshot byte budget (#516). When non-zero, each peer's `MsgWorldSnapshot` is capped at roughly this many bytes: the priority/budget scheduler ranks the visible entities by relevance (distance, closing-speed, recency, player-owned) and sends only the highest-priority set that fits, deferring the rest to later ticks. A recency term guarantees every visible entity is eventually sent, and the peer's own entity is always included. `0` disables the cap (every visible entity is sent every tick — the legacy behaviour). The default `1200` keeps a snapshot within a single ~1,400-byte MTU fragment (~72 KB/s at 60 Hz). Lower it to hold the per-client bandwidth gate as the player count grows, at the cost of lower-priority entities updating less frequently. Out-of-range values are rejected with a Warn and the default is used. **Hot-reloadable** via `reload_config`.

### `jitter_buffer_depth`

| Type | Default | Range |
|---|---|---|
| integer | `4` | `[1, 32]` |

Per-peer input ring buffer depth in sim ticks. Sets the **global cap**: the adaptive resize loop
(`jitter_buffer_adapt_window`, `jitter_buffer_hysteresis`, `jitter_buffer_jitter_multiplier`) may
reduce individual peer depths below this value but never increases them above it. Each connecting
peer's buffer is initialized to `min(estimatedDelayTicks, jitter_buffer_depth)` on their first
`MsgClientInput`, then continuously adjusted by the adaptive loop each tick. The server drains
exactly one input per sim tick before stepping the flight integrator; when the buffer runs empty the
last drained input is stale-repeated, preventing control surfaces from zeroing under transient packet
loss. Out-of-range values are rejected with a Warn and the default is used.
**Hot-reloadable** via `reload_config`.

### `jitter_buffer_adapt_window`

| Type | Default | Range |
|---|---|---|
| integer | `60` | `[10, 3600]` |

EWMA smoothing window in sim ticks for adaptive buffer sizing. The exponential moving average
weight is `alpha = 1/adapt_window`, so a window of 60 ticks (1 s at 60 Hz) gives each new
measurement a weight of ~1.7 %. Larger windows produce slower but more stable adaptation;
smaller windows respond faster to network changes but may oscillate. The EWMA is updated on
every accepted `MsgClientInput`; the resize check runs every tick for all connected peers.
Out-of-range values are rejected with a Warn and the default is used. **Hot-reloadable** via `reload_config`.

### `jitter_buffer_hysteresis`

| Type | Default | Range |
|---|---|---|
| integer | `2` | `[0, 8]` |

Dead-band in ticks around the current buffer depth. A resize fires only when
`|target_depth − current_depth| > hysteresis`, preventing rapid oscillation when the EWMA
hovers near a depth boundary. Set to `0` for immediate resizing on any EWMA change; set
higher for more stable depth under moderate jitter. Out-of-range values are rejected with a
Warn and the default is used. **Hot-reloadable** via `reload_config`.

### `jitter_buffer_jitter_multiplier`

| Type | Default | Range |
|---|---|---|
| float | `2.0` | `[0.0, 8.0]` |

Confidence factor `k` in the depth formula: `depth = ceil(ewma_delay + k × jitter_ewma)`.
The jitter EWMA tracks RFC 3550-style inter-arrival deviation from the expected 1-tick spacing.
Higher values add extra buffer headroom during bursty conditions; `0.0` disables the jitter
term entirely (pure EWMA-delay sizing, equivalent to #424 without #429). Out-of-range values
are rejected with a Warn and the default is used. **Hot-reloadable** via `reload_config`.

### `congestion_enabled` / `congestion_min_send_hz` / `congestion_loss_threshold` / `congestion_budget_floor_bytes`

| Key | Type | Default | Range |
|---|---|---|---|
| `congestion_enabled` | bool | `true` | — |
| `congestion_min_send_hz` | float | `10.0` | `[1, 60]` |
| `congestion_loss_threshold` | float | `0.02` | `[0, 1]` |
| `congestion_budget_floor_bytes` | integer | `400` | `[0, 65535]` |

Adaptive per-client send-rate / congestion response (#518). Each connected peer owns an AIMD
controller that the server steps every tick from that peer's ENet link quality (packet loss, RTT,
reliable bytes in flight). When a peer is judged congested — loss above `congestion_loss_threshold`,
RTT a margin above its running baseline, or a large reliable backlog — the server **decimates that
peer's snapshot rate** (60 Hz down toward `congestion_min_send_hz`) and **scales its byte budget
down** (never below `congestion_budget_floor_bytes`, and never below a `snapshot_budget_bytes` that is
already smaller); a healthy peer stays at the full rate and full budget. `congestion_enabled = false`
pins every peer to the full behaviour. There is no wire-format change — the client tolerates a variable
snapshot rate. Per-peer send rate and loss are shown by the `peers` admin command. Out-of-range values
are rejected with a Warn and the default is used. **Hot-reloadable** via `reload_config`. See
[congestion-control-design.md](congestion-control-design.md).

### `sim_worker_threads`

| Type | Default | Range |
|---|---|---|
| integer | `0` | `[0, 256]` |

Total CPU parallelism for the sim tick — the number of threads (including the sim thread) that
share the per-entity AI + integration work each tick. `0` = auto (sized from the host's logical
core count), `1` = serial (no worker pool). The parallel path is serial-equivalent (bit-identical
results), so this only affects CPU usage and throughput, never simulation outcome.

> **A CPU-parallelism knob, not a capacity guarantee.** Raising it lets the sim use more cores; it
> does not by itself raise the player ceiling — see Epic A / `docs/server-job-system-design.md`.

The CLI flag `--sim-worker-threads <n>` overrides this value (useful for load-test sweeps).
Out-of-range values are rejected with a Warn and the default is used. **Requires restart** to take
effect (the worker pool is built at startup).

### `test_spawn_ai_count` / `test_spawn_spread_km` / `test_spawn_agl_m`

| Key | Type | Default | Range |
|---|---|---|---|
| `test_spawn_ai_count` | integer | `0` | `[0, 1000000]` |
| `test_spawn_spread_km` | float | `50.0` | `[0, 100000]` |
| `test_spawn_agl_m` | float | `500.0` | `[0, 50000]` |

**A testing affordance, not a capacity guarantee.** When `test_spawn_ai_count > 0`, the server
pre-spawns that many server-side loiter-AI entities at startup, spread over a `test_spawn_spread_km`
disk (phyllotaxis pattern) at `test_spawn_agl_m` above the origin ground elevation. This exists to
stress the entity pool + `SpatialIndex` at thousands of entities (peers + AI) without needing that
many real clients — see [entity-scale-characterization.md](entity-scale-characterization.md) and
[load-testing.md](load-testing.md). The server *accepting* a large count does **not** mean it *serves*
that many players at rate. Leave at `0` for normal operation. Out-of-range values are rejected with a
Warn and the default is used. **Requires restart** (entities are spawned before the sim loop starts).

### `test_spawn_ai_mix` / `test_projectile_rate` / `test_projectile_ttl_s`

| Key | Type | Default | Range |
|---|---|---|---|
| `test_spawn_ai_mix` | string | `""` | behaviors `loiter` \| `pursuit` \| `patrol` |
| `test_projectile_rate` | float | `0.0` | `[0, 100000]` |
| `test_projectile_ttl_s` | float | `3.0` | `[0.05, 600]` |

**Testing affordances (#580), extending the load-spawn above.** `test_spawn_ai_mix` assigns the
pre-spawned entities a weighted controller mix instead of all-loiter, e.g.
`"loiter:60,pursuit:25,patrol:15"` (deterministic per-index assignment, no RNG). Per-tick AI cost
by behavior: `loiter` = pure guidance math (the #573 baseline); `pursuit` = `EntityManager::get()`
on a moving target; `patrol` = a `StateMachineController` whose `AnyEntityWithinRange` transitions
run `SpatialIndex::queryRadius()` every tick (the expensive AI path, and the one the overrun
governor's AI stride decimates). An invalid spec logs a Warn and falls back to all-loiter.

`test_projectile_rate > 0` enables the projectile-churn generator: that many short-lived entities
are spawned per second and each killed after `test_projectile_ttl_s` — sustained spawn+reap traffic
through the `EntityPool` free-list, the O(liveCount) `forEach`, and the `SnapshotDespawn` TLV path.
Steady-state extra population ≈ `rate × ttl`. Both **require restart**.

### `overrun_governor_enabled` / `overrun_high_watermark` / `overrun_low_watermark` / `overrun_min_snapshot_hz` / `overrun_max_ai_stride` / `overrun_budget_floor_bytes` / `overrun_min_interest_fraction`

| Key | Type | Default | Range |
|---|---|---|---|
| `overrun_governor_enabled` | bool | `true` | — |
| `overrun_high_watermark` | float | `0.90` | `[0.1, 1.0]` |
| `overrun_low_watermark` | float | `0.60` | `[0.0, high)` |
| `overrun_min_snapshot_hz` | float | `15.0` | `[1, 60]` |
| `overrun_max_ai_stride` | integer | `4` | `[1, 32]` |
| `overrun_budget_floor_bytes` | integer | `400` | `[0, 65535]` |
| `overrun_min_interest_fraction` | float | `0.5` | `[0.1, 1.0]` |

The graceful tick-overrun governor (#514, #726). When the authoritative tick's measured wall-time
exceeds its fixed-step budget (~16.667 ms at 60 Hz) under load, the governor **sheds work** to bring
the tick back under budget rather than spiralling or silently dilating time. It tracks an EWMA of
per-tick wall-ms and, when it crosses `overrun_high_watermark × budget`, lowers a server-wide
`loadFactor` that drives four composing levers (on top of the per-client congestion response):

- **snapshot send-rate decimation** — broadcasts are spaced out server-wide, down toward
  `overrun_min_snapshot_hz`;
- **per-client byte-budget scaling** — the snapshot budget is scaled down (never below
  `overrun_budget_floor_bytes`), so the priority/budget scheduler defers more low-relevance entities;
- **AI-sample decimation** — non-player (AI/scripted) entities have their controller `sample()`
  skipped on some ticks (reusing their last command), up to `overrun_max_ai_stride`. Players are never
  decimated, and the **physics integration step always runs every tick** (so flight stays stable);
- **interest-radius shedding** (#726) — each peer's effective interest radius (`draw_distance_km`) is
  scaled down with `loadFactor`, never below `overrun_min_interest_fraction` of the configured value
  (`1.0` disables this lever). Unlike the byte-budget lever, which trims the encoded output *after*
  ranking the full visible set, this shrinks the visible set itself — the interest query, scheduler
  ranking, and encode all get cheaper together. Entities leaving the shrunk radius are ordinary
  interest-out (the client's retention window and force-full re-entry handle their return); nothing
  is despawned and there is no wire change.

The governor recovers (raises `loadFactor` back toward 1) once the EWMA drops below
`overrun_low_watermark × budget`. `overrun_governor_enabled = false`, or any tick comfortably under
budget, pins `loadFactor = 1` — identical to the pre-#514 behaviour, no wire-format change. The
current load is shown by `status` (`load: NN%  interest: NN%`) and `tickstats`, and exported in
`--metrics-json` (`load_factor`, `interest_scale`). Out-of-range values are rejected with a Warn and
the default is used. **Hot-reloadable** via `reload_config`.

> The governor reduces snapshot/AI *work*. If the **integration** step alone exceeds budget (a fully
> CPU-bound sim), no lever can help and the `max_catchup_ticks` backstop absorbs it as bounded time
> dilation — surfaced as a rising `dropped_ticks` / a `Warn` log.

### `max_catchup_ticks`

| Type | Default | Range |
|---|---|---|
| integer | `8` | `[1, 64]` |

The `GameLoop` catch-up cap — the maximum number of sim ticks executed in a single loop iteration when
the sim falls behind. This is the spiral-of-death backstop: beyond the cap, excess accumulated time is
discarded (the sim time skips forward) rather than the loop trying to catch up forever. The discarded
count is exported as `dropped_ticks` in `--metrics-json` and logged as a `Warn` when it rises (a
sustained nonzero rate means the sim cannot keep up even after the governor sheds work). Out-of-range
values are rejected with a Warn and the default is used. **Requires restart** to take effect (it is a
`GameLoop` construction value).

---

## [ai] — AI policy

### `difficulty`

| Type | Default | Values |
|---|---|---|
| string | `"pilot"` | `cadet` \| `pilot` \| `ace` |

**What the server actually runs** (#682) — the first server-side consumer of the difficulty system.
Resolved at startup to an `AiScaling` and fed into the sim tick, where the sensing pass (#685) uses
two of its fields:

- **`radarSensorRange`** — a fraction applied to the max range of **radar** sensors only. A Cadet
  server's AI radar reaches half as far as its content pack says it does; an Ace server's reaches
  all the way.
- **`reactionTimeS`** — the base delay between an AI **detecting** a contact and **acting** on it,
  scaled per-unit by the entity's `[ai].reaction`.

It gates **acting**, not **seeing**: a Cadet AI detects a target at exactly the same moment an Ace
one does — it just takes longer to do anything about it (and, for radar, has to be closer to detect
it at all). A knob that made the low-difficulty AI's *eyes* worse would be a different, dishonest
thing.

The table is **mod-overridable**: the preset values come from `data/difficulty.toml` through the
AssetManager (highest-priority pack wins), exactly like the client path, so a content pack tunes its
own AI without patching the server. Unknown values log a Warn and keep the default.

**Hot-reloadable** via `reload_config`.

### `difficulty_floor`

> **`difficulty` and `difficulty_floor` are different things.** `difficulty` is what the server runs
> right now. `difficulty_floor` is the future **per-client clamp** — the minimum a connecting player
> may set for themselves. It is still parsed-and-stored only.


| Type | Default |
|---|---|
| string | `"recruit"` |

Minimum AI difficulty enforced server-side, regardless of individual client preference.

| Value | Description |
|---|---|
| `"recruit"` | Easiest; forgiving reaction times and aim. |
| `"cadet"` | Moderate challenge; suitable for newer players. |
| `"veteran"` | Competent AI; expects experienced players. |
| `"ace"` | Hardest; optimal tactics and near-perfect aim. |

---

## [gameplay] — The damage gates

Server-authoritative (#626): single-player configures these through the same path via the embedded
`fl-server`, never client-side. Both hot-reload via `reload_config`.

### `friendly_fire`

| Type | Default |
|---|---|
| bool | `false` |

When `false`, weapon damage from an instigator sharing the target's **non-zero** faction is
suppressed. Faction `0` is *neutral*, not a team — neutral-on-neutral damage always applies, as do
self-damage (your own blast radius) and environmental damage.

### `crash_damage`

| Type | Default |
|---|---|
| bool | `true` |

When `true`, a hard ground impact damages the airframe (scaling with impact speed past a
survivable-arrival threshold). Ordinary landings are never affected.

---

## [discovery] — LAN server discovery

Configures the UDP broadcast beacon that lets players on the same LAN find this server
automatically. The beacon is a raw UDP packet sent on `255.255.255.255:<port>` (IPv4 broadcast)
and `[ff02::1]:<port>` (IPv6 link-local multicast) every `interval_ms` milliseconds. It is
independent of ENet and requires no router configuration.

Client-side parsing and the server browser UI are tracked in issue #143.

### `enabled`

| Type | Default |
|---|---|
| bool | `true` |

Set to `false` to suppress LAN broadcasting entirely. Recommended for internet-only servers or
servers where LAN presence is undesirable (e.g. tournament setups, cloud deployments).

### `interval_ms`

| Type | Default | Valid range |
|---|---|---|
| integer | `2000` | 100–60000 |

How often to broadcast the beacon, in milliseconds. Out-of-range values are ignored and the
default is kept (a warning is logged).

---

## [flight] — The player's flight (AI wingmen)

Gives each connecting player a flight of AI wingmen they can order from the in-game radio menu (`C`).
This is the scripted wingman (#610) — the zero-AI path, and what a server ships instead of the
natural-language wingman when no LLM provider is configured (or when the provider is CPU-only and
cannot meet the 2 s radio-comms budget; see `docs/ai-architecture.md` §9).

```toml
[flight]
size = 0  # AI wingmen spawned per connecting player; 0 = disabled; [0, 8]

# entity_type = "builtin:debug-entity"

# Formation geometry: slot spacing per rank. Members alternate right/left and step out,
# back and down each rank, so a flight of any size stacks into a legible echelon.
# lateral_m  = 150.0    # [10, 5000]
# aft_m      = 100.0    # [0, 5000]
# vertical_m = -15.0    # negative = stepped down; [-1000, 1000]

# Behaviour tuning for the six scripted commands.
# engage_range_m           = 12000.0  # engage_bandits trigger radius about the WINGMAN
# cover_range_m            = 6000.0   # cover_me trigger radius about the LEAD
# designate_range_m        = 15000.0  # attack_my_target boresight range
# designate_half_angle_deg = 15.0     # boresight cone half-angle; [1, 90]
# command_rate_limit_per_s = 4        # wingman orders per second per player; [1, 60]
```

### `size`

| Type | Default | Range |
|---|---|---|
| integer | `0` | `[0, 8]` |

AI wingmen spawned for each connecting player. **Defaults to 0 deliberately:** N extra AI entities
per peer would move every load-test and scale-gate number, so a dedicated server is byte-for-byte
unchanged unless you ask for a flight.

**Single-player is unaffected** — the game client starts its embedded server with `--flight-size 1`,
so you always fly with a wingman there. The `--flight-size <n>` CLI flag overrides this key.

> **Requires `[world] player_faction` to be non-zero** (it is, by default). With `player_faction = 0`
> the player is *neutral*, a neutral entity has **no enemies**, and so the wingman's `engage_bandits`
> and `cover_me` orders can never trigger and `attack_my_target` can never designate. fl-server logs a
> warning at startup if you configure a flight alongside `player_faction = 0`.

### Larger formations, all-AI flights, and strike packages

This section only describes what a *player* gets on connect. The formation model underneath is a
command **tree** — a formation is `{anchor, commander, members, children}`, the commander need not be
in the formation, and a member may be an AI **or another player**. Everything past the default player
flight is built at runtime with the **`flight` admin command** (see
[Runtime administration](#runtime-administration)):

    flight create 12 --callsign Chevy --commander 3   # an AI flight commanded by peer 3 (an AWACS)
    flight add 1 13                                   # add an aircraft to flight 1
    flight order 1 return_to_base --cascade           # order flight 1 and everything under it home

An order to a **human** member is *relayed* to them as a radio call, never applied — the server
cannot fly a person's aircraft for them.

## [network] — Transport backend

Selects the network transport. See [transport-selection.md](transport-selection.md) and
[gns-backend.md](gns-backend.md).

### `transport`

- **Type:** string — `"gns"` or `"enet"`
- **Default:** `"gns"`
- **Restart required.**

`"gns"` = **GameNetworkingSockets**: encrypted UDP (curve25519 + AES-GCM), mature congestion
control, 128+ connection headroom — the recommended default for dedicated servers. `"enet"` =
**enet6**: lighter, for LAN / single-player / low-count servers. An unrecognised value logs a warning
and keeps the default. Overridable with the `--transport <gns|enet>` CLI flag (highest precedence).

> The server must be **built with `-DFL_ENABLE_GNS=ON`** (the default) to use `"gns"`; an enet6-only
> build falls back to enet6 with a warning. `net_check` speaks enet6 only; `bot_swarm` takes
> `--transport enet|gns` (#649) and the load-test runners pin both ends to the same backend via
> `FL_LOADTEST_TRANSPORT`.

### `allow_insecure`

- **Type:** bool
- **Default:** `true`
- **GNS only** (ignored by the enet backend). **Restart required.**

Accept unauthenticated peers. Standalone GNS has no Steam PKI, so connections are **encrypted but
unauthenticated** (opportunistic, like TLS-without-cert). Maps to GNS `AllowWithoutAuth`. Identity /
account authentication (Epic C) rides an in-band wire message on top of the encrypted channel.

### `compress_snapshots`

- **Type:** bool
- **Default:** `true`
- **Hot-reloadable** via `reload_config`.

zstd-compress snapshot payloads at the engine layer (#775). GNS (the default transport) does not
compress at all, so this is where its wire bytes come down: measured at 128 clients, idle wire
drops 78 % (to below enet6's range-coder figure) and active patterns ~15 %. Tiny or incompressible
snapshots are automatically sent raw; the CPU cost is a few microseconds per peer per tick inside
the parallel snapshot build. **One caveat on the `enet` transport:** ENet's own range coder cannot
compress zstd output, and its whole-packet compression is slightly better than payload-only zstd —
measured ~+10 % wire on active patterns with both enabled. A bandwidth-sensitive enet6 server can
set this `false` and let the range coder do the work; on GNS leave it on. The load-test runners
expose `FL_LOADTEST_COMPRESSION=0` for raw A/B legs.

### `gns_nagle_time_us`

| Type | Default | Valid range |
|---|---|---|
| integer (microseconds) | `0` | `0` – `200000` |

**GNS only** (ignored by the enet backend). **Restart required.**

Datagram-coalescing (Nagle) window: small sends and protocol acks wait up to this long to share a
datagram (GNS `k_ESteamNetworkingConfig_NagleTime`). `0` keeps GNS's library default (5000 µs).
Larger values cut per-datagram framing/AEAD overhead at the cost of up to that much added delivery
latency — measure the RTT effect before raising it (see
[load-testing.md](load-testing.md)).

---

## [security] — Access control and rate limiting

### `connect_rate_limit_count`

| Type | Default | Valid range |
|---|---|---|
| integer | `5` | 1–100000 |

Maximum number of times a single IP address may complete an ENet connection handshake within
`connect_rate_limit_window_s` seconds. Peers that exceed this count are immediately disconnected.
The ceiling was raised from 100 to 100000 so a load test can admit a large rapid ramp from
`127.0.0.1` (see [bot_swarm](load-testing.md)).
Note: limiting applies post-handshake (see [Access control](#access-control) for details).

### `connect_rate_limit_window_s`

| Type | Default | Valid range |
|---|---|---|
| integer | `10` | 1–3600 |

Sliding time window (in seconds) for the per-IP connection rate limiter.

### `packet_flood_multiplier`

| Type | Default | Valid range |
|---|---|---|
| integer | `3` | 1–100 |

A connected peer that sends more than `packet_flood_multiplier × 60` `MsgClientInput` packets
per second is disconnected. At the default of 3, the threshold is 180 packets/s — three times
the normal 60 Hz client rate. Set to 2 or higher to avoid false positives on 60 Hz clients.
`MsgClientInput` is delivered on the unreliable channel (channel 1); flood detection counts
received packets regardless of channel.

### `pre_handshake_rate_limit_count`

| Type | Default | Valid range |
|---|---|---|
| integer | `20` | 0–10000 |

Maximum number of ENet CONNECT packets accepted from a single IP address within
`pre_handshake_window_ms` milliseconds, checked **before** ENet allocates peer state.
Packets that exceed this count are silently dropped at the intercept layer — the
client receives no error; ENet retries are also dropped until the window expires.

Set to `0` to disable pre-handshake rate limiting entirely.

This complements the post-handshake rate limiter (`connect_rate_limit_count`): together
they defend against both syn-flood resource exhaustion (pre-handshake) and repeated-login
probing (post-handshake).

### `pre_handshake_window_ms`

| Type | Default | Valid range |
|---|---|---|
| integer | `1000` | 100–60000 |

Sliding window size in milliseconds for `pre_handshake_rate_limit_count`. Out-of-range
values are rejected with a warning and the default is kept.

### `max_connections_per_ip`

| Type | Default | Valid range |
|---|---|---|
| integer | `0` (unlimited) | 0–1024 |

Maximum number of simultaneous connections allowed from a single IP address. When non-zero,
`onConnect` counts the number of currently-connected peers from the same IP and disconnects
immediately if the count would reach or exceed the limit.

Set to `0` (default) to disable this check. This is distinct from `connect_rate_limit_count`,
which limits connection *attempts* per time window; `max_connections_per_ip` limits *held*
connections. Both can be active simultaneously.

### `banlist_path`

| Type | Default |
|---|---|
| string | `""` (disabled) |

Path to the persistent ban list file. One normalized IP address per line; lines beginning
with `#` are treated as comments. When configured, the `ban` and `unban` admin commands
automatically overwrite this file. Empty = in-memory only (bans lost on restart).

### `allowlist_path`

| Type | Default |
|---|---|
| string | `""` (disabled) |

Path to an allowlist file (same format as `banlist_path`). When non-empty, only IP addresses
listed in this file may connect. The ban list still takes precedence over the allowlist —
a banned IP is rejected even if it appears in the allowlist. Empty = all IPs permitted.

### `incoming_bandwidth_bps` / `outgoing_bandwidth_bps`

| Type | Default |
|---|---|
| integer | `0` (unlimited) |

Aggregate ENet host bandwidth caps in bytes per second. `0` = unlimited (ENet default).
`incoming_bandwidth_bps` caps total inbound traffic from all peers combined.
`outgoing_bandwidth_bps` caps total outbound traffic to all peers combined.

### `operator_password`

| Type | Default | Env override |
|---|---|---|
| string | `""` (disabled) | `FL_OPERATOR_PASSWORD` |

Password for the network-level authenticated admin command channel (`MsgAdminCommand`,
`MsgId = 0x06`). When non-empty, connected game clients that know this password can send
admin commands (e.g. `spawn`, `kill`, `tp`, `set_weather`) over ENet — the same commands
available on the stdin console — and receive text responses via `MsgAdminResponse` (short
results, ≤ 123 chars) or a sequence of `MsgAdminResponseChunk` (0x0A) packets (long results).
Commands that enqueue a sim-thread mutation (e.g. `spawn`, `kill`, `tp`, `ban`) send a brief
queued-ack immediately, followed by a deferred confirmation packet — carrying the actual result
(e.g. entity index, new position) — within approximately one sim tick (~16 ms). The deferred
packet shares the same `reqId` as the original command.

Empty string (default) **disables** the network admin channel entirely; stdin-only access
is still available.

**Single-player:** `LocalServer` automatically generates a random 24-character hex session
token at startup and passes it to `fl-server` via `--admin-token`. The game client uses this
token transparently. You do not need to configure `operator_password` for single-player.

**Security:** The token travels over UDP (ENet). Use this channel only on trusted private
networks or behind a VPN. Passwords longer than 29 characters are silently truncated by the
client (the wire field is 30 bytes including the NUL terminator). Long command output
(e.g. `peers` with many players) is streamed as a sequence of `MsgAdminResponseChunk`
(0x0A) packets; there is no per-reply character cap.

### `admin_auth_max_failures`

| Type | Default | Valid range |
|---|---|---|
| integer | `5` | 1–100 |

Maximum consecutive wrong-password attempts allowed on the `MsgAdminCommand` channel before
the source IP is locked out. Once the threshold is reached the offending peer is kicked and
reconnections from that IP are refused until the lockout TTL expires (see `admin_auth_lockout_s`).
Set to `1` to lock out on the first failure.

The failure counter is per-IP and persists across disconnect/reconnect — so an attacker cannot
reset the counter by reconnecting. A successful authentication clears the counter for that IP.

### `admin_auth_lockout_s`

| Type | Default | Valid range |
|---|---|---|
| integer | `300` (5 minutes) | 1–86400 |

Per-IP lockout duration in seconds after `admin_auth_max_failures` consecutive wrong passwords.
During the lockout window, any new connection from the same IP is refused immediately (no
`MsgHello` sent). The lockout expires automatically, can be inspected with `admin_auth_status`, or cleared immediately with `admin_unlock` (which also clears the RCON channel lockout for the same IP when RCON is enabled).

### `idle_timeout_s`

| Type | Default | Valid range |
|---|---|---|
| integer | `0` (disabled) | 0–86400 |

Disconnect any peer that sends neither `MsgClientInput` nor `MsgHeartbeat` for this many seconds.
`0` (default) disables the check. Recommended value for public servers: `60`–`300`.

Idle clients include spectators, players in menus, and connections that have stalled without ENet
detecting a timeout. This provides an application-level cleanup mechanism independent of ENet's own
peer timeout. The game client sends `MsgHeartbeat` automatically at 1 Hz while in the flight screen.

---

## [shutdown] — Graceful shutdown settings

```toml
[shutdown]
warning_interval_s  = 300   # seconds between countdown broadcast notices (default 5 min)
min_shutdown_delay_s = 0    # minimum seconds of warning required; 0 = no minimum
require_confirm      = true # require --force flag before scheduling; set false to skip prompt
```

### `warning_interval_s`

How often (in seconds) the server broadcasts a countdown notice to connected clients during a
shutdown sequence. Valid range: `[1, 86400]`. Default: `300` (5 minutes).

### `min_shutdown_delay_s`

Minimum allowed delay (in seconds) when scheduling a shutdown via `shutdown --in <dur>`. The
`shutdown --now` command bypasses this minimum. Valid range: `[0, 86400]`. Default: `0`.

### `require_confirm`

When `true` (default), the `shutdown --in` and `shutdown --now` commands require a `--force` flag
to proceed; without it, the server prints a preview and asks the operator to re-run with `--force`.
Set to `false` on automated/scripted environments where the confirmation prompt is unwanted.

All `[shutdown]` fields take effect immediately — no restart required.

---

## [rcon] — Remote Console (RCON)

Enables a TCP RCON listener using the Source Engine RCON wire protocol. Compatible with
standard RCON clients such as `mcrcon`, `rcon-cli`, and any tool that speaks the Source
Engine RCON protocol. The server exposes the same command set as the stdin console.

> **Security:** RCON passwords travel over **plain TCP** (no TLS). Use RCON only on
> trusted private networks, VPNs, or behind a TLS-terminating reverse proxy. Do not
> expose the RCON port to the public internet without additional protection.

```toml
[rcon]
enabled           = false
port              = 27015
password          = ""
max_auth_failures = 5
lockout_seconds   = 60
```

### `enabled`

| Type | Default |
|---|---|
| boolean | `false` |

Set to `true` to start the TCP RCON listener. All other `[rcon]` fields are ignored when
`enabled = false`. If `enabled = true` and `password` is empty, a warning is logged and
unauthenticated connections are accepted — do not leave `password` empty in production.

### `port`

| Type | Default | Valid range |
|---|---|---|
| integer | `27015` | 1–65535 |

TCP port the RCON listener binds on. The default (`27015`) is the Source Engine RCON
convention. Out-of-range values are ignored and the default is kept (a warning is logged).

### `password`

| Type | Default |
|---|---|
| string | `""` (empty) |

Password required for RCON authentication. Empty string means no password is required
(a startup warning is logged when `enabled = true` and `password` is empty).
Passwords are compared in constant time to resist timing attacks.

### `max_auth_failures`

| Type | Default | Valid range |
|---|---|---|
| integer | `5` | 1–1000 |

Number of consecutive failed `SERVERDATA_AUTH` attempts from the same IP before
that IP is temporarily locked out. Out-of-range values are ignored and the default
is kept (a warning is logged).

### `lockout_seconds`

| Type | Default | Valid range |
|---|---|---|
| integer | `60` | 1–86400 |

How long (in seconds) a locked-out IP is refused new RCON connections. Locked-out
connections receive an `AUTH_RESPONSE id=-1` immediately on connect and are closed.
Out-of-range values are ignored and the default is kept (a warning is logged).

### Behaviour notes

- A maximum of 4 simultaneous RCON connections are accepted. Additional connections receive
  an error response and are closed immediately.
- Repeated failed auth attempts are rate-limited per source IP: after `max_auth_failures`
  consecutive failures the IP is locked out for `lockout_seconds`. Locked-out connections
  receive an immediate `AUTH_RESPONSE id=-1` and are closed before any packets are processed.
- Command responses longer than 4086 bytes are split across multiple `SERVERDATA_RESPONSE_VALUE`
  packets per the Source Engine RCON specification, followed by an empty sentinel packet.
- The RCON lockout TTL expires automatically; use `admin_auth_status` to view RCON lockout
  state, or `admin_unlock <IP>` from the admin console or stdin to clear a lockout early without
  waiting.
- Async-mutating commands (`kick`, `ban`, `unban`, `tp`, `spawn`, `kill`) return a
  synchronous acknowledgement string immediately. The actual action executes on the next sim
  tick (~16 ms later); confirmation also appears on fl-server stdout and is sent to the RCON
  client as a second `SERVERDATA_RESPONSE_VALUE` packet (~20 ms after the initial acknowledgement).
- `peers` returns a count from the atomic peer counter immediately; the full per-peer detail
  (including one-way delay in ticks and approximate milliseconds) is printed to stdout and
  sent to the RCON client as additional `SERVERDATA_RESPONSE_VALUE` packets on the next sim tick.
- `admin_auth_status` returns the full per-IP lockout and failure detail as the synchronous
  response body (no second packet), unlike `peers`. Output is split into a
  `MsgAdminCommand channel:` section and, when RCON is enabled, an `RCON channel:` section.
  Both RCON and ENet admin clients receive the complete detail in the immediate response.
  Output also appears on fl-server stdout.

### Example: connect with mcrcon

    mcrcon -H <host> -P 27015 -p <password> "status"

All `[rcon]` fields **require a restart** to take effect.

---

## [metrics] — Tick-budget export

Exports the per-phase server tick budget (maintenance / integrate / ai / collision /
serialize / total) as a JSON file, written atomically (`.tmp` → rename) every
`tick_json_interval_ms`. Disabled when `tick_json_path` is empty. The same data is available
live via the `tickstats` admin command and summarised in `status`.

```toml
[metrics]
tick_json_path = ""          # empty = disabled; absolute or relative path
tick_json_interval_ms = 1000 # write cadence, ms; [100, 60000]
```

| Key | Type | Default | Notes |
|---|---|---|---|
| `tick_json_path` | string | `""` | Output file; empty disables export. `--metrics-json` overrides this. |
| `tick_json_interval_ms` | int | `1000` | Write cadence in milliseconds; clamped to `[100, 60000]`. |

The JSON schema (also the shape embedded as `server_tick` in the `bot_swarm` report) is
documented in [docs/load-testing.md](load-testing.md#authoritative-server-tick-budget-server_tick).
`bot_swarm --server-metrics <path>` consumes this file; the #520 CI gate asserts on its
`tick_ms.p99`.

## [trace] — Server-side input tracing

Records every peer's **accepted** (post-validation) `MsgClientInput` to a per-peer FLIT trace
file `trace_peer<id>_<n>.flit` in `input_trace_dir` (created if missing). Captured sessions —
including live multiplayer — replay at scale with `bot_swarm --pattern trace:<file>`, and the
format is the versioned server-side input stream the Phase 4 replay epic (#588) builds on.
Disabled when `input_trace_dir` is empty; toggle at runtime with the `trace_start [dir]` /
`trace_stop` admin commands.

```toml
[trace]
input_trace_dir = ""   # empty = disabled; per-peer FLIT traces written here
```

| Key | Type | Default | Notes |
|---|---|---|---|
| `input_trace_dir` | string | `""` | Directory for per-peer traces; empty disables tracing. |

The FLIT trace format (header + 28-byte records) is documented in
[docs/load-testing.md](load-testing.md#trace-replay-560).

## [spawn] — Peer spawn locations

Controls where connecting peers appear in the world. Terrain elevation at each
configured point is queried from `TerrainStreamer` on the main thread before
`gameLoop.start()` and cached; changing spawn points **requires a server restart**.

```toml
[spawn]
agl_offset = 500.0  # metres AGL above terrain for all spawn points

# Peer spawn locations assigned round-robin to connecting peers.
# Omit this section to use the default (origin at x=0, z=0).
# [[spawn.points]]
# x = 0.0
# z = 0.0
```

### `agl_offset`

| Type | Default | Range |
|---|---|---|
| float | `500.0` | `[0, 50000]` |

Metres above ground level (AGL) added to the cached terrain elevation at each
spawn point. Applies to all points uniformly.

### `[[spawn.points]]`

Array of tables, each with `x` and `z` fields (world-space metres). Peers are
assigned round-robin in connection order. Omitting this section (or providing
an empty array) defaults to a single spawn at origin `(0, 0)`.

| Field | Type | Description |
|---|---|---|
| `x` | float | World-space X coordinate (metres) |
| `z` | float | World-space Z coordinate (metres) |

**Example — two spawn points:**

```toml
[spawn]
agl_offset = 500.0

[[spawn.points]]
x = 0.0
z = 0.0

[[spawn.points]]
x = 10000.0
z = -5000.0
```

---

## Runtime administration

### Stdin console

`fl-server` accepts admin commands on standard input. No extra port or network
exposure is required — access is limited to anyone with shell access to the
process.

#### How to attach

| Environment | Command |
|---|---|
| Local terminal | Type commands directly when running `fl-server` in the foreground |
| Docker | `docker exec -i <container> fl-server` (note: `-i` for stdin) |
| Kubernetes | `kubectl exec -it <pod> -- /bin/sh`, then interact with the process |

> **Windows note:** The stdin console is unavailable when `fl-server` runs without
> an attached console (e.g. as a Windows Service). Use Docker or SSH in those environments.

#### Command reference

| Command | Args | Description |
|---|---|---|
| `help` | `[command]` | List all commands, or show usage for a specific one |
| `status` | — | Show uptime, peer count, entity count, the real tick rate (Hz + mean/p99 ms), and the overrun-governor load (`load: NN%`, `[DEGRADED]` when shedding) |
| `tickstats` | — | Per-phase sim tick budget (integrate/ai/collision/serialize/total; ms mean/p95/p99/max), actual tick Hz, and the overrun-governor state (`load`, effective snapshot Hz, AI stride) |
| `peers` | — | List connected peers (peer ID, address, entity index/generation, one-way delay in ticks/ms, input queue buffer fill/max, adaptive snapshot send rate `rate=NN Hz`, ENet packet loss `loss=N.N%`) |
| `kick` | `<peerId\|IP>` | Disconnect a peer by numeric ID, or all peers from an IP address |
| `ban` | `<peerId\|IP>` | Add IP to the ban list and kick matching peers; saves to `banlist_path` if configured |
| `unban` | `<IP>` | Remove an IP from the ban list; saves to `banlist_path` if configured |
| `admin_unlock` | `<IP>` | Clear the admin auth and RCON auth lockouts for an IP address immediately; prints a warning if neither channel was locked (idempotent) |
| `admin_auth_status` | — | Show per-IP lockout state for the MsgAdminCommand operator channel and (when RCON is enabled) the RCON TCP channel; both active lockouts and pending failure counts |
| `set_weather` | `<preset>` | Change weather: `clear`, `partly_cloudy`, `overcast`, `rain`, `storm`, `snow`, `blizzard` |
| `detonate` | `<x> <y> <z> <radius_m> <damage> [--nuclear]` | AoE warhead at a world position (#356); `--nuclear` adds the EMP ring at 4× the blast radius |
| `set_time` | `<0–24>` | Set in-game time of day (float, hours) |
| `spawn` | `<type> <x> <y> <z> [--ai <behavior> [args...]]` | Spawn a registered entity type at the given world position; optionally attach an AI controller. C++ behaviors: `loiter [cx cy cz [radius_m [alt_m [throttle [cw\|ccw]]]]]`, `waypoint x y z [x y z ...] [--loop]`, `formation <anchorIdx> [slot] [lateralM] [aftM]` (holds station on a **moving** anchor — unlike `escort`, which orbits the point where the escortee stood when the order was given), `wingman <anchorIdx> <command> [slot]`, `pursuit <entityIdx>`, `evade <entityIdx>`, `break <entityIdx> [rollDuration]`, `lead <entityIdx> [navGain]`, `lag <entityIdx> [lagFraction]`, `immelmann [pullDur] [rollDur]`, `split_s [rollDur] [pullDur]`, `high_yo_yo <entityIdx> [climbDur] [reacquireDur]`, `low_yo_yo <entityIdx> [diveDur] [pullDur]`, `guns <entityIdx> [muzzleVel] [lethalRadius]` (ballistic-lead gunnery with trigger discipline), `ballistic <tx> <ty> <tz> [mirvCount [spreadM]]` (#355 — boost-phase steering to an impact point; MIRV past apogee). Lua behavior: `lua <script_name>` (loads `ai/<script_name>.lua` from content packs; see `docs/modding/ai.md`). If the entity type's TOML sets `ai_script`, that script is attached automatically when `--ai` is omitted. |
| `flight` | `list \| create <anchorIdx> [--commander <peerId>] [--parent <id>] [--callsign <name>] \| add <id> <entityIdx> [slot] \| order <id> <command> [--member <idx>] [--cascade] \| disband <id>` | The formation / command-hierarchy surface (#610) — the **game-master and AWACS path**. Build formations (including all-AI flights and nested strike packages) and order them. `order` takes the six wingman commands (`attack_my_target`, `engage_bandits`, `rejoin`, `cover_me`, `hold_fire`, `return_to_base`); `--cascade` applies it to every sub-formation beneath the addressed one. Dispatches through the **same** code as the network order path, so a console order and a radio order cannot behave differently. `attack_my_target` is **refused** here — it needs a commander's boresight, which the console does not have; use `spawn --ai pursuit <idx>` to point an AI at a specific entity. |
| `kill` | `<idx>` | Remove a live entity by pool index (see `peers` output) |
| `tp` | `<idx> <x> <y> <z>` | Teleport entity `<idx>` to world position; also used by the game client's game console to teleport the player entity |
| `reload_config` | — | Re-read `server.toml` and apply: `name` (beacon), `motd`, `motd_display_s`, `draw_distance_km`, `snapshot_budget_bytes`, `jitter_buffer_depth`, `jitter_buffer_adapt_window`, `jitter_buffer_hysteresis`, `jitter_buffer_jitter_multiplier`, `congestion_*`, `overrun_governor_enabled`, `overrun_high_watermark`, `overrun_low_watermark`, `overrun_min_snapshot_hz`, `overrun_max_ai_stride`, `overrun_budget_floor_bytes` (all take effect on the next sim tick; `max_catchup_ticks`, `sim_worker_threads`, `spatial_cell_size_km`, and the `test_spawn_*` keys require restart) |
| `reload_banlist` | — | Re-read `security.banlist_path` from disk and apply immediately |
| `reload_allowlist` | — | Re-read `security.allowlist_path` from disk and apply immediately |
| `trace_start` | `[dir]` | Start recording each peer's accepted `MsgClientInput` to per-peer FLIT traces (`[trace] input_trace_dir` if `dir` omitted, else `traces/`); replay with `bot_swarm --pattern trace:<file>` |
| `trace_stop` | — | Stop input tracing and close all open trace files |
| `pause` | — | Pause the simulation — ticks stop advancing; network connections remain active. In single-player the game client sends this automatically when the pause menu is opened. |
| `resume` | — | Resume the simulation at normal (1×) tick rate. |
| `shutdown` | `[--in <dur>] [--interval <dur>] [--delay <dur>] [--cancel] [--now] [--force] [--reason <text>]` | Schedule or cancel a graceful shutdown with countdown notices to connected clients; `--now` exits immediately after notifying clients; `--interval` overrides `shutdown.warning_interval_s` for this run; `--force` required when `shutdown.require_confirm = true` (default); `--reason` prepends custom operator text to each countdown broadcast (long reasons are truncated to fit in `MsgServerNotice::text[60]`; `--reason` stops consuming tokens at the next `--` flag) |
| `quit` | — | Gracefully shut down fl-server immediately without client notification |

#### Hot-reload behaviour (`reload_config`)

`reload_config` re-reads the config file and applies a subset of fields immediately:

| Field | Takes effect |
|---|---|
| `server.name` | Next LAN beacon broadcast |
| `server.motd` | Takes effect for each subsequent client connection |
| `server.motd_display_s` | Takes effect for each subsequent client connection |
| `security.banlist_path` | On next `reload_banlist` command |
| `security.allowlist_path` | On next `reload_allowlist` command |

Most `[world]` fields are also hot-reloaded on the next sim tick — `draw_distance_km`,
`snapshot_budget_bytes`, the `jitter_buffer_*` set, the `congestion_*` set, and the `overrun_*` set
(see the `reload_config` command row above).

Fields that **require a restart** to take effect: `port`, `bind_address`, `max_peers`,
`game_modes`, `password`, `discovery.*`, `mods.stack`, `rotation.*`, `world.sim_worker_threads`,
`world.max_catchup_ticks`, `world.planet_radius_m`, `world.spatial_cell_size_km`,
`world.test_spawn_ai_count`, `world.test_spawn_spread_km`, `world.test_spawn_agl_m`, `ai.*`,
`security.connect_rate_limit_*`,
`security.packet_flood_multiplier`, `security.*_bandwidth_bps`,
`security.pre_handshake_rate_limit_count`, `security.pre_handshake_window_ms`,
`security.max_connections_per_ip`, `rcon.*`.

#### Access control

**Ban list file format:** one normalized IP address per line (plain IPv4 `1.2.3.4`, bare
IPv6 `::1`, or IPv4-mapped IPv6 `::ffff:1.2.3.4` — all normalized on load). Lines beginning
with `#` are comments. File line endings are portable: both `\n` and `\r\n` are accepted.

**Ban vs allowlist precedence:** the ban list check runs first. A banned IP is rejected even
if it also appears in the allowlist.

**Pre-handshake rate limiting**: ENet CONNECT packets from any source IP that exceed
`pre_handshake_rate_limit_count` attempts within `pre_handshake_window_ms` milliseconds are
silently dropped before ENet allocates peer state. This closes the gap between the raw UDP
receive and the post-handshake rate limiter below. Full challenge-cookie anti-amplification
(withholding VERIFY_CONNECT until the client echoes a server nonce) is a future item.

**Connection rate limiting** (post-handshake): fl-server tracks how many times each IP
address completes an ENet connection handshake within a sliding time window. Peers that
exceed `connect_rate_limit_count` connections within `connect_rate_limit_window_s` seconds
are disconnected immediately.

**Packet flood detection:** a connected peer that sends more than
`packet_flood_multiplier × 60` `MsgClientInput` (unreliable, channel 1) packets per second
is disconnected. Only `MsgClientInput` (client→server control input) packets count toward
this limit; flood counting runs before the application-level seqNum staleness guard.

**ENet bandwidth caps** (`incoming_bandwidth_bps` / `outgoing_bandwidth_bps`): set aggregate
host-level byte-rate limits enforced by ENet. These cap total traffic across all peers, not
per-peer. `0` = unlimited.

### TCP RCON (Source Engine protocol)

When `[rcon] enabled = true` and a `password` is configured, fl-server binds a TCP port and
accepts connections from any Source Engine RCON client.

- **Same command set** as the stdin console — all commands in the table above are available.
- **Authentication:** the client sends a `SERVERDATA_AUTH` packet with the password. Wrong
  password → the server responds with `id = -1` and closes the connection.
- **Response splitting:** responses longer than 4086 bytes are split across multiple
  `SERVERDATA_RESPONSE_VALUE` packets (same request id), followed by an empty sentinel packet.
- **Async commands:** mutation commands (`kick`, `ban`, `unban`, `admin_unlock`, `spawn`, `kill`, `tp`) and the
  per-peer detail from `peers` return a synchronous acknowledgment string immediately. The actual
  action executes on the next sim tick (~16 ms later); a second `SERVERDATA_RESPONSE_VALUE` packet
  delivers the async confirmation to the RCON client (~20 ms after the initial response, in addition
  to fl-server stdout).
- **Connection limit:** maximum 4 simultaneous RCON clients.

> **Security:** passwords travel over **plain TCP** — no TLS. Use RCON only on trusted/VPN
> networks or via a TLS-terminating reverse proxy.

Example using `mcrcon`:

    mcrcon -H <host> -P 27015 -p <password> "status"
    mcrcon -H <host> -P 27015 -p <password> "kick 42"

---

## Environment variables

| Variable | Default | Maps to |
|---|---|---|
| `FL_CONFIG` | `./server.toml` | Config file path |
| `FL_PORT` | `4778` | `server.port` |
| `FL_BIND_ADDRESS` | `"0.0.0.0"` | `server.bind_address` |
| `FL_MAX_PEERS` | `32` | `server.max_peers` |
| `FL_NAME` | `"Unnamed Server"` | `server.name` |
| `FL_PERSISTENT` | `"false"` | `--persistent` flag |
| `FL_LOBBY_REGISTER` | `"false"` | `lobby.register` |
| `FL_LOBBY_URL` | `"https://lobby.fighters-legacy.org"` | `lobby.url` |
| `FL_LOBBY_VISIBILITY` | `"public"` | `lobby.visibility` |
| `FL_AI_DIFFICULTY_FLOOR` | `"recruit"` | `ai.difficulty_floor` |

**Not available as env vars:** `server.game_modes`, `mods.stack`, `rotation.items` —
arrays are awkward in environment strings; use a mounted config file in container
environments. `server.password` is also config-file-only; see the security note in the
[password](#password) section.

Boolean env vars (`FL_PERSISTENT`, `FL_LOBBY_REGISTER`) accept `"true"` or `"1"`.

---

## Planned configuration (128+ multiplayer re-target)

> **Forward-looking — not yet implemented.** The 128+ multiplayer re-target (decision record
> 2026-06-28) adds the config sections below. Field names and ranges are indicative and will be
> finalized as each epic lands; this section exists so operators can anticipate the surface.
> Until then these keys are ignored.

- **`[identity]`** *(Epic C)* — server-side player identity. Expected keys: `provider`
  (`guest` | `standalone` | `oidc` | `platform`), `issuer_public_key` (path/PEM for offline
  token verification), `issuer_url` (OIDC discovery), `allow_guests` (bool). Self-hostable; no
  first-party hosted issuer.
- **`[persistence]`** *(Epic H)* — storage backend for accounts/stats/bans/world state.
  Expected keys: `backend` (`sqlite` | `postgres`), `dsn`/`path`, `migrate_on_start` (bool).
  File banlists import into the store.
- **`[metrics]`** *(Epic G)* — observability. Expected keys: `enabled` (bool), `bind`/`port`
  (Prometheus/OpenMetrics scrape endpoint on a side port), `log_format` (`text` | `json`),
  `match_log_dir` (per-match log/replay shipping for offline anti-cheat).
- **`[gamemode]` / extended `[rotation]`** *(Epic E)* — data-driven game modes (team
  deathmatch, conquest, escort), team assignment/balance, friendly-fire, scoring, and
  warmup/active/end/rotation lifecycle. Builds on the existing `[rotation]` section.
- **`[anticheat]`** *(Epic D)* — live input-validation thresholds + offline `fl-review`
  pipeline toggles.
- **`[ai.provider]` / `[ai.mcp]`** *(Epics M–P — Dynamic World & Agentic AI, decision record
  2026-07-01)* — the pluggable LLM provider and the agent-facing surface. `[ai.provider]`:
  `base_url` (any OpenAI-compatible endpoint; local Ollama / llama.cpp reference), `model`,
  `api_key_env` (key read from the environment, never the TOML). `[ai.mcp]`: `enabled`
  (default false), `bind`/`port`, `autonomy` (`observe` | `recommend` | `act`), `allowlist`
  (admin command names agents may invoke). Absent/unset = fully scripted behaviour — AI
  features degrade gracefully with no provider. **Namespaced under `[ai.*]` deliberately:
  distinct from the existing `[ai]` difficulty-policy section above.** See
  [docs/ai-architecture.md](ai-architecture.md).

Clustered deployments configure these through the `fl-operator` CRDs rather than hand-edited
TOML; see [docs/distribution.md](distribution.md#server-distribution--self-hosting).

## See also

- [docs/network-protocol.md](network-protocol.md) — wire format specification for all
  `fl-server` ↔ client messages; includes bandwidth tables and interest-management
  guidance for deployments with more than ~20 simultaneous players per zone.

---

## Kubernetes / container deployment

- Pass all single-value config via environment variables; no volume mount required for
  basic deployments.
- For arrays (`mods.stack`, `game_modes`, `rotation.items`) or passwords, mount a
  pre-baked `server.toml` via ConfigMap. The first-run write is skipped when the file
  already exists.
- All output goes to stdout; compatible with Fluentd, Loki, and similar log aggregators.
- Responds to `SIGTERM` (sent by Kubernetes on pod termination) with a 100 ms graceful
  peer disconnect before exit — well within the default `terminationGracePeriodSeconds`
  of 30 s.
- Example minimal deployment with env vars:

  ```yaml
  env:
    - name: FL_NAME
      value: "My Server"
    - name: FL_PORT
      value: "4778"
    - name: FL_MAX_PEERS
      value: "32"
  ```
