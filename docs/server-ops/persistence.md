# Persistence

The server keeps its durable state — bans, accounts, career statistics and campaign saves — in a
SQL database it owns. Before this existed, the only things that survived a restart were a flat
`banlist.txt` rewritten in full on every ban, and a campaign save file.

**It is on by default, and a server that cannot open its store will not start.** That is deliberate:
a server which runs perfectly while quietly persisting nothing is one whose operator finds out weeks
later, when a ban they set does not survive a restart.

```toml
[persistence]
enabled = true
backend = "sqlite"                 # sqlite | postgres
sqlite_path = "cache/fl-server.db"
postgres_dsn = ""                  # libpq connection string; needs a -DFL_WITH_POSTGRES=ON build
busy_timeout_ms = 5000
write_queue_max = 4096
```

Every key is **Restart** class — the connections, the schema and the writer thread are all
established at startup. `reload_config` reports a change here rather than applying it. Full key
reference: [server configuration](server-config.md#persistence--the-durable-store-533).

## Upgrading an existing server

Two things happen on the first start after the upgrade, both once, both logged.

**The store is created.** `cache/fl-server.db` appears relative to the server's working directory,
so that directory must be writable. A deployment whose `cache/` is read-only booted before this
release and will now refuse to start, naming the path.

**Existing files are imported.** A `banlist.txt`, an `allowlist.txt` and a
`cache/campaign_<name>.flsave` are each read once into the store and then **left on disk**. They are
your records; deleting them buys nothing, and a downgrade to an older server should still find them.

From that point the **store wins** on every subsequent load. This matters more than it sounds: the
files stay frozen at the moment of the upgrade while the store keeps changing, so a server that
preferred the file would silently undo every ban and unban made since — and it would look like the
server had simply forgotten.

> **Take a backup before upgrading.** Migrations are forward-only (see below), so the recovery path
> from a bad upgrade is a backup, not a downgrade.

## What is stored, and what is not yet

| Data | Status |
|---|---|
| **Bans and the allowlist** | Live. Written by `ban` / `unban`, with the issuing admin recorded. |
| **Campaign saves** | Live. Replaces `cache/campaign_<name>.flsave`. |
| **Accounts** | Schema and repository exist; nothing creates accounts until identity ships. |
| **Career statistics** | Schema and repository exist; nothing writes them until server-authoritative stats ship, which needs verified identity to key on. |

The account and statistics tables are present so that the schema does not have to change under a
live deployment when those features arrive. An empty `accounts` table on a busy server is expected,
not a fault.

## Backends

### SQLite — the default

Needs no external service. It is **compiled into the server** rather than taken from the system, so
every platform runs one pinned version and the database behaves identically on your machine, in CI
and on an operator's build.

It runs in WAL mode with `synchronous = NORMAL`:

- **A server crash — including `kill -9` — keeps everything the writer has applied**, which in
  practice is everything but the last fraction of a second (see below for what "written" means).
- A host crash or power loss can lose the last moments of applied writes, but does not corrupt the
  database.

If the server cannot switch the database to WAL — which happens when several servers open the same
file at the same instant — it logs a warning and carries on in the journal mode it has. The store
is fully functional either way, only less concurrent.

### PostgreSQL — for several servers sharing one store

A **build-time** option. A stock server does not contain it, and asking for `backend = "postgres"`
on such a build fails at startup saying so, rather than pretending the database is unreachable.

```
cmake -S . -B build -DFL_WITH_POSTGRES=ON
```

`postgres_dsn` carries a password, so keep `server.toml` readable only by the server's user. The
value is **redacted** wherever the server prints it — startup log, `reload_config` output, admin
surfaces — but it is in the file in clear.

## Durability, and what "written" means

Writes are applied on a dedicated thread so the simulation never blocks on the database. A command
returns once its write is **queued**; it is applied moments later, and definitely by the time the
server finishes shutting down, which flushes before closing.

So there is a brief window — sub-second on healthy storage — in which a `kill -9` can lose a write
whose command has already answered. A clean shutdown has no such window. If you are about to pull
the plug on a server and the last ban matters, stop it rather than killing it.

Two more consequences worth knowing:

- **A write is never silently dropped.** If the queue reaches `write_queue_max` the caller waits for
  it to drain rather than discarding work, and logs once naming the key to raise.
- **A failed write is logged at `Error` and counted.** It is never swallowed.

> **There is no admin command or metric for store health yet.** The counters exist internally —
> writes queued, completed, failed, and the worst queue depth seen — but nothing surfaces them. For
> now the log is the signal: grep for `persistence:` at `Error`. Exposing them belongs with the
> metrics exporter work.

## Schema and migrations

The schema is versioned and migrations are **forward-only**. On start the server applies any
migration newer than the database's version, each in its own transaction.

Two behaviours are deliberate:

- **Migrations are safe when several servers start at once.** Each runs inside an exclusive
  transaction and re-reads the applied version inside it, so a migration another process already
  applied is skipped rather than colliding. Two servers starting together against one fresh database
  is ordinary — a restart, a test suite, a second server in the same directory.
- **An older server refuses to open a newer database**, naming both versions. Carrying on would have
  it write rows against a shape it is guessing at.

There is no down-migration and there will not be one: it would be code that runs once, in an
emergency, on data nobody has a copy of, having never been exercised.

## Backups

Stop the server, or use a tool that understands the format — copying a live SQLite database with
`cp` can capture a torn state.

```bash
# SQLite, server stopped -- take the -wal and -shm files too, not just the .db
cp cache/fl-server.db cache/fl-server.db-wal cache/fl-server.db-shm backup/

# SQLite, server running -- .backup is safe against a live database; cp is not
sqlite3 cache/fl-server.db ".backup 'backup/fl-server.db'"

# PostgreSQL
pg_dump --format=custom --file=backup/fl.dump "$FL_DSN"
```

## Managing bans

Bans are managed with the admin commands, not by editing a file:

| Command | Effect |
|---|---|
| `ban <peerId\|IP>` | Bans the address, kicks matching peers, and records the rule with the issuing admin's name |
| `unban <IP>` | Removes the rule |
| `reload_banlist` | Re-applies active ban rules from the store |
| `reload_allowlist` | Re-applies active allowlist rules from the store |

`reload_banlist` is how a running server picks up a change made elsewhere — by another server
sharing a PostgreSQL store, or by an operator working directly in the database. The file-based
`security.banlist_path` and `security.allowlist_path` are **deprecated to import-only** and warn at
startup when set.

A ban rule records who issued it and when. Attribution is coarse today — `console` for stdin, RCON
and the single-player admin token, `peer N` for an in-game admin — and becomes a real account
identity when identity ships.

Rules can carry an expiry, and one that has lapsed is not applied. **Nothing sets a duration yet**:
the running server holds its ban list in memory and rebuilds it only on load, so a temporary ban
would not actually lift until a restart. A `--for` option ships together with the refresh that makes
it true, rather than before it.

## Running without a store

`[persistence] enabled = false` is a supported configuration for a throwaway or test server. The
server starts, logs plainly that bans, accounts and statistics will not survive a restart, and the
`ban` command says a ban will not outlive the process. Campaign saves fall back to
`cache/campaign_<name>.flsave` exactly as before — turning persistence off must not cost you a
campaign.

## Troubleshooting

**`persistence: cannot open the … store: <path>: attempt to write a readonly database`**
The server's working directory or the database file is not writable by the server's user. Fix the
permission, point `sqlite_path` somewhere writable, or set `enabled = false` deliberately.

**`… it was migrated by a NEWER fl-server`**
The database was upgraded by a newer build. Run that build, or restore the backup taken before the
upgrade. This server will not touch it.

**`persistence: write queue full at N`**
Writes are arriving faster than the disk accepts them, and callers are being made to wait. Almost
always a sign the underlying storage is struggling; raise `write_queue_max` only if it is a genuine
burst.

**`security.banlist_path is DEPRECATED …`**
Expected if you still have the key set. The file was imported once and is no longer read. Remove
the key when you are satisfied the store has your bans — `reload_banlist` will tell you how many are
in force.

**Bans do not survive a restart**
Check the startup log for `persistence: disabled` — the store is off. Otherwise check for
`persistence:` lines at `Error`.
