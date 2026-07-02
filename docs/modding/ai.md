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

See [Phase 4 planned bindings](#coming-in-phase-4-33) below for the richer `world.*` API coming
with the AI System milestone.

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
    local perr = guidance.pitch_error_from_alt(quat, alt - pos.y)
    return {
        aileron  = guidance.bank_to_turn_aileron(herr),
        rudder   = guidance.coordinated_rudder(guidance.bank_to_turn_aileron(herr)),
        elevator = guidance.elevator_from_pitch_error(perr),
        throttle = 0.65,
    }
end
```

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

---

## `guidance.*` module

Thin wrappers over the engine's `Guidance.h` inline math. The quaternion parameter is always
`{x,y,z,w}`; position parameters are `{x,y,z}` tables.

### `guidance.heading_error(quat, own_pos, target_pos) → number`

Signed horizontal angle in radians from the current heading to the bearing toward `target_pos`
in the XZ plane. Positive = target is to the right. Returns `0` when horizontal distance < 0.1 m.

### `guidance.pitch_error_from_alt(quat, alt_error_m) → number`

Signed pitch error in radians needed to close an altitude gap of `alt_error_m` metres.
Gain: 0.002 rad/m, clamped to ±30°.

### `guidance.bank_to_turn_aileron(heading_error_rad) → number`

Maps heading error to an aileron command. Gain: `2/π` (90° error → full deflection).

### `guidance.coordinated_rudder(aileron) → number`

Rudder command proportional to aileron (gain 0.3). Keeps the ball centred in a coordinated turn.

### `guidance.elevator_from_pitch_error(pitch_error_rad) → number`

Maps pitch error to an elevator command. Gain: `2/π`.

### `guidance.body_forward(quat) → {x, y, z}`

Extracts the world-frame forward vector (+X body axis) from the quaternion.

---

## `nearby_entities(cx, cz, radius_m) → array`

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

## Coming in Phase 4 (mission & campaign runtime epic, #584)

The mission & campaign runtime epic ([#584](https://github.com/fighters-legacy/fighters-legacy/issues/584);
`world.*` bindings [#413](https://github.com/fighters-legacy/fighters-legacy/issues/413), coroutines
[#412](https://github.com/fighters-legacy/fighters-legacy/issues/412)) will add richer engine
bindings in a `world.*` module. Planned APIs:

| Function | Description |
|----------|-------------|
| `world.spawn(type_id, pos, heading)` | Spawn an entity from a Lua script |
| `world.despawn(entity_id)` | Remove an entity |
| `world.set_music_state(state_id)` | Trigger music state transition (#166) |
| `world.set_relationship(a, b, rel)` | Change faction relationships |
| `world.on_trigger(predicate, cb)` | Register a per-tick condition callback |
| `world.timer(seconds, cb)` | One-shot timer callback |

These require a sim-thread → main-thread dispatch queue not yet implemented. Phase 4 will also
add first-class coroutine support so scripts can use `coroutine.yield()` between ticks for
sequential state machines.
