# Sandbox Reference

Developer controls for the Fighters Legacy sandbox (zero-content-pack free-flight mode).

The game opens to the main menu. Select **Sandbox (Instant Action)** to start a local server and enter free flight. The loading screen shows progress messages while the server starts; it transitions to flight automatically when connected. If the server or connection fails, the loading screen displays a specific failure message and returns to the main menu after 3 seconds (see the failure message table in the Multiplayer CLI section below). Press **Escape** during flight to open the pause menu.

---

## Menu navigation

| Action | Keyboard | Gamepad |
|---|---|---|
| Navigate up / down | Up / Down arrows or W / S | Left-stick Y or D-pad Up / Down |
| Confirm / select | Enter or Space | A button |
| Back / cancel | Escape | B button |
| Pause (in-flight) | Escape (when console is closed) | — |

---

## Camera modes

| Key | Action |
|---|---|
| F1 | Cockpit — camera locked to player entity |
| F2 | Chase — orbit behind player entity |
| F4 | Free (default) — freely movable pivot camera |
| F3 | Cycle performance overlay (Off → Compact → Full) |
| `` ` `` | Toggle game console |

### Free camera (F4)

When switching to Free camera while a player entity exists, the pivot snaps to the entity's position so it is immediately in view.

| Key / Input | Action |
|---|---|
| LMB drag | Orbit around pivot point |
| Scroll wheel | Zoom in / out |
| `=` / NumPad `+` | Zoom in |
| `-` / NumPad `-` | Zoom out |
| W / S | Pan forward / backward |
| A / D | Pan left / right |
| Q / E | Pan down / up (clamped to terrain surface + 2 m) |
| R | Reset camera pivot to player entity position (or world origin if no entity) |

### Chase camera (F2)

| Key / Input | Action |
|---|---|
| LMB drag | Orbit around player entity |
| Scroll wheel | Adjust orbit radius |

### Cockpit camera (F1)

| Key / Input | Action |
|---|---|
| RMB drag | Look offset (yaw / pitch from forward) |

---

## Flight controls

Active in all camera modes. All game inputs (flight controls and camera) are suppressed while the game console is open; throttle is held at its last value.

| Key | Action |
|---|---|
| Page Up | Throttle increase (~1 s to 100% at 60 Hz) |
| Page Down | Throttle decrease |
| Left Shift | Max throttle hold (override while held) |
| Arrow Up / Down | Elevator (pitch) |
| Arrow Left / Right | Aileron (roll) |
| Z / X | Rudder left / right |
| Space | Weapon trigger (bit 0) |
| Tab | Afterburner command (bit 1) |

## Gamepad controls

Standard gamepads (Xbox / PlayStation) are supported in all camera modes. A joystick axis
overrides the corresponding keyboard control when the axis value exceeds the deadzone.
Keyboard controls remain active when no gamepad is connected or all axes are within the
deadzone. Deadzone, response curve, inversion, and axis mapping are configured in
`config/bindings.toml` — see the **bindings.toml** section below.

| Axis | Default mapping |
|---|---|
| Throttle | Left trigger — absolute position [0, 1] |
| Elevator (pitch) | Right stick Y |
| Aileron (roll) | Right stick X |
| Rudder (yaw) | Left stick X |

| Index | Button | Default mapping |
|---|---|---|
| 5 | Right shoulder (RB / R1) | Primary fire |
| 4 | Left shoulder (LB / L1) | Afterburner |

Configure fire and afterburner buttons in the `[controls]` section of `config/user.toml` (axis deadzone/invert/mapping moved to `bindings.toml`):

| Key | Default | Description |
|---|---|---|
| `fire_button` | `5` | Gamepad button index for primary fire; see Buttons table above |
| `afterburner_button` | `4` | Gamepad button index for afterburner; see Buttons table above |

## `config/bindings.toml`

Generated at `<user data>/config/bindings.toml` on first run. Contains three sections:

### `[axis_config]`

Per-axis deadzone, response curve, inversion, and scale for all 6 gamepad axes. Defaults:

| Axis | deadzone | curve | invert | scale |
|---|---|---|---|---|
| `LeftX` (rudder) | `0.1` | `"Linear"` | `false` | `1.0` |
| `LeftY` | `0.1` | `"Linear"` | `false` | `1.0` |
| `RightX` (aileron) | `0.1` | `"Linear"` | `false` | `1.0` |
| `RightY` (elevator) | `0.1` | `"Linear"` | `false` | `1.0` |
| `TriggerLeft` (throttle) | `0.1` | `"Linear"` | `false` | `1.0` |
| `TriggerRight` | `0.1` | `"Linear"` | `false` | `1.0` |

- **deadzone**: axis magnitude below this maps to 0.0 (clamped to [0, 1]).
- **curve**: `"Linear"` passes through; `"Cubic"` applies a cubic ease-in (reduces sensitivity near centre).
- **invert**: `true` flips the axis sign. Note: `invert` is not meaningful for `TriggerLeft` (a unipolar [0, 1] axis); use the HOTAS `hotas_invert_throttle` path instead.
- **scale**: output multiplier applied after curve (default `1.0`).

### `[alt]`

Controls which physical axis or button handles each flight action on the gamepad. To remap elevator to the left stick Y axis:
```toml
[alt]
PitchAxis = { source = "GamepadAxis", id = "LeftY", negative = false }
```
A restart is required to apply changes.

### `[primary]`

Keyboard and mouse key table. Parsed and stored, but not yet acted on by the input collector (Phase 4 key-remapping).

## Haptic feedback

Controllers that support rumble receive feedback for the following flight events. Capability is
checked automatically via `supportsRumble` / `supportsTriggerRumble`; controllers without motors
silently skip all effects.

| Event | Motors | Duration |
|---|---|---|
| Gun burst | Right (high-freq) | 80 ms per trigger pull |
| Hit taken | Both | 120 ms |
| Stall buffet | Both (low-intensity) | Continuous while above stall AoA |
| Afterburner ignition | Both | 300 ms ramp, then low sustain |
| Engine failure | Both (low-freq) | Continuous while `engineFailFlags != 0` |
| G-LOC onset | Right (high-freq) | Continuous, proportional above 6 G |
| Transonic buffet | Both | 400 ms, periodic while Mach 0.85–1.05 |
| GPWS / terrain warning | Both | 2 × 100 ms double-pulse |
| Landing gear touchdown | Both (low-freq) | 200 ms impact |

Entry points for future game systems (not yet wired): missile launch, missile warning,
compressor stall, carrier trap, hydraulic failure, and ordnance release
(`HapticController::notify*` methods in `game/fighters-legacy/HapticController.h`).

See [docs/haptics.md](haptics.md) for full tuning values and platform notes.

## HOTAS controls

HOTAS sticks, throttle quadrants, and rudder pedals are supported via the raw joystick API on
all platforms. Windows and macOS work without additional device setup; Linux users may need udev
rules for device permissions (see [docs/linux-gamepad.md](linux-gamepad.md)).

Axis assignments default to a standard HOTAS layout and are configurable per device. A HOTAS
axis overrides the corresponding keyboard or gamepad control when active; inactive HOTAS axes
leave keyboard/gamepad values untouched.

**Throttle axis mapping:** the throttle axis reports full travel as `[-1, 1]`; this is remapped
to `[0, 1]` automatically. Keyboard Page Up / Page Down and the gamepad trigger remain active
when the HOTAS throttle axis is disabled (`hotas_throttle_axis = -1`).

| Default axis index | Mapping |
|---|---|
| 0 | Aileron (roll) |
| 1 | Elevator (pitch) |
| 2 | Throttle |
| 3 | Rudder (yaw) |

Configure in the `[controls]` section of `config/user.toml`:

| Key | Default | Description |
|---|---|---|
| `hotas_aileron_axis` | `0` | Axis index → aileron; -1 to disable |
| `hotas_elevator_axis` | `1` | Axis index → elevator; -1 to disable |
| `hotas_throttle_axis` | `2` | Axis index → throttle; -1 to disable |
| `hotas_rudder_axis` | `3` | Axis index → rudder; -1 to disable |
| `hotas_deadzone` | `0.05` | Center deadzone for stick and pedal axes (not applied to throttle) |
| `hotas_invert_pitch` | `false` | Flip elevator axis |
| `hotas_invert_roll` | `false` | Flip aileron axis |
| `hotas_invert_rudder` | `false` | Flip rudder axis |
| `hotas_invert_throttle` | `false` | Flip throttle direction |

## Performance overlay (F3)

Cycles Off → Compact → Full. **Full mode** includes a 128-position rolling frame-time bar graph using Unicode shade characters (░ ▒ ▓ █ — U+2591/92/93/88). The renderer uses GNU Unifont 8×16 (full Unicode BMP), so these render correctly on all platforms without the CP437 fallback workaround previously used.

---

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
| `spawn <type> <x> <y> <z>` | Spawn entity by id or numeric index at world position |
| `kill <idx>` | Remove entity from simulation (queued to sim thread) |
| `tp <x> <y> <z>` | Teleport player entity to world position |
| `toggle_pos` | Toggle entity world-position readout below the camera position display |
| `set_weather <preset>` | Set weather instantly: `clear`, `partly_cloudy`, `overcast`, `rain`, `storm`. Queued to sim thread; takes effect on next tick. |
| `set_difficulty <level>` | *(stub — Phase 2b)* |
| `reload_content` | *(stub — see issue #152)* |

`spawn`, `kill`, and `set_weather` are queued to the sim thread and take effect on the next tick.
Entity indices shown by `entities` come from the most-recent render snapshot.

**Weather presets:**

| Preset | Cloud cover | Fog | Turbulence | Time of day | Precipitation |
|---|---|---|---|---|---|
| `clear` | 0% | None | None | Driven by time clock | None |
| `partly_cloudy` (default) | 35% | None | Light | Driven by time clock | None |
| `overcast` | 75% | Light | Moderate | Driven by time clock | Rain < 2000 m / Snow ≥ 2000 m |
| `rain` | 85% | Heavy | Moderate | Driven by time clock | Rain < 2000 m / Snow ≥ 2000 m |
| `storm` | 95% | Maximum | Strong | Driven by time clock | Heavy rain < 2000 m / Heavy snow ≥ 2000 m |

When `cloudCoverage ≥ 0.75` (overcast, rain, storm), precipitation particles emit from a 3×3 grid 60 m above the camera. Below 2000 m, rain (`rain`/`storm_rain`) is emitted with a narrow 20° cone and 10%/25% wind influence. Above 2000 m, snow (`snow`/`storm_snow`) is emitted with an 80° cone (wide drifting spread) and 35%/55% wind influence. With no wind the particles fall straight down.

In **Cockpit mode (F1)**, a screen-space windshield overlay is rendered simultaneously: 48 semi-transparent streaks animate on the glass — blue-white diagonal lines for rain (below 2000 m), short white smears for snow (at or above 2000 m). Streak opacity and length scale with `cloudCoverage`. Lateral lean is proportional to crosswind speed (`windX`).

The in-game clock advances at **10× real time** by default (1 real minute = 10 game minutes; full day/night cycle ≈ 2.4 real hours). The Cockpit HUD (F1 mode) shows **IAS / ALT / AGL** on the left column, **THR / FUEL** on the right column, **HDG** at the bottom, and `HH:MM` clock top-right. AGL is computed from the terrain heightmap at the aircraft's XZ position and falls back to the same value as ALT (MSL) when the LOD-0 chunk is not yet loaded. The time scale is configurable via `[world] time_scale` in `server.toml`.

### Position widget

The camera world position (`CAM x y z`) is always displayed in the top-right corner in all
camera modes. `toggle_pos` adds a second line showing the player entity position (`ENT x y z`)
below it; toggle it off with a second `toggle_pos`.

## Client display settings

Configure in the `[client]` section of `config/user.toml`:

| Key | Default | Range | Description |
|---|---|---|---|
| `motd_display_s` | `15` | 0–3600 | Client fallback for MOTD banner display duration (seconds); overridden per-connection when the server specifies a non-zero `[server].motd_display_s`; banner fades out over the final 2 s of the window; `0` = persistent (no fade, no auto-dismiss) |
| `operator_password` | `""` | any string | Operator password for admin console commands when connecting with `--connect`. CLI `--operator-password` arg and `FL_OPERATOR_PASSWORD` env var take precedence. |

## Multiplayer connection

Pass `--connect` to join a remote `fl-server` instead of spawning a local single-player session.

| Flag | Description |
|---|---|
| `--connect <host[:port]>` | Connect to a remote fl-server. Port defaults to `4778` if omitted. IPv6 literals must be bracketed: `--connect [::1]:4778`. |
| `--operator-password <pw>` | Operator password for admin console commands on the remote server. Enables the in-game console commands (`spawn`, `kill`, `tp`, etc.) over the network. Takes precedence over the env var and user.toml. |

To avoid exposing the password in the process listing, use the `FL_OPERATOR_PASSWORD` environment variable instead of the CLI flag. Merge precedence: `--operator-password` CLI arg > `FL_OPERATOR_PASSWORD` env var > `[client].operator_password` in user.toml.

When `--connect` is given the main menu shows **Join Server** instead of **Sandbox (Instant Action)**, and the loading screen displays "Connecting to remote server…".

The loading screen reports specific connection failures immediately rather than waiting for the 10-second timeout:

| Message | Cause | Returns to |
|---|---|---|
| `Server binary not found.` | `fl-server` executable not found at startup | Main menu after 3 s |
| `Port already in use.` | `fl-server` could not bind to the chosen port | Main menu after 3 s |
| `Server startup timed out.` | `fl-server` started but never became ready | Main menu after 3 s |
| `Server version mismatch.` | Server sent `MsgHello` with a different `protocolVersion` | Main menu after 3 s |
| `Connection refused by server.` | Server dropped the ENet connection before accepting the client (ban, allowlist, rate limit) | Main menu after 3 s |
| `Connection timed out.` | No response from server within 10 s | Main menu after 3 s |
| `Local server failed to start.` | `fl-server` process hung and never became ready within 10 s (fallback) | Main menu after 3 s |
