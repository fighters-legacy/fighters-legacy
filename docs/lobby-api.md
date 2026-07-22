<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# fl-lobby REST API (v1)

This is the wire contract between a Fighters Legacy dedicated server (the **registration client**,
`engine/net/LobbyRegistration`), the in-game **server browser** (`engine/net/LobbyListClient`), and an
**fl-lobby** service. Hosting is self-host only: fl-lobby is a small standalone service (planned as a Go
companion repo, issue #999 / #36) that anyone can run. This document is the source of truth it is written
from — the C++ side in this repo already speaks it.

All bodies are JSON (`Content-Type: application/json`). All endpoints are versioned under `/v1`. There is
**no authentication in v1**: the lobby trusts the network it is exposed on, keys entries on the request's
**source IP + advertised port**, and caps its state (see Limits). NAT traversal is out of scope for v1 —
a server behind NAT that cannot be reached at its source IP simply will not be joinable.

## Endpoints

### `POST /v1/servers` — register / heartbeat

A dedicated server POSTs this on an interval (`[lobby]`-configured, default 30 s) while it wants to be
listed. The lobby upserts an entry keyed by `(source IP, port)` and refreshes its TTL. The server never
sends its own host — the lobby uses the request source IP.

Request body:

    {
      "name": "My Server",          // display name
      "port": 4778,                 // the GAME port a client connects to
      "players": 3,                 // current player count
      "max_players": 16,            // capacity
      "mode": "builtin:tdm",        // game-mode id
      "mission": "fjord",           // current mission/map (may be empty)
      "visibility": "public"        // always "public"; a private server never POSTs
    }

Response: `200 OK` (or `201 Created` on first registration). The body is ignored by the client.

### `DELETE /v1/servers` — deregister

Sent best-effort on server shutdown to drop the entry immediately (rather than waiting for the TTL). The
lobby matches the entry by `(source IP, port)`.

Request body:

    { "port": 4778 }

Response: `200 OK` or `204 No Content`. A missing entry is not an error.

### `GET /v1/servers` — list

The in-game browser GETs this to list public servers. The response is a JSON **array** of server objects:

    [
      {
        "name": "My Server",
        "host": "203.0.113.7",      // the address a client connects to (the lobby fills this from the source IP)
        "port": 4778,
        "mode": "builtin:tdm",
        "mission": "fjord",
        "players": 3,
        "max_players": 16,
        "passworded": false
      }
    ]

The client parser is deliberately tolerant: it ignores unknown keys, accepts `address` as an alias for
`host`, drops any object with no host or a zero port, and bounds both the row count and per-string length
(see Limits). A server whose entry has gone stale should simply be absent from the list.

## TTL and freshness

An entry is considered live for **2.5 × the server's heartbeat interval** after its last `POST`. The
default heartbeat is 30 s, so a default entry expires ~75 s after the server stops heartbeating (e.g. a
crash with no `DELETE`). The lobby prunes expired entries lazily and never returns them from `GET`.

## Limits (denial-of-service posture)

- At most **1024** entries returned from `GET /v1/servers` (the client also caps at 1024 rows).
- At most **1 MiB** response body for `GET` (the client caps its accumulated body at 1 MiB).
- Per-string fields truncated to **256 bytes** client-side; the lobby should bound them similarly.
- The lobby should rate-limit `POST`/`DELETE` per source IP and cap total entries.

## Federation

The client's lobby list is configured via `[client] lobby_urls` (comma-separated, **empty by default**):
the player opts in to a lobby. LAN discovery works with no lobby at all. There is no central registry — a
community runs its own fl-lobby and shares its URL. Multiple lobby URLs are merged in the browser, and LAN
entries win over lobby entries for the same `host:port`.

## Versioning

The path is versioned (`/v1`). A breaking change ships as `/v2`; the client advertises which versions it
speaks by the path it requests. v1 is frozen once fl-lobby ships.
