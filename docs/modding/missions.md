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
| `airspace_zones` | sequence | none | Restricted airspace the server enforces. See [`airspace_zones`](#airspace_zones--restricted-airspace-optional). |

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
| `alert` | string | no | `peacetime` \| `elevated` \| `conflict` \| `war_state` | Starting airspace readiness posture (default `peacetime`). See [`airspace_zones`](#airspace_zones--restricted-airspace-optional). |

An ally that names an unknown side is a hard error; listing yourself as your own ally is ignored with
a warning. The engine loads these into the faction registry so AI decides friend-from-foe by the
coalition graph, not by a crude "different team = enemy" rule.

---

## `objects` entries

Each entry in the `objects` list places one entity in the world at mission start.

```yaml
objects:
  - type: fl-base:f22
    id: player1
    side: nato
    pos: [12400, 0, 8800]
    heading: 90
    alt: 500

  - type: fl-base:sa10_battery
    id: sam1
    side: russia
    pos: [15000, 0, 9000]
    heading: 0
    start: ground
    ai: "sam"
```

| Field | Type | Required | Constraints | Description |
|---|---|---|---|---|
| `type` | string | yes | non-empty | Aircraft or unit type ID — must resolve to a TOML asset in `aircraft/` or `entities/` |
| `id` | string | yes | unique across all objects in the file | Internal ID referenced by triggers and Lua scripts |
| `side` | string | yes | must appear in top-level `sides` list | Coalition this unit belongs to |
| `pos` | sequence | yes | exactly 3 numbers: [x, y, z] in metres | World-space spawn position. Y is up; sea level ≈ 0 |
| `heading` | float | yes | — | Initial heading in degrees (0 = north, clockwise) |
| `alt` | float | no | — | Altitude above sea level in metres; overrides `pos[1]` if both given |
| `speed` | float | no | ≥ 0 | Initial airspeed in **m/s** along the object's heading. Absent = a sane cruise default so an airborne object is in stable flight at spawn (an aircraft dropped in at 0 airspeed departs controlled flight); set `0` for a stationary start. Applies to AI objects and player slots alike. Ignored (forced to 0) on a `start: ground` object. |
| `start` | string | no | `air` \| `ground` | `air` (default) drops the object in at its `alt`/`pos[1]` with a cruise airspeed. `ground` spawns it **parked on the terrain** — placed at ground level, idle throttle, zero airspeed, held stable until the pilot throttles up and rotates. Use `ground` for a runway/ramp start. |
| `player` | bool | no | default `false` | When `true`, this object is a **joinable player slot**: the engine does not spawn it as an AI/world entity — a connecting pilot is assigned to it and inherits its faction, spawn position, aircraft type and `loadout`. Declare more than one for multiplayer. |
| `ai` | string | no | — | Attach an AI controller (ignored on a player slot). See Scripted bots below. |
| `route` | sequence | no | each entry is `[x, y, z]` | A waypoint list the object flies. Takes precedence over `ai` when both are given. Ignored on a player slot. |
| `loadout` | sequence of strings | no | each store must be in that station's `allowed` list | Per-station weapon override of the entity's default payload — **on AI objects and player slots alike** (see Scripted bots). |
| `crew` | mapping | no | see **Crew configuration** | Bot skill ranges and per-seat overrides for a multi-crew aircraft. Ignored on a player slot. |

Object IDs must be unique — the validator will reject duplicate IDs within a single file.

---

## Crew configuration

A multi-crew aircraft (one that declares `[[crew]]` seats — see the entity format) can have its bots
configured per mission via a `crew:` block on the object. The engine rolls each bot seat's
per-instance skill deterministically from the **mission name** (seeded so a replay is byte-identical and
two missions differ), within the configured range — a flight of bombers is not uniformly deadly.

    - type: fl-base:b17
      id: bomber1
      side: allies
      pos: [40000, 3000, 12000]
      heading: 270
      ai: "waypoint 40000 3000 12000"
      crew:
        skill: [0.3, 0.8]        # aircraft-level range every bot seat rolls within (or a single number = fixed)
        seats:
          - role: tail-gunner    # name a seat by `role` OR by `seat: <index>` (exactly one)
            skill: 0.9           # per-seat skill override (fixed or [min, max]); wins over the aircraft range
            bot: builtin:gunner  # override the seat's bot spec
          - seat: 2
            empty: true          # spawn this seat empty (it contributes no fire) instead of a bot

`crew:` fields:

| Field | Type | Notes |
|-------|------|-------|
| `skill` | number or `[min, max]` | Aircraft-level skill range in `[0, 1]` all bot seats roll within. A single number is a fixed skill. |
| `seats` | sequence | Per-seat overrides. |
| `seats[].seat` | int | Seat index to override. **Exactly one** of `seat` / `role` must be set. |
| `seats[].role` | string | Seat role to override (resolved to an index against the entity's `[[crew]]`). |
| `seats[].skill` | number or `[min, max]` | Per-seat skill override (wins over the aircraft range). |
| `seats[].bot` | string | Override the seat's bot spec (`gunner` / `builtin:gunner` / `lua:<script>`). |
| `seats[].empty` | bool | `true` spawns the seat empty (no bot). |

`validate-mission --pack <dir>` cross-checks a `crew:` block against the referenced entity type's
declared `[[crew]]`: a `seat`/`role` the entity does not have, or a `crew:` block on a single-seat
entity, is an error.

---

## Scripted bots

An object can be given a brain, a route, and a custom loadout so a mission stands up armed adversaries
without a human at the stick.

```yaml
objects:
  - type: fl-base:f5e
    id: bandit
    side: red
    pos: [22000, 0, 18000]
    alt: 4500
    heading: 225
    ai: "lua fighter"                       # a Lua behavior from the pack's ai/ directory
    loadout: [fl-base:m39a2, fl-base:aim9p, "~"]   # gun + one wingtip missile; strip the other rail
    route:
      - [22000, 4500, 18000]
      - [10000, 4500, 6000]
```

### `ai`

A controller spec, using the same grammar as the `spawn --ai` admin command:

- `lua <script>` — a Lua behavior loaded from the content pack's `ai/` directory (by asset name).
- a C++ behavior name plus arguments — e.g. `pursuit <idx>`, `loiter <cx> <cy> <cz> [radius] [alt]`,
  `patrol_attack <idx>`, `escort <idx>`. See the AI controller factory for the full list.

**Static air defence** has two behaviours, for emplacements placed with `start: ground`. Neither
takes a target index — both engage whatever hostile they honestly detect, so a battery a jammer has
blinded engages nothing:

| Behaviour | Arguments (all optional, in order) | Notes |
|---|---|---|
| `sam` | `engageRangeM=30000`, `coneHalfDeg=90`, `fireIntervalS=4`, `launchElevMinDeg=35` | Acquires on radar and launches a SARH. `launchElevMinDeg` is the floor on how far above the local horizon the missile leaves: an emplacement's nose is horizontal, so a store launched flat starts at deck level and the ground check ends it before it can climb. Set `0` only for a launcher that genuinely fires flat. |
| `aaa` | `engageRangeM=1200`, `coneHalfDeg=25`, `muzzleVelMps=1000`, `lethalRadiusM=15` | Leads the target with the ballistic solution and fires when the predicted miss is inside the lethal radius. Fires along its **fixed** nose — a gun emplacement has no elevation yet, so it engages what crosses its boresight. |

    - { type: fl-base:sa10_battery, id: redsam, side: red, pos: [9500, 0, 0], heading: 90, start: ground, ai: "sam" }
    - { type: fl-base:zsu23, id: redaaa, side: red, pos: [9200, 0, 500], heading: 90, start: ground, ai: "aaa 1800" }

### `route`

A list of `[x, y, z]` waypoints the object flies via a waypoint-following controller. When both `route`
and `ai` are present, `route` wins (the object flies its waypoints); the validator warns.

### `loadout`

Overrides the entity's default payload station by station. Each entry names the store on that station in
hardpoint order; `~`, `-`, or an empty string leaves a station **empty**, and listing fewer stores than
the aircraft has stations keeps the remaining stations at their defaults. A store that is not in the
station's `allowed` list (or is an unknown weapon id) is refused and the station is left empty. `ai`
and `route` on a `player: true` slot are ignored (a human flies it) — the validator warns.

`loadout` **does** apply to a player slot: it is how a mission says what the pilot takes off with, and
it is applied when a pilot is assigned to the slot rather than at spawn. A gunnery lesson can put the
student on the gun instead of asking for the discipline in briefing text while the rails stay live:

    objects:
      - type: fl-base:f5e
        id: student
        side: blue
        pos: [4000, 0, 1200]
        heading: 90
        start: ground
        player: true
        loadout: [fl-base:m39a2, "~", "~"]   # guns only — both wingtip rails stripped

It is a **fixed fit**, not a default the pilot may change: nothing in the engine lets stores be
re-chosen after the slot is claimed, so what the mission names is what is flown.

---

## `triggers` entries

Triggers define events that fire during the mission. The engine evaluates the `on` predicate
server-side at a second-scale cadence (not every 60 Hz tick) and executes the `do` action once,
the first time it becomes true. Triggers are evaluated in the order they appear in the file.

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
| `mission_success` | Ends the mission as a success (a terminal objective action). |
| `mission_failure` | Ends the mission as a failure (a terminal objective action). |
| `set_weather <preset>` | Transitions the weather to `<preset>` (`clear`/`partly_cloudy`/`overcast`/`rain`/`storm`/`snow`/`blizzard`). Example: `do: set_weather storm` on an objective-destroy trigger. |
| `set_time <hours>` | Sets the in-game time of day (0–24, float). Example: `do: set_time 20.5`. |
| `spawn(<type>,<side>,<pos>)` | Spawns a new unit of `type` for coalition `side` at world position `pos` (x,y,z comma-separated, no spaces). Example: `spawn(Su27,russia,15500,0,9200)` |
| `detonate <x> <y> <z> <radius_m> <damage>` | An AoE warhead at a world point (linear falloff; add `--nuclear` for the EMP ring). A scripted flak/explosion event. Example: `do: detonate -880 1800 0 600 30`. |
| `atc_scramble <airport> <type> [count]` | Launches `count` AI departures of `type` from a named airport in ATC-sequenced order. Example: `do: atc_scramble builtin:airfield builtin:debug-entity 1`. |

Non-terminal actions (everything except `mission_success`/`mission_failure`) are routed
server-side through the **same validated command path the admin console uses** — a mission can do
exactly what an operator could, and nothing more. Unknown action strings are logged and skipped
(or handled by a Lua script's `world.on_trigger()`), never a hard error.

---

## `airspace_zones` — restricted airspace (optional)

The optional `airspace_zones:` block declares regions of airspace a coalition **enforces**. It is
server-authoritative: an aircraft of another side that enters a zone starts a dwell timer and
escalates through *warned* → *interceptors scrambled* → *weapons free* the longer it stays. Clients
are told about posture changes but decide nothing.

A zone is a shape in the **XZ plane crossed with an altitude band** — the same planar `(x, z)`
vocabulary `[spawn]` points use. Circles take a `center`/`radius`; polygons take a convex `vertices`
ring. (`center` is written `[x, y, z]` for symmetry with an object's `pos`, but the Y component is
ignored: `alt_floor`/`alt_ceiling` own the vertical extent.)

```yaml
sides:
  - { id: nato }
  - { id: russia, alert: elevated }   # starting posture; default peacetime

airspace_zones:
  - id: capital_airspace
    type: circle
    center: [15000, 0, 8000]   # world XYZ; the zone test is in the XZ plane
    radius: 5000               # metres
    alt_floor: 0               # metres above the datum
    alt_ceiling: 12000
    owner: russia              # the coalition that enforces the zone
    policy: military_intercept # a zones/<id>.toml in the content pack

  - id: border_region
    type: polygon              # convex ring in XZ
    vertices: [[0,0],[10000,0],[15000,5000],[10000,10000],[0,10000]]
    alt_ceiling: 999999
    owner: russia              # no `policy:` — the builtin default posture applies
```

| Field | Type | Applies to | Required | Default | Description |
|---|---|---|---|---|---|
| `id` | string | all | yes | — | Zone id, unique within the mission; referenced by Lua zone queries |
| `type` | string | all | yes | — | `circle` \| `polygon` |
| `owner` | string | all | yes | — | The enforcing side; must appear in `sides` |
| `center` | `[x,y,z]` | circle | yes | — | Centre in world metres; the Y component is ignored |
| `radius` | number | circle | yes | — | Zone radius in metres (> 0) |
| `vertices` | sequence | polygon | yes | — | ≥ 3 `[x, z]` pairs forming a **convex** ring |
| `alt_floor` | number | all | no | 0 | Band floor, metres above the datum (inclusive) |
| `alt_ceiling` | number | all | no | 999999 | Band ceiling, metres (inclusive); must exceed `alt_floor` |
| `policy` | string | all | no | builtin default | Escalation policy id — a `zones/<id>.toml` in a content pack |

### How escalation works

Every zone owner has an **alert level** — `peacetime`, `elevated`, `conflict` or `war_state` — set by
`sides[].alert` and changed at runtime by `world.set_alert_level()`. The owner's *current* level
picks which row of the zone's escalation policy applies, so raising a coalition's alert level tightens
every zone it owns at once without editing a single zone.

Within a zone an intruder passes through five stages: `clean` → `in_zone` → `warned` → `intercept` →
`hostile`. Dwell thresholds are cumulative from the moment of entry, and a threshold of `0` means the
stage is already satisfied on entry — which is how the `war_state` row means "weapons free the instant
you cross the line, no warning". Two shortcuts apply: a side already **hostile** to the owner (per the
coalition graph) is weapons-free on entry regardless of dwell, and the owner is never an intruder in
its own airspace.

Leaving the zone fires a zone-exit event. Whether that *helps* is the policy's call: with
`compliance_reset` the zone forgets the intruder after a cooldown, so turning back works; without it
the stage sticks, which is the point of a wartime posture. Lowering the alert level relaxes the
schedule for the next intruder — it never revokes a weapons-free call already made on this one.

A zone whose `owner` does not name a side is a hard error at parse time. A zone whose `policy` names a
file the loaded packs do not ship is **not** an error: it falls back to the builtin default posture, so
a missing pack degrades the zone rather than silently switching enforcement off. Escalation policy TOML
is documented in [`formats.md`](formats.md).

Zone state is readable from Lua via `world.get_zone_stage(entity_idx, zone_id)` and
`world.is_in_zone(entity_idx, zone_id)`; see [`ai.md`](ai.md).

---

## `cameras` — cinematic shots (optional)

The optional `cameras:` block scripts camera shots for the **cinematic demo-recording pipeline**
(`docs/developer/demo-recording.md`). It is **presentation-only**: the server parses and ignores it, and the
recording client's `ShotDirector` consumes it to drive the camera while recording. A normal
single-player or multiplayer session ignores it entirely.

Shot times are in **sim-seconds from mission start**. Shots must be listed in ascending `start`
order and must not overlap (a shot occupies `[start, start + duration)`); a gap between shots holds
the previous shot's final pose. There are four shot types:

```yaml
cameras:
  shots:
    - { type: static, start: 0,  duration: 8,  pos: [1200, 250, -300], look_at: player1, fov: 60 }
    - { type: orbit,  start: 8,  duration: 12, target: bandit1, radius: 400, height: 60, period: 30 }
    - { type: chase,  start: 20, duration: 15, target: player1, offset: [-60, 15, 0],
        stiffness: 4.0, look_at: bandit1 }          # offset is in the target's body frame [aft, up, right]
    - { type: move,   start: 35, duration: 7,  look_at: player1, ease: smooth,
        keyframes: [ { time: 0, pos: [800, 300, -100] }, { time: 7, pos: [-200, 280, 150] } ] }
```

| Field | Type | Applies to | Required | Default | Description |
|---|---|---|---|---|---|
| `type` | string | all | yes | — | `static` \| `orbit` \| `chase` \| `move` |
| `start` | number | all | yes | — | Sim-seconds from mission start (≥ 0) |
| `duration` | number | all | yes | — | Shot length in seconds (> 0) |
| `fov` | number | all | no | 60 | Vertical FOV in degrees, in [20, 120] |
| `look_at` | id or `[x,y,z]` | all | static/move: yes; orbit/chase: no | target | An object id **or** a fixed world point |
| `pos` | `[x,y,z]` | static | yes | — | Fixed eye position, world metres, Y up |
| `alt` | number | static | no | — | MSL altitude override of `pos[1]` |
| `target` | id | orbit, chase | yes | — | The tracked object id (cross-checked against `objects`) |
| `radius` | number | orbit | no | 300 | Orbit radius in metres |
| `height` | number | orbit | no | 50 | Orbit height above the target in metres |
| `period` | number | orbit | no | 30 | Seconds per revolution; **negative = clockwise** |
| `offset` | `[aft,up,right]` | chase | no | `[-60, 15, 0]` | Eye offset in the target's body frame |
| `stiffness` | number | chase | no | 0 | Exponential eye-smoothing rate (1/s); 0 = rigid |
| `keyframes` | sequence | move | yes | — | ≥ 2 `{ time, pos }` entries; `time` is relative to the shot start |
| `ease` | string | move | no | linear | `linear` or `smooth` (Catmull-Rom when > 2 keyframes) |

`target` and any `look_at` that names an object id are cross-checked against `objects` (an unknown id
is a hard error, the same rule as `destroy(<id>)` triggers). A **cameras-only sidecar document** (a
YAML doc containing just a `cameras:` block, used with the recorder's `--shot-track` override) parses
through the same entry point; because it declares no `objects`, its id cross-references are not
checked. All camera errors accumulate like the rest of the schema.

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
  - type: fl-base:f22
    id: player1
    side: nato
    pos: [12400, 0, 8800]
    heading: 90
    alt: 500

  - type: fl-base:sa10_battery
    id: sam1
    side: russia
    pos: [15000, 0, 9000]
    heading: 0
    start: ground
    ai: "sam"

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

---

## Running a mission as an automated test

A mission can run headless to completion and report its outcome, so a scripted scenario doubles as an
integration test:

```
fl-server --mission <name> --mission-report outcome.json
```

fl-server steps the mission in a deterministic fixed-step loop (no clients, no wall-clock, the overrun
governor pinned off), writes a JSON outcome — `outcome` (`success`/`failure`/`incomplete`), plus
`elapsed_seconds`, `ticks`, `triggers_fired`, `live_entities`, and `spawned_objects` — and exits. The
`tools/mission_test/mission_test.py` wrapper runs this and asserts on the result; the
`mission_harness_ci_smoke` ctest exercises it against the `ci-mission-pack` fixture mission.

For a run to be reproducible, keep triggers to the built-in predicates/actions and Lua behaviors to
pure functions of `(state, tick, dt, contacts)` — no wall-clock, no unseeded RNG.
