# Sandbox Reference

Developer controls for the Fighters Legacy sandbox (zero-content-pack free-flight mode).

---

## Camera modes

| Key | Action |
|---|---|
| F1 | Cockpit — camera locked to player entity |
| F2 | Chase — orbit behind player entity |
| F4 | Free (default) — freely movable pivot camera |
| F3 | Cycle performance overlay (Off → Compact → Full) |
| `` ` `` | Toggle debug console |

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
| R | Reset camera pivot to world origin (clamped above terrain) |

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

Active in all camera modes. All game inputs (flight controls and camera) are suppressed while the debug console is open; throttle is held at its last value.

| Key | Action |
|---|---|
| Page Up | Throttle increase (~1 s to 100% at 60 Hz) |
| Page Down | Throttle decrease |
| Left Shift | Max throttle hold (override while held) |
| Arrow Up / Down | Elevator (pitch) |
| Arrow Left / Right | Aileron (roll) |
| Z / X | Rudder left / right |
| Space | Weapon trigger (bit 0) |

## Gamepad controls

Standard gamepads (Xbox / PlayStation) are supported in all camera modes. A joystick axis
overrides the corresponding keyboard control when the axis value exceeds the deadzone
(default 0.05). Keyboard controls remain active when no gamepad is connected or all axes
are within the deadzone.

| Axis | Default mapping |
|---|---|
| Throttle | Left trigger — absolute position [0, 1] |
| Elevator (pitch) | Right stick Y |
| Aileron (roll) | Right stick X |
| Rudder (yaw) | Left stick X |

Configure in the `[controls]` section of `config/user.toml`:

| Key | Default | Description |
|---|---|---|
| `gamepad_deadzone` | `0.05` | Minimum axis magnitude before input is registered |
| `invert_pitch` | `false` | Flip elevator axis |
| `invert_roll` | `false` | Flip aileron axis |
| `invert_rudder` | `false` | Flip rudder axis |
| `invert_throttle` | `false` | Flip throttle direction |

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

## Debug console

**Toggle:** `` ` `` (backtick / grave). **Close:** Escape.

The console is a half-screen drop-down overlay rendered over the HUD. All game inputs (flight controls and camera) are suppressed while it is open; throttle is held at its last value so opening the console does not cut the engines.

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
| `toggle_pos` | Toggle world-position readout in top-right corner (all camera modes) |
| `set_weather <preset>` | Set weather instantly: `clear`, `partly_cloudy`, `overcast`, `rain`, `storm`. Queued to sim thread; takes effect on next tick. |
| `set_difficulty <level>` | *(stub — Phase 2b)* |
| `reload_content` | *(stub — see issue #152)* |

`spawn`, `kill`, and `set_weather` are queued to the sim thread and take effect on the next tick.
Entity indices shown by `entities` come from the most-recent render snapshot.

**Weather presets:**

| Preset | Cloud cover | Fog | Turbulence | Time of day |
|---|---|---|---|---|
| `clear` | 0% | None | None | Driven by time clock |
| `partly_cloudy` (default) | 35% | None | Light | Driven by time clock |
| `overcast` | 75% | Light | Moderate | Driven by time clock |
| `rain` | 85% | Heavy | Moderate | Driven by time clock |
| `storm` | 95% | Maximum | Strong | Driven by time clock |

The in-game clock advances at **10× real time** by default (1 real minute = 10 game minutes; full day/night cycle ≈ 2.4 real hours). The Cockpit HUD (F1 mode) shows **IAS / ALT / AGL** on the left column, **THR / FUEL** on the right column, **HDG** at the bottom, and `HH:MM` clock top-right. AGL is computed from the terrain heightmap at the aircraft's XZ position and falls back to the same value as ALT (MSL) when the LOD-0 chunk is not yet loaded. The time scale is configurable via `[world] time_scale` in `server.toml`.

### Position widget

`toggle_pos` enables a persistent `X/Y/Z` readout in the top-right corner. It remains
visible in all camera modes even when the console is closed. Toggle it off with a second
`toggle_pos`.
