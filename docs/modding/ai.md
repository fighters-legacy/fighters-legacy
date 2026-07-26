# Lua AI Scripting

Fighters Legacy supports Lua 5.5 AI scripts that drive server-side entity behavior through
the same `IEntityController` seam used by the built-in C++ autopilot controllers. Scripts are
sandboxed: the `io`, `os`, `debug`, and `package` globals are nil; `require()` is restricted to
the pack's own `ai/` directory.

---

## Phase 1 API (available now)

This document covers the Phase 1 `compute_control` function-call model. Each entity with a Lua
controller runs one persistent `lua_State` for the entity's lifetime. The engine calls your
`compute_control` function every sim tick (~60 Hz). Module-level variables persist between ticks,
enabling state machines, counters, and timers without coroutines.

See [`world.*` — engine integration](#world--engine-integration-413) and
[Coroutine control flow](#coroutine-control-flow-ai_main--412) below for the richer bindings and the
sequential-state-machine model.

---

## Script anatomy

```lua
-- ai/loiter.lua
-- Module-level variables persist between ticks.
local cx, cz, alt = 0, 0, 600
local radius = 3000

-- compute_control is called every sim tick (60 Hz).
-- state: table of entity state fields (see below)
-- tick:  integer sim tick counter (monotonically increasing)
-- dt:    number, step duration in seconds (typically 1/60)
-- Returns a table of control fields (all optional; missing = 0/false).
function compute_control(state, tick, dt)
    local pos  = state.pos
    local quat = state.quat
    local nx   = cx - pos.x
    local nz   = cz - pos.z
    local dist = math.sqrt(nx * nx + nz * nz)
    if dist < 1 then
        return {throttle = 0.65}
    end
    nx, nz = nx / dist, nz / dist
    local tx   = pos.x + nx * math.min(dist, 1000) + nz * 1000
    local tz   = pos.z + nz * math.min(dist, 1000) - nx * 1000
    local herr = guidance.heading_error(quat, pos, {x = tx, y = pos.y, z = tz})
    local perr = guidance.pitch_error_from_alt(quat, pos, alt - pos.y)
    return {
        aileron  = guidance.bank_to_turn_aileron(herr),
        rudder   = guidance.coordinated_rudder(guidance.bank_to_turn_aileron(herr)),
        elevator = guidance.elevator_from_pitch_error(perr),
        throttle = 0.65,
    }
end
```

---

## What your script can and cannot see

**Your AI is not omniscient, and this is not optional.** Every entity the server controls runs its
sensors each tick, and a script sees the result — not the world.

| Question | Answer |
|---|---|
| Where is the enemy? | Only if you have **detected** it. `detected_contacts()` is your world. |
| How do I know where it is *now*? | You do not. A contact carries **last-known** position; `age_s` tells you how stale that is. |
| Can I see behind me? | Only if a sensor's cone covers it. An aircraft with a forward-facing suite genuinely cannot see its own six. |
| Can I see through cloud, or at night? | Worse than in clear daylight, and it depends on the sensor: at night an eyeball keeps ~25% of its chance while radar and IR are unaffected. |
| What if the entity has no sensors declared? | It gets the **builtin eyeball**. There is no configuration in which an AI sees through terrain. |

**An entity with no sensors is not blind and not omniscient — it has eyes.** A content pack cannot
opt out of honest sensing by leaving the `sensors` list off an entity; it just gets the default one.

Two escape hatches still exist for compatibility, and both are cheating: `nearby_entities()` and
`get_entity()` read ground truth directly. Use them for debug scripts and for anything that is
deliberately not a pilot (a scripted camera, a test rig). If you use them to fly a fighter, your AI
will behave like it is reading the server's memory — because it is.

---

## `compute_control(state, tick, dt)` — required function

| Parameter | Type   | Description                                           |
|-----------|--------|-------------------------------------------------------|
| `state`   | table  | Live entity state (see fields below)                  |
| `tick`    | number | Monotonically increasing sim tick counter             |
| `dt`      | number | Seconds since last tick (typically `1/60`)            |

**Return value:** a table. All fields are optional; absent fields default to `0`/`false`.

---

## `state` table fields

| Field          | Type    | Description                                              |
|----------------|---------|----------------------------------------------------------|
| `state.pos`    | `{x,y,z}` | World position in metres (double precision)           |
| `state.vel`    | `{x,y,z}` | World velocity in m/s                                 |
| `state.quat`   | `{x,y,z,w}` | Orientation quaternion                              |
| `state.hp`     | number  | Current hit points                                       |
| `state.max_hp` | number  | Maximum hit points                                       |
| `state.damage_level` | integer | `0`=Intact, `1`=Light, `2`=Heavy, `3`=Critical, `4`=Destroyed |
| `state.dead`   | boolean | True when the entity is destroyed (controller is not called while dead) |
| `state.player_owned` | boolean | True for player-controlled entities             |
| `state.owner_id` | integer | Peer ID of the owning player; `0` for server/AI   |
| `state.faction` | integer | This entity's faction (`0` = neutral). **Compare against a contact's `faction` to tell friend from foe** — without it every contact looks hostile |
| `state.type_index` | integer | Entity type index into the server's type registry |

**Coordinate convention:** Y-up, right-handed, +X forward.

---

## Return table fields

| Field         | Type    | Range    | Description                        |
|---------------|---------|----------|------------------------------------|
| `elevator`    | number  | `[-1,1]` | `+1` = pull = nose-up command      |
| `aileron`     | number  | `[-1,1]` | `+1` = right roll                  |
| `rudder`      | number  | `[-1,1]` | `+1` = right yaw                   |
| `throttle`    | number  | `[0,1]`  | `0` = idle, `1` = MIL power        |
| `afterburner` | boolean | —        | `true` = afterburner on            |
| `speedbrake`  | number  | `[0,1]`  | `0` = retracted, `1` = fully deployed |
| `gear_down`   | boolean | —        | `true` = landing gear extended     |
| `trigger`     | boolean | —        | `true` = hold the gun trigger (level; the server rate-limits) |
| `release`     | boolean | —        | `true` = fire the selected store (the server edge-detects: holding it is ONE shot) |
| `weapon_station` | integer | `[0,254]` | Absolute station selection; absent or out-of-range = keep the current selection |

The fire fields are **intents**, not actions: the server's fire control validates station, ammo,
rate of fire, and any wingman weapons-hold order exactly as it does for a player. A script that
holds `trigger` forever gets what a trigger-holding player gets — a rate-limited burst until the
magazine is empty.

---

## `guidance.*` module

Thin wrappers over the engine's `Guidance.h` inline math. The quaternion parameter is always
`{x,y,z,w}`; position parameters are `{x,y,z}` tables.

### `guidance.heading_error(quat, own_pos, target_pos, [radius_m]) → number`

Signed horizontal angle in radians from the current heading to the bearing toward `target_pos`,
measured in the **local tangent plane at `own_pos`** — correct anywhere on the sphere, not just
near the world origin. Positive = target is to the right. Returns `0` when the tangent-plane
distance < 0.1 m. `radius_m` is the planet radius and defaults to Earth; scripts almost never
pass it.

### `guidance.pitch_error_from_alt(quat, own_pos, alt_error_m, [radius_m]) → number`

Signed pitch error in radians needed to close an altitude gap of `alt_error_m` metres.
Gain: 0.002 rad/m, clamped to ±30°.

`own_pos` is **required, not redundant**: the world is a sphere, so "up" — and therefore what
counts as pitch — is the geodetic radial direction *at your position*. The function cannot
measure your pitch against the local horizon without knowing where that horizon is. (Omitting it
does not degrade gracefully: the call raises a Lua error every tick and the controller returns
neutral controls, so the aircraft flies straight ahead while the log fills.) `radius_m` defaults
to Earth.

### `guidance.bank_to_turn_aileron(heading_error_rad) → number`

Maps heading error to an aileron command. Gain: `2/π` (90° error → full deflection).

### `guidance.coordinated_rudder(aileron) → number`

Rudder command proportional to aileron (gain 0.3). Keeps the ball centred in a coordinated turn.

### `guidance.elevator_from_pitch_error(pitch_error_rad) → number`

Maps pitch error to an elevator command. Gain: `2/π`.

### `guidance.body_forward(quat) → {x, y, z}`

Extracts the world-frame forward vector (+X body axis) from the quaternion.

---

## `detected_contacts() → array`

**The honest view — use this one.** Returns what your entity has actually *detected* through its
sensors, and nothing else.

`nearby_entities()` (below) is a raw radius query over ground truth: it sees through terrain, through
the back of your aircraft's head, and at any range you ask for. `detected_contacts()` returns only
targets your sensors have found, with **last-known** position and velocity — a *coasting* contact
reports where the target **was**, not where it is.

Each entry:

| Field | Type | Notes |
|---|---|---|
| `idx` | int | Target entity index |
| `state` | string | `detected` (search lobe — bearing, no firing-quality track), `locked` (track lobe), `coasting` (was held, geometry lost, running out `lock_hold_s` on last-known state) |
| `pos` | `{x,y,z}` | **Last-known** position — not live truth |
| `vel` | `{x,y,z}` | Last-known velocity |
| `age_s` | number | Seconds since the target was last actually *seen*. `0` while it is being seen; it grows while coasting — which is exactly when you should stop trusting `pos` |
| `reacted` | bool | `false` until the reaction delay has elapsed. **A contact exists before its owner has noticed it** — see below |
| `faction` | int | Target's faction index (`0` = neutral) |
| `sensor_types` | string[] | Which kinds hold it: `radar`, `ir`, `visual`, `laser`. "He has me on radar" and "he can see me" are different tactical facts |

```lua
function compute_control(state, tick, dt)
    for _, c in ipairs(detected_contacts()) do
        if c.reacted and c.faction ~= 0 and c.faction ~= state.faction then
            -- Steer at the LAST-KNOWN position. If the contact is coasting, this is a guess —
            -- age_s tells you how old a guess it is.
            local herr = guidance.heading_error(state.quat, state.pos, c.pos)
            return { aileron = guidance.bank_to_turn_aileron(herr), throttle = 1.0 }
        end
    end
    return { throttle = 0.6 }   -- nothing detected: no target to chase
end
```

**`reacted` is not a formality.** Detection and reaction are separate: your entity *sees* a contact
the instant its sensors find it, and `reacted` flips only once the reaction delay has elapsed
(server `[ai] difficulty` × the entity's own `[ai].reaction`). A script that ignores `reacted` is a
script whose AI has superhuman reflexes on every difficulty setting.

**Returns `{}` when sensing was not evaluated** (a headless caller or a unit test). That is not the
same as an empty table from a real check, which means your sensors ran and found nothing — but for a
script both simply mean "no contacts", so existing scripts keep working unchanged.

> **`get_entity(idx)` still returns live ground truth**, including for an entity you have not
> detected. It is not restricted today because existing scripts depend on it. **Prefer the contact's
> own `pos`/`vel`** if you want your AI to behave honestly — a script that reads `get_entity` on a
> coasting contact is quietly cheating.

---

## `nearby_entities(cx, cz, radius_m) → array`

> **This is a ground-truth query, and it is omniscient.** It ignores cones, ranges, terrain and
> probability. Prefer [`detected_contacts()`](#detected_contacts--array) for anything that should
> behave like a pilot rather than a cheat.

Returns an array of `{idx, pos={x,y,z}}` tables for entities within `radius_m` metres in the XZ
plane, queried from the server's spatial index. Returns `{}` when the spatial index is unavailable.

The query is conservative (cells intersecting the radius square); it may include entities outside
the exact circle. Filter by exact distance if needed:

```lua
local nb = nearby_entities(state.pos.x, state.pos.z, 5000)
for _, e in ipairs(nb) do
    local dx = e.pos.x - state.pos.x
    local dz = e.pos.z - state.pos.z
    if dx*dx + dz*dz < 5000*5000 then
        -- e is within 5 km
    end
end
```

---

## `get_entity(idx) → state table or nil`

Returns the full state table for the entity with pool index `idx`, or `nil` if the entity is not
found, is dead, or the entity manager is unavailable. Combine with `nearby_entities` to get nearby
entity state:

```lua
local nb = nearby_entities(state.pos.x, state.pos.z, 3000)
if #nb > 0 then
    local target = get_entity(nb[1].idx)
    if target then
        local herr = guidance.heading_error(state.quat, state.pos, target.pos)
        return {aileron = guidance.bank_to_turn_aileron(herr), throttle = 0.85}
    end
end
return {throttle = 0.65}
```

---

## Error handling

If `compute_control` is missing or throws a runtime error, the engine returns a neutral
`ControlInput{}` (all zeros, no afterburner) and logs the error to stderr at most once per
60 ticks. The entity becomes aerodynamically inert but the server keeps running.

---

## Splitting scripts with `require()`

`require()` is restricted to the pack's own `ai/` directory. Use it to share helpers between
scripts:

```lua
-- ai/guidance_utils.lua
local M = {}
function M.loiter_aileron(state, cx, cz)
    local nx = cx - state.pos.x
    local nz = cz - state.pos.z
    local dist = math.sqrt(nx*nx + nz*nz)
    if dist < 1 then return 0 end
    local tx = state.pos.x + (nz/dist)*1000
    local tz = state.pos.z - (nx/dist)*1000
    return guidance.bank_to_turn_aileron(
        guidance.heading_error(state.quat, state.pos, {x=tx,y=state.pos.y,z=tz}))
end
return M
```

```lua
-- ai/patroller.lua
local gu = require('guidance_utils')
function compute_control(state, tick, dt)
    return {aileron = gu.loiter_aileron(state, 0, 0), throttle = 0.65}
end
```

Bytecode (precompiled `.lua` files starting with `\x1b`) is rejected by the sandbox.

---

## Pack conventions

- Script files live at `ai/<name>.lua` inside the content pack directory.
- Reference the script by name (no extension, no path) in entity TOML or the admin console:
  - Entity TOML: `ai_script = "patrol"` — auto-assigns the script when an entity of that type
    is spawned without an explicit `--ai` flag.
  - Admin console: `spawn <type> x y z --ai lua patrol`

---

## Performance

`lua_pcall` overhead at 60 Hz is negligible (< 1 µs for simple scripts on modern hardware).
Persistent Lua state is safe; module-level variables are not shared between entities (each entity
has its own `lua_State`). Avoid I/O or blocking operations inside `compute_control`.

---

## Lua 5.5 compatibility note

`global` is a reserved keyword in Lua 5.5. Scripts that use `global` as a variable name will fail
to load. Rename any such variables before shipping a pack targeting this engine version.

---

## Crew seat bots — the turret gunner (#966/#971)

A multi-crew aircraft (see the [`[[crew]]`/`[[turrets]]` schema](formats.md#crew-and-turrets-optional--multi-crew-seats-and-turret-mounts))
fills its non-flying seats with **seat bots**, which are a different (narrower) thing than the flying
AI above: a seat bot produces only a *seat command* (where to aim its turret and whether to fire), not
a full flight `ControlInput`. The engine ships one: the **turret gunner**.

Author it on a seat with `bot = "gunner"` (or `"builtin:gunner"`); a `fire` seat that aims a turret
also defaults to the gunner when its `bot` is left empty. The gunner:

- **Acquires honestly** off the aircraft's own contact table — a jammed or blind aircraft's gunner
  engages nothing, exactly like a pilot. It picks the nearest detected hostile within its turret's
  coverage arc.
- **Leads** the target with the shared ballistic solver, **slews** the turret onto the lead point
  (the server servo rate-limits and clamps it to the turret's traverse limits), and **holds fire**
  until the gun is actually pointed at the target and the predicted miss is inside the lethal cone.
- Fires along the **turret bore**, independent of the airframe nose.

**Per-instance skill** — the first in the engine. Each gunner rolls a skill in a range with a
deterministic seed (mission seed ⊕ object id ⊕ seat index), so two bombers from the same mission are
not equally deadly, and a replay is identical. A higher rolled skill **tightens the aim** (a smaller
error cone, scaled off the difficulty `aim_error_deg`) and **shortens the reaction** before the gunner
opens fire. The per-seat skill *range* is a mission-authored setting (mission & campaign runtime, #976);
until then the seat's authored `skill` is used as a fixed point.

Per-instance skill is not gunner-specific — plain mission AI can roll it too (the same
`rollPerInstanceSkill` seed helper), so a flight of interceptors need not fly at one uniform skill.

## Coroutine control flow (`ai_main`) — #412

Besides the per-tick `compute_control` model, a script may drive itself as a **coroutine** so you can
write a sequential state machine instead of a switch on module-level variables. Define `ai_main`
instead of `compute_control` (defining both makes `ai_main` win):

```lua
function ai_main()
    -- run-once setup happens before the first yield
    while true do
        -- yield the control table for THIS tick; execution resumes here next tick,
        -- and yield returns the next (state, tick, dt).
        local state, tick, dt = coroutine.yield({ throttle = 1.0 })
        if state.hp < state.max_hp * 0.5 then
            -- ... a whole sub-behaviour can live here, yielding each tick ...
            state = coroutine.yield({ throttle = 1.0, afterburner = true })
        end
    end
end
```

- The engine resumes `ai_main` once per sim tick, passing `(state, tick, dt)`.
- `coroutine.yield(control_table)` hands back the control table for the tick and suspends until the
  next tick; the fields are the same as `compute_control` returns (see the return table above).
- A yield with **no value** produces neutral control that tick.
- When `ai_main` returns (or errors), the behaviour is finished and every subsequent tick is neutral —
  the same fail-safe as a `compute_control` error.
- The coroutine shares the sandbox's globals, so `guidance.*`, `nearby_entities`, `get_entity`,
  `detected_contacts`, `world.*`, and the rumble bindings all work inside it.

## `world.*` — engine integration (#413)

The `world` module lets a mission/AI script touch the wider engine: spawning, faction relations,
mission outcome, music, and its own event triggers. On a dedicated server these calls run on the sim
thread and are routed through the host, so an unavailable capability is a safe no-op.

| Function | Description |
|----------|-------------|
| `world.spawn(type_id, pos, heading [, side])` | Spawn an entity of `type_id` at `pos` `{x,y,z}` facing compass `heading` (deg), for coalition `side` (default neutral). Returns the new entity's index, or `-1` on failure. A spawned aircraft gets a default loiter so it flies. |
| `world.despawn(entity_idx)` | Destroy the entity at `entity_idx`. |
| `world.set_relationship(a, b, rel)` | Set the symmetric relationship between faction ids `a` and `b`. `rel` = `"friendly"` / `"neutral"` / `"hostile"`. |
| `world.set_music_state(state)` | Ask connected clients to change music. `state` = `"menu"` / `"patrol"` / `"combat"` / `"success"` / `"debrief"`. |
| `world.set_alert_level(faction_id, level)` | Set a coalition's airspace readiness posture. `level` = `"peacetime"` / `"elevated"` / `"conflict"` / `"war_state"`. Server-authoritative and broadcast to clients; retunes every airspace zone that coalition owns at once. |
| `world.get_alert_level(faction_id)` | That coalition's current posture, same vocabulary. `"peacetime"` for an unknown faction or a server with no alert system, so a script can branch on it without checking first. |
| `world.get_zone_stage(entity_idx, zone_id)` | How far an intruder has escalated in an airspace zone: `"clean"` / `"in_zone"` / `"warned"` / `"intercept"` / `"hostile"`. `"clean"` for an unknown entity or zone. |
| `world.is_in_zone(entity_idx, zone_id)` | Whether that entity is inside the zone right now. |
| `world.mission_success()` / `world.mission_failure()` | End the current mission with the given outcome (drives the objective state machine). |
| `world.get_elapsed_time()` | Seconds since this controller started. |
| `world.on_trigger(predicate_fn, callback_fn)` | Fire `callback_fn` **once**, the first tick `predicate_fn()` returns true, then forget it. |
| `world.timer(seconds, callback_fn)` | Fire `callback_fn` once after `seconds` of sim time. |

```lua
-- Ambush: when the player closes inside 5 km, spawn two bandits and go to combat music.
function ai_main()
    world.on_trigger(
        function()
            local c = detected_contacts()
            return #c > 0
        end,
        function()
            world.spawn("builtin:debug-entity", { x = 8000, y = 3000, z = 0 }, 180, "russia")
            world.set_music_state("combat")
        end)
    world.timer(600, function() world.mission_failure() end) -- 10-minute fail-safe
    while true do coroutine.yield({ throttle = 0.7 }) end
end
```

`on_trigger`/`timer` are evaluated on the engine side each tick; a callback may itself call any
`world.*` / `guidance.*` function. A broken predicate or callback is logged (rate-limited) and dropped
— it never wedges the tick.

## Haptics — `rumble` (#128)

A script can trigger controller haptics. There is **no gamepad id** — the call always targets the
current player's gamepad (a server-side script's request is delivered to each client, which plays it
on its own pad). Intensities clamp to `[0, 1]` and one request is capped at **5000 ms**, so a mod
cannot latch rumble on; `stop_rumble()` is always available.

| Function | Description |
|----------|-------------|
| `rumble(low_freq, high_freq, duration_ms)` | Both rumble motors (low = heavy, high = buzz). |
| `rumble_triggers(left, right, duration_ms)` | Impulse-trigger motors (Xbox One / Series pads). |
| `stop_rumble()` | Cancel all rumble immediately. |

```lua
if state.hp < state.max_hp * 0.3 then
    rumble(0.8, 0.4, 300) -- a knock when badly hurt
end
```

See [`docs/haptics.md`](../haptics.md) for the full haptic design and the wire path.

## `atc.*` — air-traffic control (#705)

A script can talk to the server's deterministic ATC service (#702): sequence **its own** aircraft for
departure or arrival, or (for mission/director scripts) launch and hold traffic at a named airport.
Every function is a safe no-op returning `nil`/`false` when the server has no ATC configured
(`[atc] enabled = false`), so a script is never required to check first. The service is
server-authoritative and thread-safe — a script may call these from `compute_control`.

The self-service calls (`clearance`, `request_takeoff`, `request_landing`, `inbound`) act on the
entity the script is flying. An optional airport id targets a specific field; omitted, the **nearest**
field is used.

| Function | Description |
|----------|-------------|
| `atc.clearance() → string` | This aircraft's clearance state: `"none"`, `"hold_short"`, `"cleared_takeoff"`, `"departed"`, `"inbound"`, `"pattern"`, `"cleared_to_land"`, `"go_around"`, `"landed"`. |
| `atc.request_takeoff([airport_id]) → bool` | Queue this aircraft for departure. It is cleared onto the runway in turn. |
| `atc.request_landing([airport_id]) → bool` | Sequence this aircraft for arrival by distance to the threshold. |
| `atc.inbound([airport_id]) → bool` | Declare inbound (a radio call; the tower acknowledges). |
| `atc.scramble(airport_id, type_id, count) → bool` | Launch `count` AI departures of `type_id` from a named field. Returns false if the airport is unknown or no launcher is wired. |
| `atc.hold(airport_id, on) → bool` | Freeze (`true`) or release (`false`) that field's departure queue. |

```lua
-- A patrol that recovers to its home field when low on fuel.
function compute_control(state, tick, dt)
    if state.fuel_pct and state.fuel_pct < 15 and atc.clearance() == "none" then
        atc.request_landing("khjo")
    end
    -- ... fly toward the field; the ATC arrival composition takes over once cleared ...
    return {}
end
```

```lua
-- A mission/director script scrambling a CAP flight on an alert.
if alert_raised then atc.scramble("khjo", "fl-base:f16", 2) end
```
