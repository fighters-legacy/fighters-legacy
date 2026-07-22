# Game Mode Authoring Guide

This guide covers the game-mode TOML schema (Epic E, #521). Game modes define the
*rules* of a multiplayer match — teams, scoring, respawn, and win conditions — on top
of the world a mission or the sandbox provides. They are aimed at content-pack authors
and server operators.

---

## Overview

A game mode is a TOML document loaded as a `GameMode` content-pack asset via
`FolderContentPack`, or one of the compiled-in builtins. The engine parses it through
`parseGameModeToml` — the same function the `validate-mode` tool uses, so the validator
and the engine cannot disagree.

Files live in `modes/` inside the content pack directory (e.g. `modes/tdm.toml`), and are
referenced by their file stem prefixed with the pack namespace, e.g. `fl-base:tdm`.

Three modes ship compiled in and need no content pack:

- **`builtin:free-flight`** — the default. No teams beyond the mission's sides, no
  score or time limit, no warmup, immediate respawn, friendly fire follows the server
  setting. This is byte-identical to the pre-Epic-E sandbox behavior.
- **`builtin:tdm`** — team deathmatch: two teams (Red/Blue), a 50-point score limit, a
  15-minute clock, a 30-second warmup, 10-second respawns, friendly fire off, minimum
  2 players.
- **`builtin:strike`** — objective-scored (#1000): two teams, a kill worth 1 and each
  completed objective worth 10 (`points_per_objective`), a 100-point score limit, a
  20-minute clock. The score comes from mission objectives, not just kills — a mission
  trigger's action calls `world.score_objective(faction, count)` (see below).

### Objective scoring

A mission awards objective points to a team from a Lua trigger action:
`world.score_objective(faction, count)` (count defaults to 1). The host routes it to the
match controller, which adds `count × points_per_objective` to that team — but only
during the **Active** phase, and only when the mode declares a non-zero
`points_per_objective`. This is how strike/conquest modes accumulate score from
objectives rather than (or alongside) kills.

---

## Selecting a mode

A dedicated server picks the mode with the `[match] mode` key in `fl-server.toml`, or
per-rotation-item with a `mission@mode` suffix in `[rotation] items`:

```toml
[match]
mode = "builtin:tdm"

[rotation]
items = ["fjord@builtin:tdm", "canyon@fl-base:conquest"]
```

An unknown mode id logs a warning and falls back to `builtin:free-flight`, so the server
always boots.

---

## Schema

All sections are optional; an omitted key takes the documented default. The only required
field is `[mode] id`.

```toml
[mode]
id = "fl-base:tdm"
name = "Team Deathmatch"

[teams]
use_mission_sides = false
[[teams.team]]
id = "red"
name = "Red Force"
capacity = 16
[[teams.team]]
id = "blue"
name = "Blue Force"
capacity = 16

[scoring]
points_per_kill = 1
points_per_assist = 0
points_per_objective = 0
score_limit = 50

[match]
time_limit_min = 15
warmup_s = 30
min_players = 2

[respawn]
delay_s = 10
waves = false
wave_interval_s = 15

[rules]
friendly_fire = "off"
```

### `[mode]`

| Field | Type | Constraints | Description |
|---|---|---|---|
| `id` | string | non-empty (required) | Mode id, e.g. `fl-base:tdm` |
| `name` | string | — | Display name; empty falls back to `id` |

### `[teams]`

| Field | Type | Constraints | Description |
|---|---|---|---|
| `use_mission_sides` | bool | default `true` | When true, teams are the loaded mission's `sides`; the `[[teams.team]]` list is ignored |
| `team.id` | string | non-empty | Team id (maps onto a FactionRegistry faction) |
| `team.name` | string | — | Display name; empty falls back to `id` |
| `team.capacity` | int | 0–4096 | Max players on the team; `0` = unlimited |

When `use_mission_sides = false` and no mission is loaded, the server synthesizes a
FactionRegistry from the team list (index 0 neutral, teams 1..N mutually hostile) — so a
zero-content-pack TDM server has real, hostile teams. When a mission *is* loaded, mode
teams alias onto the mission's sides positionally (mode team `i` → mission side `i`); a
count mismatch warns and the mission's sides win.

### `[scoring]`

| Field | Type | Constraints | Description |
|---|---|---|---|
| `points_per_kill` | int | −1000..1000 | Points a team scores per enemy kill (a team kill scores nothing) |
| `points_per_assist` | int | −1000..1000 | Parsed and stored; wired when the assist channel lands |
| `points_per_objective` | int | −100000..100000 | Points per completed objective (#1000); awarded via `world.score_objective` during Active |
| `score_limit` | int | 0–1000000 | Team score that ends the match; `0` = no score limit |

### `[match]`

| Field | Type | Constraints | Description |
|---|---|---|---|
| `time_limit_min` | float | 0–1440 | Match time limit in minutes; `0` = no time limit |
| `warmup_s` | float | 0–3600 | Warmup countdown before the match goes Active; `0` = start immediately |
| `min_players` | int | 1–4096 | Human players required before the warmup countdown runs |

### `[respawn]`

| Field | Type | Constraints | Description |
|---|---|---|---|
| `delay_s` | float | 0–3600 | Seconds from death to respawn eligibility; `0` = immediate on request |
| `waves` | bool | default `false` | Round respawns up to a wave boundary |
| `wave_interval_s` | float | 1–600 | Wave period when `waves = true` |

A dead human player respawns on request (default key **Backspace**) once the delay has
elapsed and combat is not frozen (the Ending/PostMatch phases freeze combat). Bots
respawn automatically.

### `[rules]`

| Field | Type | Constraints | Description |
|---|---|---|---|
| `friendly_fire` | string | `server` \| `on` \| `off` | `server` defers to `[gameplay] friendly_fire`; `on`/`off` override it |

---

## Match lifecycle

A match runs a deterministic state machine (`MatchController`):

`Idle` → `Warmup` → `Active` → `Ending` → `PostMatch` → (rotate) → `Warmup`

- **Idle**: no humans yet.
- **Warmup**: players spawn and fly; scoring is frozen; the countdown runs only while
  `min_players` are present.
- **Active**: scoring is live; the match ends on the score limit, the time limit, or a
  mission-scripted victory.
- **Ending**: combat is frozen and the final scoreboard is shown for
  `[match] end_screen_s` seconds.
- **PostMatch → rotate**: the world resets in-process (peers stay connected), the next
  `[rotation]` item's mode is loaded, and pilots are re-admitted.

---

## Validating

Lint a mode before shipping it:

```
validate-mode modes/tdm.toml
```

It shares the runtime parser and adds plausibility checks (duplicate team ids, a team
count that cannot fill the player cap, a warmup longer than the match clock, an
unreachable score limit).
