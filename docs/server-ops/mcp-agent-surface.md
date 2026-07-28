# Agent surface (MCP)

Exposes the server to AI agents over the [Model Context Protocol](https://modelcontextprotocol.io),
so an agent can watch a match, reason about it, and — if you let it — act on it.

**Off by default, and read-only when you first turn it on.** This page is about the security model
as much as the plumbing, because the interesting question is not how to enable it but what an agent
can do once you have.

## What it is

A second frontend on the [admin API](admin-api.md) listener. Same port, same token table, same
per-IP lockout. It speaks MCP revision **`2025-06-18`** over Streamable HTTP.

```toml
[http_admin]          # required — the MCP surface rides this listener
enabled      = true
port         = 8080
bind_address = "127.0.0.1"

[[http_admin.tokens]]
token    = "an-agent-token"
role     = "moderator"
autonomy = "observe"

[ai.mcp]
enabled            = true
path               = "/mcp"
autonomy           = "observe"
allowlist          = []
rate_limit_per_min = 120
max_sessions       = 8
```

Enabling `[ai.mcp]` without `[http_admin]` is **refused at startup**: there would be no listener to
attach to and no token table to authenticate against, and starting anyway would leave you believing
you had an agent surface that does not exist.

Point a client at `http://<host>:<port>/mcp` with the token as a bearer credential.

## The security model

Three independent things must all permit an action. None of them substitutes for the others, and
the order matters.

### 1. The token authenticates

Same `[[http_admin.tokens]]` table as the REST API. An unknown token is rejected before anything
else happens, and repeated failures trip the shared per-IP lockout.

**The MCP session is bound to the token that opened it.** A session id cannot be replayed against a
different credential.

### 2. The autonomy tier is a ceiling

Each token carries a tier, either its own `autonomy` field or the `[ai.mcp] autonomy` default:

| Tier | An agent may |
|---|---|
| `observe` | Read world state and events. Nothing else. **The default.** |
| `recommend` | Also validate proposals — e.g. check a mission YAML — without applying anything |
| `act` | Also run admin commands, subject to everything in §3 |

A tier is a **ceiling, not a grant**. Raising a token to `act` does not give it any capability it
did not already have; it only stops the tier from being the thing that refuses. An unknown tier
name stops the server starting.

### 3. Capabilities and the allowlist both still apply

For `admin_command`, two further checks run server-side:

- The command must appear in **`[ai.mcp] allowlist`**. It is empty by default, and **empty permits
  nothing** — an `act`-tier token with no allowlist can run no commands at all. This is the knob to
  reach for first: list the specific commands you are comfortable with.
- The command must be permitted by the **token's capabilities**, exactly as if an operator with
  that role had typed it.

Both, not either. An `act`-tier `moderator` token whose allowlist includes `shutdown` is still
refused `shutdown`, because a moderator has no shutdown capability. There is a test that provokes
precisely that case with tier and allowlist both permitting.

This is the property worth internalising: **every agent action converges on the same
permission-checked command path the console uses.** MCP adds a frontend, not a second set of rules.

### And it is all recorded

Every tool invocation goes into the match event log, so it appears in `GET /events` and in the
match's `.flrep` recording. An agent's actions are as auditable as an operator's — after the fact,
you can replay the match and see what it did and when.

## Tools

| Tool | Minimum tier | What it does |
|---|---|---|
| `world_state` | `observe` | The authoritative world snapshot: entities, peers, factions and relationships, alert levels, mission state, weather. Rebuilt about once a second. |
| `events` | `observe` | Tail of the match event log. `after` resumes from a sequence number; `gap` in the result means records were dropped before those returned. |
| `submit_mission` | `recommend` | Validates a mission YAML against the engine's own schema and reports every error and warning. **It does not load the mission** — validation only. |
| `admin_command` | `act` | Runs one admin command, subject to the allowlist and the token's capabilities. |

`world_state`'s output schema is the same golden JSON schema the REST `/worldstate` route is tested
against, so the two cannot drift into describing the same document differently.

## Resources

| URI | Contents |
|---|---|
| `fl://world_state` | The world snapshot, subscribable |
| `fl://events` | The match event log tail, subscribable |

Both support subscription: the server notifies on update rather than making an agent poll.

## Limits

- **`rate_limit_per_min`** — calls per minute **per token** (default 120). `0` disables the
  limiter, which is rarely what you want on a public server.
- **`max_sessions`** — concurrent MCP sessions (default 8).
- **JSON-RPC batching is not supported.** Revision `2025-06-18` removed it, so a batch is an
  explicit error rather than an unimplemented feature:

  ```json
  {"jsonrpc": "2.0", "id": null,
   "error": {"code": -32600,
             "message": "JSON-RPC batching is not supported in MCP revision 2025-06-18"}}
  ```

## Connecting

```bash
curl -X POST http://127.0.0.1:8080/mcp \
  -H 'Authorization: Bearer an-agent-token' \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize",
       "params":{"protocolVersion":"2025-06-18","capabilities":{},
                 "clientInfo":{"name":"my-agent","version":"1"}}}'
```

```json
{"jsonrpc": "2.0", "id": 1, "result": {
  "protocolVersion": "2025-06-18",
  "capabilities": {"tools": {"listChanged": false},
                   "resources": {"subscribe": true, "listChanged": false}},
  "serverInfo": {"name": "fighters-legacy/fl-server", "version": "0.3.13"}}}
```

Carry the returned `Mcp-Session-Id` header on subsequent calls. Calling `tools/list` without it is
refused:

```json
{"error": {"code": -32600,
           "message": "no MCP session for this token; call initialize first (Mcp-Session-Id header)"}}
```

## Running one safely

1. **Leave `bind_address` at `127.0.0.1`** and reach it through a reverse proxy with TLS. The
   surface speaks plain HTTP; a bearer token sent in clear is a token you have published.
2. **Start at `observe`.** Watch what the agent asks for over a real match before granting more.
3. **Grant `act` by allowlist, not by tier.** Name the specific commands. `[]` — the default — is a
   working configuration, not a placeholder.
4. **Give the agent its own token** with the narrowest role that works, not an `admin` one. The
   capability check is the backstop that survives a prompt injection; a token scoped to what the
   agent actually needs is what makes that backstop meaningful.
5. **Read `GET /events` afterwards.** Every invocation is there.

The model is never the security boundary. Treat everything an agent sends as untrusted input that
happens to arrive over an authenticated channel, and let the allowlist and the capability mask do
the deciding.

## See also

- [Admin API](admin-api.md) — the REST frontend on the same listener
- [Server configuration](server-config.md) — `[ai.mcp]` and `[http_admin]` key reference
- [AI architecture](../developer/ai-architecture.md) — why the surface is shaped this way
