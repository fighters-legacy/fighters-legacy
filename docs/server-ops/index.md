# Server Ops Guide

For anyone running a dedicated `fl-server`.

A server needs no configuration file to start — it writes a commented default `server.toml` on
first run and listens on port 4778. Everything below is about changing that default sensibly.

| Page | What it covers |
|---|---|
| [Server configuration](server-config.md) | Every `server.toml` key with its type, default and range; every CLI flag; the full admin command reference; and which keys hot-reload |
| [Admin API](admin-api.md) | The REST endpoints, bearer-token auth, the unauthenticated health probe, and what each route returns |
| [Agent surface (MCP)](mcp-agent-surface.md) | Exposing the server to AI agents over MCP, the autonomy tiers, and the security model that bounds them |
| [Persistence](persistence.md) | The durable store behind bans, accounts, statistics and campaign saves: upgrading an existing server, the two backends, durability, migrations and backups |
| [Metrics](metrics.md) | The `--metrics-json` report: every field, what a bad value looks like, and what to do about it |
| [Lobby API](lobby-api.md) | Registering with a lobby so players can find the server |

## Before you expose a server

Three settings decide whether a public server is safe, and all three are off or closed by default.
If you turn any of them on, read the page that owns it first:

- **`[http_admin]`** — the REST admin API can kick, ban and shut the server down. It refuses to
  start with no tokens, binds to localhost by default, and should stay behind a reverse proxy.
- **`[ai.mcp]`** — the agent surface rides the same listener and the same token table. Its default
  autonomy tier is read-only; raising it grants an agent the ability to act.
- **`[rcon]`** — the TCP admin channel. Off by default, and it warns at startup if enabled with an
  empty password.

Per-IP lockout guards all three the same way; see
[Server configuration](server-config.md) for the thresholds.
