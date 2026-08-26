# Debug console

The in-game developer console, the performance overlay, and the visual-verification harness.

Player-facing controls live in the [User Guide](../user-guide/controls.md); this page is the
developer surface.

## Game console

**Toggle:** `` ` `` (backtick / grave). **Close:** Escape.

The console is a half-screen drop-down overlay. It is fully independent of the cockpit HUD and available in any game state. All game inputs (flight controls and camera) are suppressed while it is open; throttle is held at its last value so opening the console does not cut the engines.

### Editing

| Key | Action |
|---|---|
| Backspace | Delete last character |
| Up arrow | Recall previous command |
| Down arrow | Step forward in history |
| Enter | Submit command |

### Commands

| Command | Description |
|---|---|
| `help [command]` | List all commands, or show usage for one |
| `types` | List all registered entity types with their indices |
| `entities` | List all live entities (idx/gen, type, world position) |
| `spawn <type> <x> <y> <z> [--faction <n>] [--ai <behavior> [args...]]` | Spawn entity with an optional faction/team (`n` = 0–65535; 0 = neutral) and AI controller (see AI behaviors below) |
| `kill <idx>` | Remove entity from simulation (queued to sim thread) |
| `tp <x> <y> <z>` | Teleport player entity to world position |
| `toggle_pos` | Toggle entity world-position readout below the camera position display |
| `show_ping` | Toggle "Ping: N ms" RTT overlay (visible even when F3 performance overlay is off) |
| `set_weather <preset>` | Set weather instantly: `clear`, `partly_cloudy`, `overcast`, `rain`, `storm`, `snow`, `blizzard`. Queued to sim thread; takes effect on next tick. |
| `atc_status [airport]` | Show ATC facility queues + runway occupancy (#705). Synchronous read of the ATC service; `[airport]` filters to one field. |
| `atc_scramble <airport> <type> [count]` | Launch `count` AI departures from a named airport (#705, default 1). They spawn hold-short, get sequenced onto the runway, and take off in order. Queued to the sim thread. |
| `atc_hold <airport> <on\|off>` | Freeze or release that airport's departure queue (#705). Queued to the sim thread. |
| `detonate <x> <y> <z> <radius_m> <damage> [--nuclear]` | AoE warhead at a world position (#356); `--nuclear` adds the EMP ring (avionics kill) at 4× the blast radius. Forwarded to the server. |
| `set_difficulty <level>` | *(stub — Phase 2b)* |
| `reload_content` | *(stub — see issue #152)* |

`spawn`, `kill`, and `set_weather` are queued to the sim thread and take effect on the next tick.
Entity indices shown by `entities` come from the most-recent render snapshot.

**AI behaviors** (optional `--ai` flag on `spawn`):

> **Every AI behaviour is sensing-gated (#670).** A spawned AI engages only what its sensors have
> actually detected. A bandit behind a forward-looking aircraft is **invisible** to it; a fresh
> contact takes a **reaction delay** to act on (scaled by the server's `[ai] difficulty` and the
> entity's own `[ai].reaction`); and a target that breaks the lock keeps being flown at its
> **last-known** position until the track's `lock_hold_s` coast runs out.
>
> So if a spawned AI seems to be ignoring you, first check whether it can *see* you. That is now a
> real question with a real answer, rather than a bug.


| Behavior | Args | Description |
|---|---|---|
| `loiter` | `[cx cy cz] [radius_m] [alt_m] [throttle] [cw\|ccw]` | Orbit a fixed center point; `cw` = clockwise (default), `ccw` = counterclockwise. `radius_m` is a **floor**: the orbit opens up to the tightest circle the aircraft can turn at the speed it is flying (#1340) |
| `dynamic_loiter` | `<entityIdx> [radius_m] [throttle] [cw\|ccw]` | Orbit a **moving** entity (#464): re-centers the loiter circle on the target's live position each tick and matches its altitude; returns neutral when the target is dead or invalid |
| `waypoint` | `x1 y1 z1 [x2 y2 z2 ...] [--loop]` | Fly a sequence of 3D waypoints; `--loop` restarts from the first when complete |
| `pursuit` | `<entityIdx>` | Pursue an entity by pool index; returns neutral when target is dead or invalid |
| `evade` | `<entityIdx>` | Flee a threat entity by inverting the pursuit heading error |
| `break` | `<entityIdx> [rollDuration]` | Defensive ACM: roll toward threat then pull maximum-G (rollDuration in seconds, default 0.5) |
| `lead` | `<entityIdx> [navGain]` | Proportional navigation pursuit; aims at predicted intercept point ahead of target (navGain: 0.0=pure pursuit, 1.0=first-order lead [default]) |
| `lag` | `<entityIdx> [lagFraction]` | Lag pursuit; aims behind the target at `target.pos − target.vel × TTC × lagFraction`; keeps the attacker inside the target's turn circle without overshooting (lagFraction: 0.0=pure pursuit, 1.0=one TTC-step behind [default]) |
| `immelmann` | `[pullDur] [rollDur]` | Half-loop + roll to reverse heading; pull up to inverted then roll upright (defaults: 4.0 s, 1.5 s) |
| `split_s` | `[rollDur] [pullDur]` | Roll inverted + pull through to reverse heading; opposite energy trade to Immelmann (defaults: 1.5 s, 4.0 s) |
| `high_yo_yo` | `<entityIdx> [climbDur] [reacquireDur]` | Overshoot correction: bank away from target, pull up to bleed speed, then reacquire (defaults: 2.5 s, 3.0 s) |
| `low_yo_yo` | `<entityIdx> [diveDur] [pullDur]` | Dive-and-cut-corner to close on a turning target (defaults: 1.5 s, 2.5 s) |
| `guns` | `<entityIdx> [muzzleVel] [lethalRadius]` | Guns employment (#462): steers onto the ballistic lead point (muzzle velocity, shooter-velocity carry, gravity drop) and fires only when the predicted miss is inside the lethal radius (defaults: 1030 m/s, 8 m) |
| `ballistic` | `<tx> <ty> <tz> [mirvCount [spreadM]]` | Ballistic missile guidance (#355) for `type = "ballistic"` entities: boost-phase TVC steering to the impact point with a lofted pitch program, inertial after burnout; `mirvCount > 0` deploys child RVs past apogee (kills credit whoever launched the bus) |
| `lua` | `<script_name>` | Load a Lua AI script from the content pack's `ai/` directory (e.g. `patrol`, `interceptor`). See `docs/modding/ai.md`. |
| `patrol_attack` | `<entityIdx> [engageRangeM] [retreatHp]` | Three-state machine: loiter patrol → lead-pursuit engage when the target is **detected** within range → evade retreat when HP below threshold (defaults: engageRangeM=8000 m, retreatHp=0.25). **Sensing-gated (#690):** it engages what it has actually seen and reacted to, not whatever is within the radius |
| `escort` | `<entityIdx> [standoffM]` | Two-state orbit protection: clockwise loiter at standoffM radius around the escorted entity, **tracking it as it moves** (#464) → Immelmann reversal when a **hostile** entity enters the inner defense zone (standoffM×0.5). Hostiles are classified by faction, so the escort and escortee should be spawned with the same non-neutral `--faction`; friendlies and neutrals are ignored. (default: standoffM=2000 m) |
| `swarm` | `<cx> <cy> <cz> [neighborRadiusM] [separationRadiusM] [cruiseThrottle]` | Boids swarm member (#353): separation / alignment / cohesion with **same-type, same-faction** flockmates found through the spatial index, migrating toward the point. Spawn N identical entities with this behavior and they flock — there is no swarm object, so losing members degrades the flock, never breaks it. (defaults: 600 m, 120 m, 0.75) |
| `swarm_follow` | `<entityIdx> [neighborRadiusM] [separationRadiusM] [cruiseThrottle]` | The same boids member, migrating after a **moving** anchor entity (e.g. a strike lead the swarm escorts) |

**Weather presets:**

| Preset | Cloud cover | Fog | Turbulence | Time of day | Precipitation |
|---|---|---|---|---|---|
| `clear` | 0% | None | None | Driven by time clock | None |
| `partly_cloudy` (default) | 35% | None | Light | Driven by time clock | None |
| `overcast` | 75% | Light | Moderate | Driven by time clock | Rain |
| `rain` | 85% | Heavy | Moderate | Driven by time clock | Rain |
| `storm` | 95% | Maximum | Strong | Driven by time clock | Heavy rain |
| `snow` | 85% | Moderate | Moderate | Driven by time clock | Snow (any altitude) |
| `blizzard` | 95% | Heavy | Strong | Driven by time clock | Heavy snow (any altitude) |

When `cloudCoverage ≥ 0.75` (overcast, rain, storm, snow, blizzard), precipitation particles emit from a 3×3 grid 60 m above the camera. The precipitation type is server-authoritative: `snow`/`blizzard` presets always emit snow particles regardless of altitude; `overcast`/`rain`/`storm` presets always emit rain particles. With no wind, particles fall straight down. Rain uses a 20° cone and 10%/25% wind influence; snow uses an 80° cone and 35%/55% wind influence.

In **Cockpit mode (F1)**, a screen-space windshield overlay is rendered simultaneously: 48 semi-transparent streaks animate on the glass — blue-white diagonal lines for `rain`/`storm`, short white smears for `snow`/`blizzard`. Streak opacity and length scale with `cloudCoverage`. Lateral lean is proportional to crosswind speed (`windX`).

The in-game clock advances at **10× real time** by default (1 real minute = 10 game minutes; full day/night cycle ≈ 2.4 real hours). The Cockpit HUD (F1 mode) shows **IAS / ALT / AGL** on the left column, **THR / FUEL** on the right column, **HDG** at the bottom, and `HH:MM` clock top-right. AGL is computed from the terrain heightmap at the aircraft's XZ position and falls back to the same value as ALT (MSL) when the LOD-0 chunk is not yet loaded. The time scale is configurable via `[world] time_scale` in `server.toml`.

### Position widget

The camera world position (`CAM x y z`) is always displayed in the top-right corner in all
camera modes. `toggle_pos` adds a second line showing the player entity position (`ENT x y z`)
below it; toggle it off with a second `toggle_pos`.

## Performance overlay (F3)

Cycles Off → Compact → Full. **Full mode** includes a 128-position rolling frame-time bar graph using Unicode shade characters (░ ▒ ▓ █ — U+2591/92/93/88). The renderer uses GNU Unifont 8×16 (full Unicode BMP), so these render correctly on all platforms without the CP437 fallback workaround previously used.

---

## Visual verification (`tools/visual_check.sh`)

One command opens a window rendering the `builtin:shape-gallery` mission — a museum row with one
entity of every category (ground vehicle, naval vessel, structure), floating ordnance exhibits
(missile / bomb / rocket projectile types spawned as plain objects), an armed joinable player
slot, and a live fight 9 km out (fighters + SAM + AAA) so missiles fly. No menu interaction:

    tools/visual_check.sh            # observer mode: standalone server + ghost camera; wreck-
                                     # staging detonations fire after ~25 s so you watch the
                                     # intact -> wreck swap live (FL_VISUAL_STAGE_DELAY=0 disables)
    tools/visual_check.sh --fly      # pilot mode: single-player into the armed player slot —
                                     # fire bombs/rockets (stations 4/5), strafe the museum row
    tools/visual_check.sh --build    # build the debug preset first

In observer mode use **Num1/Num2** to cycle entities (labels show type + faction), **F2/F1** to
frame the pick in Chase/Cockpit, **F4** to return to free-fly. Windows: `tools\visual_check.ps1`
with `-Fly` / `-Build`. Any mission id works via `--mission <id>`, including a `.yaml` file path
(fl-server resolves builtin id → file path → pack asset).

The loading screen reports specific connection failures immediately rather than waiting for the 10-second timeout:

| Message | Cause | Returns to |
|---|---|---|
| `Server binary not found.` | `fl-server` executable not found at startup | Main menu after 3 s |
| `Port already in use.` | `fl-server` could not bind to the chosen port | Main menu after 3 s |
| `Server startup timed out.` | `fl-server` started but never became ready | Main menu after 3 s |
| `Server version mismatch.` | Server sent `MsgHello` with a different `protocolVersion` | Main menu after 3 s |
| `Connection refused by server.` | Server dropped the ENet connection before accepting the client (ban, allowlist, rate limit) | Main menu after 3 s |
| `The server denied the requested role.` | Requested `--observer` but the server has `allow_observers = false` (#857) | Main menu after 3 s |
| `Connection timed out.` | No response from server within 10 s | Main menu after 3 s |
| `Local server failed to start.` | `fl-server` process hung and never became ready within 10 s (fallback) | Main menu after 3 s |
