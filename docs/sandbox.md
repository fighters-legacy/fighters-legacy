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

| Key / Input | Action |
|---|---|
| LMB drag | Orbit around pivot point |
| Scroll wheel | Zoom in / out |
| `=` / NumPad `+` | Zoom in |
| `-` / NumPad `-` | Zoom out |
| W / S | Pan forward / backward |
| A / D | Pan left / right |
| Q / E | Pan down / up |
| R | Reset camera to default position (500 m altitude) |

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

Active in all camera modes. Inputs are suppressed while the debug console is open.

| Key | Action |
|---|---|
| Left Shift | Throttle full |
| Arrow Up / Down | Elevator (pitch) |
| Arrow Left / Right | Aileron (roll) |
| Z / X | Rudder left / right |
| Space | Weapon trigger (bit 0) |

## Performance overlay (F3)

Cycles Off → Compact → Full. **Full mode** includes a 128-position rolling frame-time bar graph using Unicode shade characters (░ ▒ ▓ █ — U+2591/92/93/88). The renderer uses GNU Unifont 8×16 (full Unicode BMP), so these render correctly on all platforms without the CP437 fallback workaround previously used.

---

## Debug console

**Toggle:** `` ` `` (backtick / grave). **Close:** Escape.

The console is a half-screen drop-down overlay rendered over the HUD. All flight inputs are
suppressed while it is open.

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

The in-game clock advances at **10× real time** by default (1 real minute = 10 game minutes; full day/night cycle ≈ 2.4 real hours). Time is shown as `HH:MM` in the HUD top-right (cockpit mode only). The time scale is configurable via `[world] time_scale` in `server.toml`.

### Position widget

`toggle_pos` enables a persistent `X/Y/Z` readout in the top-right corner. It remains
visible in all camera modes even when the console is closed. Toggle it off with a second
`toggle_pos`.
