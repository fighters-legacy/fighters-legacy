# Controls

The complete key map, plus gamepad, HOTAS and rebinding. Every binding here is a **default** —
all of them can be changed in `config/bindings.toml`.

New to the game? [Quick start](quickstart.md) covers just enough to fly.

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
| G | Landing gear up / down (#639; latched, absolute on the wire). Position slews at the model's `gear_transit_s` and the drag ramps in with it, so the HUD reads `GEAR ...` mid-travel. **With the gear up, a ground contact is a belly slide** — no brakes, no tyre grip, no nosewheel steering |
| F | Flap detent: clean → manoeuvre → full → clean (#639). Lift, drag and stall AoA follow the POSITION as it travels |
| K | Speed brake (momentary — held, not latched; the one flight-configuration control that is not a switch) |
| H | Arresting hook up / down (#639; latched) |
| Shift + C | Canopy open / closed (#639; latched). Plain C is the wingman radio menu |
| B | Wheel brakes (bit 6, level; #700). Only bites in ground contact — hold it to stop the rollout after touchdown. Rudder also steers the nosewheel at taxi speed, fading out by ~50 m/s |
| Space | Gun trigger (bit 0, level — hold to keep firing, rate-limited server-side) |
| Tab | Afterburner command (bit 1) |
| Enter / Right mouse | Fire selected store (bit 2 — edge-detected server-side; holding it is one shot) |
| 1 / 2 | Cycle weapon station next / previous (local; the wire carries the absolute selection) |
| R | Cycle radar mode: Silent → Search → TWS → STT (#526; absolute on the wire). Drives the datalink scope + RWR |
| E | Dispense chaff + flare (#529; server edge-detects — a held key is one pop). Needs a dispenser with rounds |
| J | Toggle the ECM jammer (#529; denies a hostile radar a lock beyond its burn-through range) |
| End | Eject (#672; server edge-detects — a held key is one ejection). Spawns a parachute and destroys the aircraft; within the seat envelope the pilot survives |
| C | Open the wingman radio menu (#610) |
| T | Open the comms menu (#704). Non-modal like the wingman menu (the aircraft keeps flying). Digits `1`–`9` pick an item; `Escape` backs out a page, then closes. Root: `1` ATC, `2` Ground crew (#55). ATC page: request takeoff / landing / declare inbound / cancel. Ground crew page: **Refuel / Rearm / Repair** — server-authoritative, honoured only when the aircraft is shut down (stopped, on the ground) at a base: within a few km of an airport, or on a carrier deck. The reply appears as a subtitle (and a voice line when a pack provides `radio/` audio); an ineligible request gets a crew-chief refusal with the reason. After landing and stopping for a couple of seconds in the Chase view, the camera blends into a slow **ramp orbit** around the parked aircraft (the ground-crew scene); it clears on the takeoff roll, a camera-mode change, or after 60 s. |
| K | Crew seat picker (#975): open / cycle the joinable non-fly seats across every crewed aircraft the client knows |
| L | Join the selected crew seat (a gunner on that airframe; the bot parks) |
| U | Leave the current crew seat (become an observer) |
| Y | Open the chat input box on the **all** channel (#646). While it is open the keyboard is captured — type your line, `Enter` (or the Send button) sends, `Escape` cancels. The gamepad/HOTAS axes stay live. |
| H | Open the chat input box on the **team** channel (#646; reaches only your faction) |
| I | Hold to show the multiplayer scoreboard (#647). Auto-shown in the match end phase. |
| V | **Push to talk** on the selected radio net (Epic J, #531). HELD, never latched. |
| B | Push to talk on the **flight** net; wins if both PTT keys are held. |
| M | Cycle the primary PTT key through the server's net table (TEAM / FLIGHT / ATC / PROX by default). |

While the radio menu is open, Enter and the digit keys belong to the menu — the fire-store bit and
station cycling are suppressed, the flight axes and gun trigger stay live.

**Voice comms (Epic J).** The bottom-left HUD shows the net your PTT key will use, a `TX` indicator
whose brightness follows the live mic level (the first thing to check when nobody answers), and who
is currently on the air. Transmission is suppressed entirely while the console, the chat box or the
GM map has focus — a PTT key that also fired while you were typing would send your keystrokes'
worth of room noise to the whole team. With no microphone, no permission, or no Opus encoder the
client is simply listen-only; none of those is an error you have to resolve before flying. Full
reference: [voice.md](../developer/voice.md).

The crew seat picker (`K` / `L` / `U`) is **non-modal** like the radio menu — the aircraft keeps flying
while it is up. Only the `Fly` seat runs flight prediction and shows attitude; a gunner seat views the
host airframe without predicting its flight. A join that is denied (the seat is taken by another player,
or you named the pilot seat) is surfaced as a one-line message. Seat join/leave and the operator
`seats` / `set_seat` commands are the multi-crew surface for Epic #966.

**The sandbox aircraft is armed.** The builtin debug entity carries five stations: a 20 mm cannon
(station 1), two IR missiles (2–3) and two radar missiles (4–5) — all compiled-in "builtin:"
weapons, so the whole fire path works with zero content packs mounted. The HUD's right column
shows the selection as `ARM <weapon> x<rounds>`; the default selection is the first IR rail
("selected" means the stores — the gun has its own trigger).

## Camera modes

The camera-mode keys are now rebindable `InputAction`s (`CameraCockpit`/`CameraChase`/`CameraFree`, #689) resolved through `config/bindings.toml`, and work from a gamepad; the defaults below preserve the old behaviour. The console toggle (`` ` ``) and the performance overlay (F3) stay raw — they are not bound actions.

| Key | Action | Binding |
|---|---|---|
| F1 | Cockpit — camera locked to player entity | `CameraCockpit` |
| F2 | Chase — orbit behind player entity | `CameraChase` |
| F4 | Free (default) — freely movable pivot camera | `CameraFree` |
| F5 | Padlock — slew to keep the designated target centered; auto-picks best-in-cone if nothing is designated, breaks lock with a `PADLOCK — BREAK` cue when terrain or the airframe masks the target, and reverts to Cockpit after a 4 s reacquire window (#697) | `PadlockToggle` (gamepad: RightStick click) |
| F6 | Toggle the target-slaved inset view (#698) | `TargetInsetToggle` |
| F7 | Toggle night-vision goggles — a green photocathode gain applied at the tonemap stage; brightens dim night scenes (#210) | `NvgToggle` |
| M | Toggle the game-master overview map (#861) — the whole-battlespace top-down map. Only functional for a peer granted the `gm_map` capability (see `grant` below). While open: ← / → / ↑ / ↓ pan, `-` / `=` zoom, left-click selects an entity, and the side panel issues orders / drops into the entity's view. | `GmMap` |
| N / P | Cycle to the next / previous target (#696) | `NextTarget` / `PrevTarget` (gamepad: DpadUp = next) |
| PageUp / PageDown / ← / → | Pan the cockpit view (keyboard alternative to RMB drag; Cockpit/Padlock only) | `ViewUp` / `ViewDown` / `ViewLeft` / `ViewRight` |
| F3 | Cycle performance overlay (Off → Compact → Full) | raw |
| `` ` `` | Toggle game console | raw |

> Note (#689): the default `ViewLeft`/`ViewRight` keys (← / →) also drive the legacy raw aileron in `FlightInputCollector`, so in Cockpit view an arrow both rolls the aircraft and pans the view. RMB drag is the primary look control; rebind `ViewLeft`/`ViewRight` in `config/bindings.toml` if the double duty is unwanted.

### Autopilot (#640)

Client-side hold modes, shaped over your input before it is sent (the server stays authoritative). Toggle in Cockpit/Padlock view with no menu or console open; any stick input past a small threshold disengages the attitude holds, and moving the throttle disengages speed hold. The engaged holds and their captured targets are annunciated on the HUD (`AP ALT… HDG… SPD…`).

| Key | Action | Binding |
|---|---|---|
| F9 | Altitude hold (captures current altitude) | `AutopilotAltHold` |
| F10 | Heading hold (captures current heading) | `AutopilotHdgHold` |
| F11 | Speed hold (captures current airspeed) | `AutopilotSpdHold` |

### Combat HUD (#641)

The combat symbology renders in Cockpit/Padlock view against the designated target (#696): an IFF-coloured designator box with range + closure, a gun pipper with ballistic lead when a gun station is selected, a CCIP impact cross + fall line for bombs, and a lower-right weapon-status block. **V** toggles master arm — SAFE really suppresses the fire triggers (not just a HUD label).

| Key | Action | Binding |
|---|---|---|
| V | Master arm (ARM / SAFE) | `MasterArm` |
| O | Cycle radar MFD page (Off → PPI → B-scope → RWR) | `MfdPage` |
| 3 | Cycle radar MFD range (10 / 20 / 40 / 80 nm) | `MfdRange` |

The MFD (lower-left) presents the fused datalink picture (#528): the PPI is a 360° plan view (ownship centred, nose up), the B-scope plots azimuth (±60°) vs range, and the RWR page is a dedicated threat-warning ring. All are IFF-coloured (friend green / foe red / unknown amber) and annunciate the requested radar mode (SIL/SRCH/TWS/STT, cycled with **R**). An RWR `RWR LAUNCH` / `RWR LOCK` caption shows on **every** page, including Off.

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

### Observer / ghost camera (`--observer`)

Joining with `--observer` (#851/#859) spawns no aircraft: you free-fly a ghost camera and oversee the
world instead of flying it. Terrain streams around the camera, and the camera eye you move drives the
server's interest management, so entities appear around wherever you look.

| Key | Action |
|---|---|
| Num1 | Select / cycle to the **next** live entity (by index); the first pick jumps into Chase |
| Num2 | Select / cycle to the **previous** live entity |
| F1 / F2 | View the selected entity in Cockpit / Chase |
| F4 | Return to the free ghost camera |

The selected entity is labelled top-centre by type name and faction (e.g. `[ F-16C | Blue ]`). If the
entity is destroyed or leaves view, the camera degrades gracefully back to free-fly.

---

## Menu navigation

| Action | Keyboard | Gamepad |
|---|---|---|
| Navigate up / down | Up / Down arrows or W / S | Left-stick Y or D-pad Up / Down |
| Confirm / select | Enter or Space | A button |
| Back / cancel | Escape | B button |
| Pause (in-flight) | Escape (when console is closed) | — |

---

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

Button bindings for `FireWeapon`, `FireMissile` (fire selected store), `NextWeapon` / `PrevWeapon`
(station cycling — D-pad right / left by default), and `Afterburner` are configured in the `[alt]`
section of `config/bindings.toml` (see the **bindings.toml** section below).

## HOTAS controls

HOTAS sticks, throttle quadrants, and rudder pedals are supported via the raw joystick API on
all platforms. Windows and macOS work without additional device setup; Linux users may need udev
rules for device permissions (see [docs/user-guide/gamepad-linux.md](../user-guide/gamepad-linux.md)).

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

See [docs/developer/haptics.md](../developer/haptics.md) for full tuning values and platform notes.

## Client display settings

Configure in the `[client]` section of `config/user.toml`:

| Key | Default | Range | Description |
|---|---|---|---|
| `motd_display_s` | `15` | 0–3600 | Client fallback for MOTD banner display duration (seconds); overridden per-connection when the server specifies a non-zero `[server].motd_display_s`; banner fades out over the final 2 s of the window; `0` = persistent (no fade, no auto-dismiss) |
| `operator_password` | `""` | any string | Operator password for admin console commands when connecting with `--connect`. CLI `--operator-password` arg and `FL_OPERATOR_PASSWORD` env var take precedence. |

## HUD settings

Configure in the `[hud]` section of `config/user.toml`:

| Key | Default | Range | Description |
|---|---|---|---|
| `show_latency` | `true` | bool | Show per-peer latency indicator (`42 ms`) in the cockpit HUD. Hidden automatically when latency is zero (single-player localhost). |

