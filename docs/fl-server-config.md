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

[world]
save_path          = "world.sav"
autosave_interval_s = 300
time_scale         = 10.0        # game seconds per real second; 10 = full day/night ≈ 2.4 real hours
# planet_radius_m         = 6371000  # planet sphere radius (m); Earth default
# draw_distance_km        = 200.0    # per-peer interest management radius (km); [1, 100000]
# baseline_interval_ticks = 120      # full-snapshot baseline interval for delta recovery; [1, 3600]

[ai]
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

[spawn]
agl_offset = 500.0  # metres AGL above terrain for all spawn points

# [[spawn.points]]
# x = 0.0
# z = 0.0
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
| integer | `32` | 1–128 |

Maximum number of simultaneous connected peers. Values outside `[1, 128]` are rejected
with a warning and the default is used instead.

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

> **Phase 2:** Rotation logic lands with the game server runtime. These keys are parsed
> and stored; no automatic cycling occurs yet.

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

### `draw_distance_km`

| Type | Default | Range |
|---|---|---|
| float | `200.0` | `[1, 100000]` |

Per-peer interest management radius in kilometres. Only entities within this XZ-plane radius of a peer's own entity are included in that peer's `MsgWorldSnapshot`. The default of 200 km covers any current Phase 2 theater. Out-of-range values are rejected with a Warn and the default is used. **Hot-reloadable** via `reload_config`.

### `baseline_interval_ticks`

| Type | Default | Range |
|---|---|---|
| integer | `120` | `[1, 3600]` |

Interval in sim ticks between full-snapshot baselines. On baseline ticks all visible entities receive a full `MsgEntityEntry` regardless of known-entity state, providing UDP packet-loss recovery. At 60 Hz: `120` = 2 s recovery window. Smaller values reduce recovery time but increase bandwidth. Out-of-range values are rejected with a Warn and the default is used. **Hot-reloadable** via `reload_config`.

---

## [ai] — AI policy

> **Phase 2:** `difficulty_floor` is parsed and stored; server-side difficulty enforcement
> lands with the AI runtime.

### `difficulty_floor`

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

## [security] — Access control and rate limiting

### `connect_rate_limit_count`

| Type | Default | Valid range |
|---|---|---|
| integer | `5` | 1–100 |

Maximum number of times a single IP address may complete an ENet connection handshake within
`connect_rate_limit_window_s` seconds. Peers that exceed this count are immediately disconnected.
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
| integer | `0` (unlimited) | 0–128 |

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
- `admin_auth_status` returns per-IP lockout and failure detail as a single synchronous
  response packet (no second packet), unlike `peers`. Output is split into a
  `MsgAdminCommand channel:` section and, when RCON is enabled, an `RCON channel:` section.
  The sync ack format is `"admin: N lockout(s) | rcon: M lockout(s)"` when RCON is enabled,
  or `"N lockout(s) active"` when RCON is disabled.

### Example: connect with mcrcon

    mcrcon -H <host> -P 27015 -p <password> "status"

All `[rcon]` fields **require a restart** to take effect.

---

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
| `status` | — | Show uptime, peer count, entity count, tick rate |
| `peers` | — | List connected peers (peer ID, address, entity index/generation, one-way delay in ticks/ms) |
| `kick` | `<peerId\|IP>` | Disconnect a peer by numeric ID, or all peers from an IP address |
| `ban` | `<peerId\|IP>` | Add IP to the ban list and kick matching peers; saves to `banlist_path` if configured |
| `unban` | `<IP>` | Remove an IP from the ban list; saves to `banlist_path` if configured |
| `admin_unlock` | `<IP>` | Clear the admin auth and RCON auth lockouts for an IP address immediately; prints a warning if neither channel was locked (idempotent) |
| `admin_auth_status` | — | Show per-IP lockout state for the MsgAdminCommand operator channel and (when RCON is enabled) the RCON TCP channel; both active lockouts and pending failure counts |
| `set_weather` | `<preset>` | Change weather: `clear`, `partly_cloudy`, `overcast`, `rain`, `storm` |
| `set_time` | `<0–24>` | Set in-game time of day (float, hours) |
| `spawn` | `<type> <x> <y> <z> [--ai <behavior> [args...]]` | Spawn a registered entity type at the given world position; optionally attach an AI controller. Behaviors: `loiter [cx cy cz [radius_m [alt_m [throttle [cw\|ccw]]]]]`, `waypoint x y z [x y z ...] [--loop]`, `pursuit <entityIdx>`, `evade <entityIdx>`, `break <entityIdx> [rollDuration]` |
| `kill` | `<idx>` | Remove a live entity by pool index (see `peers` output) |
| `tp` | `<idx> <x> <y> <z>` | Teleport entity `<idx>` to world position; also used by the game client's game console to teleport the player entity |
| `reload_config` | — | Re-read `server.toml` and apply: `name` (beacon), `motd`, `motd_display_s`, `draw_distance_km`, `baseline_interval_ticks` (takes effect immediately for connected peers) |
| `reload_banlist` | — | Re-read `security.banlist_path` from disk and apply immediately |
| `reload_allowlist` | — | Re-read `security.allowlist_path` from disk and apply immediately |
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

Fields that **require a restart** to take effect: `port`, `bind_address`, `max_peers`,
`game_modes`, `password`, `discovery.*`, `mods.stack`, `rotation.*`, `world.*`, `ai.*`,
`security.connect_rate_limit_*`, `security.packet_flood_multiplier`, `security.*_bandwidth_bps`,
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
