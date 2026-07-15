# Mission Authoring Guide

This guide covers every field in the mission YAML schema. It is aimed at fl-base-pack
contributors who want to author missions for Fighters Legacy.

---

## Overview

A mission file is a YAML document that the engine loads into a `MissionData` asset via
`FolderContentPack`. The engine reads it at mission start; no runtime reloading occurs.
Missions can be standalone or embedded in a campaign's `story` sequence.

Files live in `missions/` inside the content pack directory.

---

## Required top-level fields

```yaml
name: "Storm Warning"
map: ukraine
layer: ukraine_clear
time: { hour: 14, minute: 0 }
wind: { heading: 270, speed: 12 }
sides: [nato, russia]
objects:
  - ...
triggers:
  - ...
```

| Field | Type | Constraints | Description |
|---|---|---|---|
| `name` | string | non-empty | Display name shown in mission select and after-action screens |
| `map` | string | non-empty | Terrain asset ID — must match a file in `terrain/` |
| `layer` | string | non-empty | Weather/lighting layer ID (e.g. `ukraine_clear`, `ukraine_overcast`) |
| `time.hour` | int | 0–23 | Local mission start hour |
| `time.minute` | int | 0–59 | Local mission start minute |
| `wind.heading` | int | 0–359 | Wind-from heading in degrees (0 = north, 90 = east) |
| `wind.speed` | float | ≥ 0 | Steady-state wind speed in **m/s**. Gusts are added by the engine on top of this value. |
| `sides` | sequence | ≥ 1 element | Coalitions active in this mission — see Coalitions below. Each entry is a side id (string) or a mapping with an `id` and optional `allies` |
| `objects` | sequence | ≥ 1 element | Unit and aircraft placements — see Objects section |
| `triggers` | sequence | — | Win/loss/event conditions — see Triggers section |

## Optional top-level fields

| Field | Type | Default | Description |
|---|---|---|---|
| `weather.preset` | string | `clear` | Initial weather: `clear`, `partly_cloudy`, `overcast`, `rain`, `storm`, `snow`, `blizzard`. Gust amplitude and turbulence intensity scale with the preset. `snow` and `blizzard` produce snow precipitation at any altitude. |
| `time_scale` | float | server default (10) | Game seconds per real second for this mission. Overrides `[world] time_scale` in `server.toml`. Use `1.0` for real-time cinematic missions. |

Example:
```yaml
weather:
  preset: partly_cloudy
time_scale: 10.0   # omit to use server default
```

---

## Coalitions

`sides` declares the coalitions in the mission. In the simplest form it is a flat list of ids, and
every side is hostile to every other:

```yaml
sides: [nato, russia]
```

To make sides **allied**, give an entry as a mapping with an `allies` list. Allied sides are friendly
to each other; any side not in your `allies` list is hostile. Alliances are symmetric — declaring
`ukraine` as an ally of `nato` makes the pair friendly in both directions.

```yaml
sides:
  - id: nato
    allies: [ukraine]   # nato + ukraine are friendly; both are hostile to russia
  - id: ukraine
  - id: russia
```

| Field | Type | Required | Constraints | Description |
|---|---|---|---|---|
| `id` | string | yes (map form) | non-empty | Coalition id; referenced by each object's `side` |
| `allies` | sequence | no | each must name another side in `sides` | Sides this coalition is friendly with |

An ally that names an unknown side is a hard error; listing yourself as your own ally is ignored with
a warning. The engine loads these into the faction registry so AI decides friend-from-foe by the
coalition graph, not by a crude "different team = enemy" rule.

---

## `objects` entries

Each entry in the `objects` list places one entity in the world at mission start.

```yaml
objects:
  - type: F22
    id: player1
    side: nato
    pos: [12400, 0, 8800]
    heading: 90
    alt: 500

  - type: SA10
    id: sam1
    side: russia
    pos: [15000, 0, 9000]
    heading: 0
```

