# Admin API

An optional HTTP interface for administering a running server: health probes for an orchestrator,
read routes for a dashboard, and a small set of mutating routes for kick/ban/shutdown.

It is **off by default**, and it refuses to start unauthenticated. See
[`[http_admin]`](server-config.md) for the configuration keys; this page is the endpoint reference.

```toml
[http_admin]
enabled      = true
port         = 8080
bind_address = "127.0.0.1"

[[http_admin.tokens]]
token = "a-long-random-secret"
role  = "admin"
```

Enabling it with **no tokens is refused at startup**, not warned about. This endpoint can kick, ban
and shut the server down; a warning in a startup log is not a control.

## Authentication

Every route except `/health` requires a bearer token from the `[[http_admin.tokens]]` table:

```
Authorization: Bearer a-long-random-secret
```

A missing or unknown token gets **401** with `{"error": "unauthorized"}`. Repeated failures trip the
same per-IP lockout the RCON and in-game admin channels use, so the API cannot be used to
brute-force a token faster than the other two.

A token's `role` names a capability preset, and the capabilities are what actually decide whether a
request is allowed — **the transport grants nothing**. A `moderator` token calling a route that
needs `shutdown` capability gets **403**, not a shutdown. An unknown role name stops the server
starting rather than silently granting nothing.

`bind_address` defaults to `127.0.0.1`. Set it to `0.0.0.0` only behind a reverse proxy that
terminates TLS — the API speaks plain HTTP, and a bearer token on the open internet without TLS is
a token you have given away.

## Endpoints

| Method | Route | Auth | Returns |
|---|---|---|---|
| `GET` | `/health` | none | Liveness. `{"status": "ok", "uptime": <seconds>}` |
| `GET` | `/status` | token | One-line server summary: uptime, peers, entities, tick rate, governor load |
| `GET` | `/peers` | token | Connected peers with delay, queue depth, send rate and packet loss |
| `GET` | `/worldstate` | token | The ~1 Hz world snapshot as JSON |
| `GET` | `/events?after=N&max=M` | token | The match event stream |
| `POST` | `/kick` | token | `{"peer": <id>}` |
| `POST` | `/ban` | token | `{"ip": "<address>"}` |
| `POST` | `/unban` | token | `{"ip": "<address>"}` |
| `POST` | `/shutdown` | token | `{"in": <seconds>}` (optional) |

### `GET /health`

The only unauthenticated route, and the only one that **answers while the simulation is stalled** —
it is served from the accept thread, so a server whose tick has locked up still reports on this
route. That is deliberate: a liveness probe that hangs when the thing it probes hangs cannot
distinguish "wedged" from "unreachable", and an orchestrator needs to tell those apart.

```json
{"status": "ok", "uptime": 18}
```

Use it for a container liveness probe. Do not use it for readiness in a way that assumes the sim is
healthy — check `/status`'s tick rate for that.

### `GET /status`

```json
{"result": "uptime: 137s  peers: 3  entities: 41  tick: 59.9 Hz (0.31/0.88 ms mean/p99)  load: 100%  interest: 100%"}
```

`load` is the tick-overrun governor's load factor and `interest` its interest-radius scale; both at
100 % mean the server is not shedding work. A `[DEGRADED]` marker appears when it is.

!!! warning "Uptime is wrong here"
    `/status` currently reports the **host's** uptime rather than the server's, while `/health`
    reports the server's correctly. Tracked as
    [#1048](https://github.com/fighters-legacy/fighters-legacy/issues/1048); trust `/health` until
    it is fixed.

### `GET /peers`

Per-peer diagnostics: peer id, address, entity index, one-way delay in ticks and milliseconds,
input-queue fill against its adaptive maximum, the adaptive snapshot send rate, and ENet packet
loss. This is the route for answering "is it them or is it us" — a peer with high loss and a
reduced send rate is a link problem; every peer decimated at once is a server problem, and
[metrics](metrics.md) will show the governor engaged.

### `GET /worldstate`

The aggregated world snapshot: entities, the faction table with alert levels and the relationship
matrix, peers, mission and objective state, weather and wind. Rebuilt about once a second and read
from a published off-thread copy, so requesting it never blocks the simulation.

Empty for the first second of a server's life, before the first snapshot is published.

### `GET /events`

The append-only match event log — kills with attribution and weapon class, spawns, damage
transitions, joins and leaves, chat, admin commands and alert-level changes.

```json
{"next_seq": 812, "gap": false, "count": 2, "events": [ ... ]}
```

Pass `after=<next_seq from your last call>` to resume, and `max=<n>` to bound the page. **`gap:
true` means records you had not read were dropped** before the ones returned — the log is a bounded
ring, so a consumer that stops reading loses history rather than growing the server's memory. Treat
a gap as a signal to widen your polling interval or accept the loss explicitly; it is not an error.

With no `after`, you get the recent tail.

### `POST /kick`, `/ban`, `/unban`

```bash
curl -X POST http://127.0.0.1:8080/ban \
  -H 'Authorization: Bearer a-long-random-secret' \
  -H 'Content-Type: application/json' \
  -d '{"ip": "203.0.113.7"}'
```

`/kick` takes `{"peer": <id>}`; `/ban` and `/unban` take `{"ip": "<address>"}`. Addresses are
normalised server-side (IPv6 brackets and `::ffff:` prefixes are stripped), so pass whatever
`/peers` reported. When `security.banlist_path` is configured, bans persist to it.

A malformed body gets **400** with the expected shape in the error message.

### `POST /shutdown`

```json
{"in": 300}
```

Schedules a graceful shutdown with countdown notices to connected players. Omit `in` for the
configured default delay. `shutdown.require_confirm` does not apply here — presenting a valid admin
token *is* the confirmation.

## How mutations are applied

Every mutating route enqueues its work onto the simulation thread and answers immediately with an
acknowledgement, rather than blocking the HTTP thread until the next tick. So a **200 means the
command was accepted and queued**, not that it has already taken effect. The effect lands on the
next tick — within about 17 ms at a healthy tick rate.

Under the hood each route builds the same text command an operator would type and dispatches it
through the one permission-checked command path the console, RCON, the in-game admin channel and
the MCP surface all use. There is no separate REST permission logic to get out of step: a
capability that refuses a command in the console refuses it here too.

## Status codes

| Code | Meaning |
|---|---|
| `200` | Accepted (for mutations: queued, see above) |
| `400` | Malformed body — the message names the expected shape |
| `401` | Missing or unknown bearer token |
| `403` | Token authenticated, but its capabilities do not permit this |
| `429` | Rate limited |
| `503` | The simulation is not available to service the request |

## See also

- [Server configuration](server-config.md) — the `[http_admin]` keys and the full admin command set
- [Agent surface (MCP)](mcp-agent-surface.md) — the agent frontend on this same listener
- [Metrics](metrics.md) — machine-readable server health
