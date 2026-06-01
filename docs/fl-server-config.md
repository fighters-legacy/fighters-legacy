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
| 2 | CLI positional args | `fl-server 9000 32` |
| 3 (highest) | Environment variables | `FL_PORT=9000` |

All six TOML sections are tier-1 only (env vars and CLI do not cover arrays or
multi-key sections). See [Environment variables](#environment-variables) for the full
`FL_*` list.

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
motd         = ""
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

[ai]
difficulty_floor = "recruit"
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

Message shown to connecting clients. Empty string disables the message.

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

> **Phase 2:** Mod stack is parsed and logged at startup; `ModLoader` integration lands
> with the content system.

### `stack`

| Type | Default |
|---|---|
| array of strings | `[]` |

Ordered list of mod IDs to load. Index 0 is the highest-priority mod; later entries are
lower priority. IDs must match the `[mod].id` field in each mod's `manifest.toml`. See
[docs/architecture.md](architecture.md) for the mod manifest format.

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