| Field | Type | Required | Constraints | Description |
|---|---|---|---|---|
| `type` | string | yes | non-empty | Aircraft or unit type ID — must resolve to a TOML asset in `aircraft/` or `entities/` |
| `id` | string | yes | unique across all objects in the file | Internal ID referenced by triggers and Lua scripts |
| `side` | string | yes | must appear in top-level `sides` list | Coalition this unit belongs to |
| `pos` | sequence | yes | exactly 3 numbers: [x, y, z] in metres | World-space spawn position. Y is up; sea level ≈ 0 |
| `heading` | float | yes | — | Initial heading in degrees (0 = north, clockwise) |
| `alt` | float | no | — | Altitude above sea level in metres; overrides `pos[1]` if both given |
| `player` | bool | no | default `false` | When `true`, this object is a **joinable player slot**: the engine does not spawn it as an AI/world entity — a connecting pilot is assigned to it and inherits its faction, spawn position, and aircraft type. Declare more than one for multiplayer. |

Object IDs must be unique — the validator will reject duplicate IDs within a single file.

---

## `triggers` entries

Triggers define events that fire during the mission. The engine evaluates the `on` predicate
each simulation tick and executes the `do` action once when it first becomes true.

```yaml
triggers:
  - on: destroy(sam1)
    do: mission_success

  - on: timer(300)
    do: mission_failure

  - on: mission_start
    do: spawn(Su27,russia,[15500,0,9200])
```

| Field | Type | Required | Description |
|---|---|---|---|
| `on` | string | yes | Trigger predicate — see supported forms below |
| `do` | string | yes | Action to execute when predicate becomes true — see supported forms below |

### Supported `on` predicates

| Form | Description |
|---|---|
| `destroy(<id>)` | Fires when the object with the given ID is destroyed. `id` must exist in `objects`. |
| `mission_start` | Fires once, immediately when the mission begins. |
| `timer(<seconds>)` | Fires after the given number of seconds have elapsed since mission start. |

Additional predicates may be available to Lua scripts via `world.on_trigger()`. The validator
only checks the forms listed above; unknown predicate strings are passed through without error.

### Supported `do` actions

| Form | Description |
|---|---|
| `mission_success` | Ends the mission as a success. |
| `mission_failure` | Ends the mission as a failure. |
| `spawn(<type>,<side>,<pos>)` | Spawns a new unit of `type` for coalition `side` at world position `pos` (x,y,z comma-separated, no spaces). Example: `spawn(Su27,russia,15500,0,9200)` |

Unknown action strings are passed through to Lua scripts without error.

---

## Cross-references between triggers and objects

If a trigger's `on` predicate references an object by ID (e.g. `destroy(sam1)`), the
`validate-mission` tool verifies that the ID exists in the `objects` list. An object referenced
in a trigger but not defined in `objects` is a hard error — the engine cannot resolve it at
runtime.

---

## Worked example — "Storm Warning"

```yaml
name: "Storm Warning"
map: ukraine
layer: ukraine_clear
time: { hour: 14, minute: 0 }
wind: { heading: 270, speed: 12 }  # 12 m/s westerly; gusts added by engine
weather:
  preset: rain
sides: [nato, russia]

objects:
  - type: F22
    id: player1
    side: nato
    pos: [12400, 0, 8800]
    heading: 90
    alt: 500

  - type: SA10
    id: sam1
    side: russia
    pos: [15000, 0, 9000]
    heading: 0

triggers:
  - on: destroy(sam1)
    do: mission_success

  - on: timer(600)
    do: mission_failure
```

---

## Validation

Run `validate-mission <file.yaml>` to check a mission file before committing it. The tool
reports all errors in a single pass so contributors see the full list at once.

Exit codes: 0 = valid, 1 = validation failure, 2 = bad arguments.

`validate-mission` and the engine share **one** schema: the linter delegates to the engine's own
mission parser (`engine-mission`), so a mission the tool passes is a mission the engine loads. The
schema described in this document is that parser — the two cannot drift.

Schema source: this document. For format reference and additional asset types
see [`docs/modding/formats.md`](formats.md).
