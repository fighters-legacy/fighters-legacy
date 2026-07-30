# Controls

The complete key map, plus gamepad, HOTAS and rebinding. Every binding here is a **default** —
all of them can be changed in `config/bindings.toml`.

New to the game? [Quick start](quickstart.md) covers just enough to fly.

## How bindings work

Every gameplay control is an **action** with a name (`FireWeapon`, `MasterArm`, …) and **any number of
bindings, all live at once**. The gun can be `Space`, the left mouse button, a gamepad shoulder, button
5 on your stick and button 7 on your throttle quadrant simultaneously; press any of them and it fires.
The game never reads a physical key directly — it asks the table for the action — so anything listed on
this page can be rebound in `config/bindings.toml`, and rebinding it moves the control for real.

A binding names the **device** it belongs to, by its GUID rather than by its position in the device
list. That is what lets two joysticks be bound independently, and what makes those bindings survive
unplugging something: device *indices* are renumbered every time hardware comes or goes, so an
index-keyed binding would silently start driving a different piece of hardware. The shipped defaults
name no device at all, which means "whichever stick is plugged in".

**A binding whose device is not connected is kept, and simply does nothing.** Plug the device back in
and the control returns, with no re-binding — nothing is ever pruned from your file for being
temporarily absent. The game logs one warning per missing device at startup, naming it, so a control
that has gone quiet is never a mystery.

Two actions may share an input when the game never reads both at once. `Space` is the gun trigger
while you are flying and the pause key while you are watching a replay; those are different
**contexts** (flying / spectating / replay / photo mode) and never overlap. Two actions that *are*
live together may not share an input, and the build fails if the shipped defaults ever do. Bindings on
two *different* devices are never a clash, even at the same button number. If you hand-edit
`bindings.toml` into a real clash, the game logs a warning naming both actions at startup.

## Flight controls

Active in all camera modes. All game inputs are suppressed while the game console is open; throttle
is held at its last value. While the chat box is open the keyboard is suppressed but the gamepad and
HOTAS stay live, so a partner can keep flying.

| Key | Action | Binding |
|---|---|---|
| Arrow Up / Down | Elevator — nose down / nose up | `PitchDown` / `PitchUp` |
| Arrow Left / Right | Aileron (roll) | `RollLeft` / `RollRight` |
| Z / X | Rudder left / right | `YawLeft` / `YawRight` |
| Page Up / Page Down | Throttle increase / decrease (~1 s to 100% at 60 Hz) | `ThrottleUp` / `ThrottleDown` |
| Left Shift | Max throttle while held (momentary override) | `ThrottleMax` |
| Tab | Afterburner command (bit 1) | `Afterburner` |
| K | Speed brake — momentary, held rather than latched; the one flight-configuration control that is not a switch | `Airbrake` |
| G | Landing gear up / down (#639; latched, absolute on the wire). Position slews at the model's `gear_transit_s` and the drag ramps in with it, so the HUD reads `GEAR ...` mid-travel. **With the gear up, a ground contact is a belly slide** — no brakes, no tyre grip, no nosewheel steering | `LandingGear` |
| F | Flap detent: clean → manoeuvre → full → clean (#639). Lift, drag and stall AoA follow the POSITION as it travels | `Flaps` |
| H | Arresting hook up / down (#639; latched) | `ArrestorHook` |
| `[` | Canopy open / closed (#639; latched) | `CanopyToggle` |
| B | Wheel brakes (bit 6, level; #700). Only bites in ground contact — hold it to stop the rollout after touchdown. Rudder also steers the nosewheel at taxi speed, fading out by ~50 m/s | `WheelBrake` |

### Weapons

| Key | Action | Binding |
|---|---|---|
| Space *or* left mouse | Gun trigger (bit 0, level — hold to keep firing, rate-limited server-side) | `FireWeapon` |
| Enter *or* right mouse | Fire selected store (bit 2 — edge-detected server-side; holding it is one shot) | `FireStore` |
| 1 / 2 | Cycle weapon station next / previous (local; the wire carries the absolute selection) | `NextWeapon` / `PrevWeapon` |
| 4 | Master arm (ARM / SAFE) | `MasterArm` |
| R | Cycle radar mode: Silent → Search → TWS → STT (#526; absolute on the wire). Drives the datalink scope + RWR | `RadarModeCycle` |
| Delete | Dispense chaff + flare (#529; server edge-detects — a held key is one pop). Needs a dispenser with rounds | `CountermeasureDispense` |
| J | Toggle the ECM jammer (#529; denies a hostile radar a lock beyond its burn-through range) | `EcmToggle` |
| End | Eject (#672; server edge-detects — a held key is one ejection). Spawns a parachute and destroys the aircraft; within the seat envelope the pilot survives | `Eject` |
| Backspace | Request a respawn after death in a match (#648) | `Respawn` |

**SAFE really suppresses the fire triggers**, on every input path — it is not a HUD label. Master
arm is on the armament digit row (`1` next station, `2` previous, `3` MFD range, `4` master arm) and
**not** on `V`: `V` is the radio push-to-talk, and while the two shared a key, keying the radio
silently safed your guns ([#1050](https://github.com/fighters-legacy/fighters-legacy/issues/1050)).

### Comms, chat and crew

| Key | Action | Binding |
|---|---|---|
| C | Open the wingman radio menu (#610) | `WingmanMenu` |
| T | Open the comms menu (#704). Non-modal like the wingman menu (the aircraft keeps flying). Digits `1`–`9` pick an item; `Escape` backs out a page, then closes. Root: `1` ATC, `2` Ground crew (#55). ATC page: request takeoff / landing / declare inbound / cancel. Ground crew page: **Refuel / Rearm / Repair** — server-authoritative, honoured only when the aircraft is shut down (stopped, on the ground) at a base: within a few km of an airport, or on a carrier deck. The reply appears as a subtitle (and a voice line when a pack provides `radio/` audio); an ineligible request gets a crew-chief refusal with the reason. After landing and stopping for a couple of seconds in the Chase view, the camera blends into a slow **ramp orbit** around the parked aircraft (the ground-crew scene); it clears on the takeoff roll, a camera-mode change, or after 60 s. | `CommsMenu` |
| Y | Open the chat input box on the **all** channel (#646). While it is open the keyboard is captured — type your line, `Enter` (or the Send button) sends, `Escape` cancels. | `ChatAll` |
| U | Open the chat input box on the **team** channel (#646; reaches only your faction) | `ChatTeam` |
| I | Hold to show the multiplayer scoreboard (#647). Auto-shown in the match end phase. | `Scoreboard` |
| V | **Push to talk** on the selected radio net (Epic J, #531). HELD, never latched. | `PushToTalkPrimary` |
| Left Ctrl | Push to talk on the **flight** net; wins if both PTT keys are held. | `PushToTalkSecondary` |
| `\` | Cycle the primary PTT key through the server's net table (TEAM / FLIGHT / ATC / PROX by default). | `VoiceNetCycle` |
| F8 | Hold to give a wingman order by voice (#935; local transcription, opt-in build) | `WingmanVoiceCommand` |
| , | Crew seat picker (#975): open / cycle the joinable non-fly seats across every crewed aircraft the client knows | `CrewSeatCycle` |
| . | Join the selected crew seat (a gunner on that airframe; the bot parks) | `CrewSeatJoin` |
| / | Leave the current crew seat (become an observer) | `CrewSeatLeave` |

While the radio menu is open, Enter and the digit keys belong to the menu — the fire-store bit and
station cycling are suppressed, the flight axes and gun trigger stay live.

**Voice comms (Epic J).** The bottom-left HUD shows the net your PTT key will use, a `TX` indicator
whose brightness follows the live mic level (the first thing to check when nobody answers), and who
is currently on the air. Transmission is suppressed entirely while the console, the chat box or the
GM map has focus — a PTT key that also fired while you were typing would send your keystrokes'
worth of room noise to the whole team. With no microphone, no permission, or no Opus encoder the
client is simply listen-only; none of those is an error you have to resolve before flying. Full
reference: [voice.md](../developer/voice.md).

The crew seat picker is **non-modal** like the radio menu — the aircraft keeps flying while it is up,
which is why its keys sit on the bottom-row punctuation cluster rather than on letters a flight
control already owns. Only the `Fly` seat runs flight prediction and shows attitude; a gunner seat
views the host airframe without predicting its flight. A join that is denied (the seat is taken by
another player, or you named the pilot seat) is surfaced as a one-line message. Seat join/leave and
the operator `seats` / `set_seat` commands are the multi-crew surface for Epic #966.

**The sandbox aircraft is armed.** The builtin debug entity carries five stations: a 20 mm cannon
(station 1), two IR missiles (2–3) and two radar missiles (4–5) — all compiled-in "builtin:"
weapons, so the whole fire path works with zero content packs mounted. The HUD's right column
shows the selection as `ARM <weapon> x<rounds>`; the default selection is the first IR rail
("selected" means the stores — the gun has its own trigger).

## Camera modes

| Key | Action | Binding |
|---|---|---|
| F1 | Cockpit — camera locked to player entity | `CameraCockpit` |
| F2 | Chase — orbit behind player entity | `CameraChase` |
| F4 | Free (default) — freely movable pivot camera | `CameraFree` |
| F5 | Padlock — slew to keep the designated target centered; auto-picks best-in-cone if nothing is designated, breaks lock with a `PADLOCK — BREAK` cue when terrain or the airframe masks the target, and reverts to Cockpit after a 4 s reacquire window (#697) | `PadlockToggle` (gamepad: RightStick click) |
| F6 | Toggle the target-slaved inset view (#698) | `TargetInsetToggle` |
| F7 | Toggle night-vision goggles — a green photocathode gain applied at the tonemap stage; brightens dim night scenes (#210) | `NvgToggle` |
| F12 | Toggle the game-master overview map (#861) — the whole-battlespace top-down map. Only functional for a peer granted the `gm_map` capability (see `grant` below). While open: ← / → / ↑ / ↓ pan, `-` / `=` zoom, left-click selects an entity, and the side panel issues orders / drops into the entity's view. | `GmMap` |
| N / P | Cycle to the next / previous target (#696) | `NextTarget` / `PrevTarget` (gamepad: DpadUp = next) |
| Keypad 8 / 2 / 4 / 6 | Pan the cockpit view (keyboard alternative to RMB drag; Cockpit/Padlock only) | `ViewUp` / `ViewDown` / `ViewLeft` / `ViewRight` |
| M | Toggle the in-flight aircraft manual (#821) | `AircraftManual` |
| Keypad 9 / 3 | Scroll the manual up / down | `ManualScrollUp` / `ManualScrollDown` |
| F3 | Cycle performance overlay (Off → Compact → Full) | `PerfOverlayCycle` |
| `` ` `` | Toggle game console | `ConsoleToggle` |
| Escape | Pause (when the console and every overlay are closed) | `Pause` |

The cockpit view pan is on the **keypad cross**, not the arrows: the arrows fly the aircraft, and a
key that both rolled the aircraft and panned the view was two live actions on one input. The manual
likewise has its own scroll keys — it is non-modal, so Page Up / Page Down are still the throttle
while it is open.

### Autopilot (#640)

Client-side hold modes, shaped over your input before it is sent (the server stays authoritative). Toggle in Cockpit/Padlock view with no menu or console open; any stick input past a small threshold disengages the attitude holds, and moving the throttle disengages speed hold. The engaged holds and their captured targets are annunciated on the HUD (`AP ALT… HDG… SPD…`).

| Key | Action | Binding |
|---|---|---|
| F9 | Altitude hold (captures current altitude) | `AutopilotAltHold` |
| F10 | Heading hold (captures current heading) | `AutopilotHdgHold` |
| F11 | Speed hold (captures current airspeed) | `AutopilotSpdHold` |

### Combat HUD (#641)

The combat symbology renders in Cockpit/Padlock view against the designated target (#696): an IFF-coloured designator box with range + closure, a gun pipper with ballistic lead when a gun station is selected, a CCIP impact cross + fall line for bombs, and a lower-right weapon-status block.

| Key | Action | Binding |
|---|---|---|
| O | Cycle radar MFD page (Off → PPI → B-scope → RWR) | `MfdPage` |
| 3 | Cycle radar MFD range (10 / 20 / 40 / 80 nm) | `MfdRange` |

The MFD (lower-left) presents the fused datalink picture (#528): the PPI is a 360° plan view (ownship centred, nose up), the B-scope plots azimuth (±60°) vs range, and the RWR page is a dedicated threat-warning ring. All are IFF-coloured (friend green / foe red / unknown amber) and annunciate the requested radar mode (SIL/SRCH/TWS/STT, cycled with **R**). An RWR `RWR LAUNCH` / `RWR LOCK` caption shows on **every** page, including Off.

### Free camera (F4)

The free camera is reachable in **every** mode — flying, spectating, watching a replay and framing a
photo — which is why its movement keys are distinct from the flight controls rather than merely
"unlikely to be pressed at the same time". When switching to it while a player entity exists, the
pivot snaps to the entity's position so it is immediately in view.

| Key / Input | Action | Binding |
|---|---|---|
| LMB drag | Orbit around pivot point | — |
| Scroll wheel | Zoom in / out | — |
| W / S | Pan forward / backward | `FreeCamForward` / `FreeCamBack` |
| A / D | Pan left / right | `FreeCamLeft` / `FreeCamRight` |
| E / Q | Pan up / down (clamped to terrain surface) | `FreeCamUp` / `FreeCamDown` |
| Keypad + / − | Faster / slower movement (2–1000 m/s) | `FreeCamFaster` / `FreeCamSlower` |
| Insert | Reset the pivot to the player entity (or the world origin if there is none) | `FreeCamReset` |

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

| Key | Action | Binding |
|---|---|---|
| 1 | Select / cycle to the **next** live entity (by index); the first pick jumps into Chase | `SpectateNext` |
| 2 | Select / cycle to the **previous** live entity | `SpectatePrev` |
| F1 / F2 | View the selected entity in Cockpit / Chase | `CameraCockpit` / `CameraChase` |
| F4 | Return to the free ghost camera | `CameraFree` |

The spectator picker shares its default keys with the weapon-station cycle on purpose: a spectator
has no ownship, so the two are never read in the same session, and rebinding one leaves the other
where it was.

The selected entity is labelled top-centre by type name and faction (e.g. `[ F-16C | Blue ]`). If the
entity is destroyed or leaves view, the camera degrades gracefully back to free-fly.

### Replay playback and photo mode

Live only while a recording is playing — a replay has no ownship, so these reuse the flight keys
without colliding with them. Full reference: [replays and photo mode](replays-and-photo-mode.md).

| Key | Action | Binding |
|---|---|---|
| Space | Pause / resume | `ReplayPauseToggle` |
| Arrow Left / Right | Scrub ∓5 s | `ReplaySeekBack` / `ReplaySeekForward` |
| Arrow Down / Up | Scrub ∓30 s | `ReplaySeekBackFar` / `ReplaySeekForwardFar` |
| Home / End | Jump to start / end | `ReplaySeekStart` / `ReplaySeekEnd` |
| `-` / `=` | Slower / faster (0.25× … 2×) | `ReplaySpeedDown` / `ReplaySpeedUp` |
| P | Toggle photo mode | `PhotoModeToggle` |
| Page Up / Page Down | Photo mode: zoom in / out (FOV 20°–120°) | `PhotoFovIn` / `PhotoFovOut` |
| Left Shift | Photo mode: hold for 1° FOV steps | `PhotoFovFine` |
| ; / ' | Photo mode: roll the camera left / right | `PhotoRollLeft` / `PhotoRollRight` |
| `-` / `=` | Photo mode: exposure −/+ (¼-stop, ±4 stops) | `PhotoEvDown` / `PhotoEvUp` |
| Enter | Photo mode: export a PNG | `PhotoCapture` |

Photo mode pauses playback, so the speed keys are dead there and the exposure control reuses them.

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

| Axis | Default mapping | Binding |
|---|---|---|
| Throttle | Left trigger — absolute position [0, 1] | `ThrottleAxis` |
| Elevator (pitch) | Right stick Y | `PitchAxis` |
| Aileron (roll) | Right stick X | `RollAxis` |
| Rudder (yaw) | Left stick X | `YawAxis` |

Button bindings are ordinary entries in `[bindings]` with `source = "GamepadButton"` (see below).
Defaults: `FireWeapon` = right shoulder, `FireStore` = left trigger, `NextWeapon` / `PrevWeapon` =
D-pad right / left, `Afterburner` = left shoulder, `Airbrake` = B, `LandingGear` = D-pad down,
`PadlockToggle` = right stick click, `NextTarget` = D-pad up, `WingmanMenu` = Back, `Pause` = Start.

## HOTAS controls

HOTAS sticks, throttle quadrants, rudder pedals and POV hats are supported through the raw joystick
API on all platforms. Windows and macOS work without additional device setup; Linux users may need
udev rules for device permissions (see
[docs/user-guide/gamepad-linux.md](../user-guide/gamepad-linux.md)).

**Everything on a HOTAS is an ordinary binding.** Axes, buttons and hat directions all live in
`[bindings]` alongside the keyboard, so a HOTAS trigger can fire the gun, a hat can drop the gear, and
every one of them is visible to the conflict checker. Up to and including v0.3.13 the four flight axes
lived in a separate `[controls]` section of `user.toml`, read by index on whichever stick happened to be
first, and *no* HOTAS button could be bound to anything at all.

| Source | Field | Meaning |
|---|---|---|
| `JoystickAxis` | `index` | Raw axis number on the device |
| `JoystickButton` | `index` | Raw button number on the device |
| `JoystickHat` | `index` + `direction` | Hat number, plus which of the eight directions triggers |

A binding on a cardinal hat direction (`Up`/`Down`/`Left`/`Right`) also fires on the two diagonals
either side of it, so a four-way POV view control does not go dead when your thumb rolls slightly off.
A binding on a diagonal (`UpRight`, …) matches only that corner.

Default axis layout, applied to whichever stick is connected:

| Default axis index | Mapping | Action |
|---|---|---|
| 0 | Aileron (roll) | `RollAxis` |
| 1 | Elevator (pitch) | `PitchAxis` |
| 2 | Throttle | `ThrottleAxis` |
| 3 | Rudder (yaw) | `YawAxis` |

For each of the four flight axes the joystick binding is listed **first**, ahead of the gamepad axis:
for an analog control the first binding past its deadzone wins, so if you have a HOTAS the HOTAS is in
command. Reorder the list to change that.

Deadzone, response curve, inversion and scale for a HOTAS axis are set in `[[axis_config]]` with
`source = "JoystickAxis"` — the same table the gamepad axes use. The stick axes default to a `0.05`
deadzone (a HOTAS potentiometer has far less slop than a thumbstick) and the throttle axis defaults to
`mode = "Absolute"`; see below.

**Upgrading from v0.3.13 or earlier.** The `hotas_*` keys in `[controls]` are read once, folded into
`config/bindings.toml`, and then ignored — your axis assignments, deadzone and inversions carry over
automatically, including an axis you had disabled with `-1`. Your previous `bindings.toml` is kept
beside the new one as `bindings.toml.bak`. The `[controls]` keys are no longer written and can be
deleted by hand.

## `config/bindings.toml`

Generated at `<user data>/config/bindings.toml` on first run. A restart applies changes. It has four
parts: `version`, an optional `[[devices]]` list, `[bindings]`, and `[[axis_config]]`.

### `version`

Marks which shipped key map and file schema the file was written from. Lowering or removing it makes
the game regenerate the file from the current defaults. When the game upgrades the file it writes your
old one beside it as `bindings.toml.bak` and says so in the log.

### `[[devices]]`

The joysticks this file refers to, recorded the first time each one is connected:

```toml
[[devices]]
guid = "03000000a1b2c3d4000000000000aaaa"
name = "Thrustmaster T.16000M"
```

The **GUID is the identity**; the name is there so you have something legible to work with and so a
warning about an absent device can name it. Copy a GUID from here into a binding's `device` field to
pin that binding to that specific stick.

### `[bindings]`

One array per action. Every entry in the array is live at the same time; order matters only for analog
axes, where the first one past its deadzone wins.

```toml
[bindings]
FireWeapon = [
  { source = "Keyboard",       id = "Space" },
  { source = "MouseButton",    id = "Left" },
  { source = "GamepadButton",  id = "RightShoulder" },
  { source = "JoystickButton", index = 5, device = "03000000a1b2c3d4000000000000aaaa" },
  { source = "JoystickButton", index = 7, device = "03000000a1b2c3d4000000000000bbbb" },
]
LandingGear = [
  { source = "Keyboard",    id = "G" },
  { source = "JoystickHat", index = 0, direction = "Down" },
]
PitchAxis = [
  { source = "JoystickAxis", index = 1 },
  { source = "GamepadAxis",  id = "RightY", negative = false },
]
EcmToggle = []   # explicitly unbound
```

`source` is one of `None` / `Keyboard` / `MouseButton` / `GamepadButton` / `GamepadAxis` /
`JoystickButton` / `JoystickAxis` / `JoystickHat`.

- `id` names an enum value: letters, digits, `Space`, `Enter`, `Tab`, `Escape`, arrows, `Home`, `End`,
  `PageUp`, `PageDown`, `Insert`, `Delete`, `F1`–`F12`, `Minus`, `Equals`, `Comma`, `Period`, `Slash`,
  `Semicolon`, `Apostrophe`, `LeftBracket`, `RightBracket`, `Backslash`, `Grave`, `Numpad0`–`Numpad9`,
  `NumpadPlus`, `NumpadMinus`, `NumpadMultiply`, `NumpadDivide`, `NumpadPeriod`, `NumpadEnter` and the
  four modifier pairs; mouse `Left` / `Middle` / `Right`; the gamepad button and axis names from the
  tables above.
- `index` is the raw number for the three `Joystick*` sources, which have no fixed enum.
- `direction` applies to `JoystickHat` only: `Up`, `UpRight`, `Right`, `DownRight`, `Down`, `DownLeft`,
  `Left`, `UpLeft`.
- `negative` applies to axis sources used as an on/off control, and selects the negative half.
- `device` applies to the `Joystick*` sources; omit it or leave it empty for "whichever stick is
  plugged in".

An entry the game cannot resolve — an unknown key name, a hat with no direction, a joystick binding
with no index — makes it reject the whole file and keep the previous bindings rather than guess at a
different control.

If two actions that are live at the same time end up on the same input, the game logs a warning at
startup naming both — it will not silently pick one, which is how a radio key came to double as the
master-arm switch ([#1050](https://github.com/fighters-legacy/fighters-legacy/issues/1050)).

A file written by an older build still loads: the `[primary]` / `[secondary]` / `[gamepad]` sections
(and the even older `[alt]`) are read in that order into one binding list per action, so your rebinds
survive the upgrade.

### `[[axis_config]]`

Per-axis deadzone, response curve, mode, inversion and scale, keyed by the same
`(source, device, index)` triple a binding uses — so a gamepad, a stick and a second identical stick
can each be tuned separately.

```toml
[[axis_config]]
source = "GamepadAxis"
id = "RightY"
deadzone = 0.1
curve = "Linear"
mode = "Centered"
invert = false
scale = 1.0

[[axis_config]]
source = "JoystickAxis"
index = 2
mode = "Absolute"
deadzone = 0.05
invert = false
```

- **deadzone**: axis magnitude at or below this reads as zero *and* leaves the control to the keyboard
  (clamped to [0, 0.99]). Ignored in `Absolute` mode.
- **curve**: `"Linear"` passes through; `"Cubic"` reduces sensitivity near centre.
- **mode**: `"Centered"` for a spring-return stick or thumbstick — zero is the middle of its travel and
  the value is the control deflection. `"Absolute"` for a lever that stays where you leave it: the full
  `[-1, 1]` travel maps onto `[0, 1]`, there is no centre deadzone, and it commands the control even at
  idle (a closed throttle is a command, not an absent input). Use `Absolute` for a HOTAS throttle.
- **invert**: flips the axis. In `Absolute` mode it flips which end of the travel is full power, which
  is what the old `hotas_invert_throttle` did.
- **scale**: output multiplier applied after the curve (default `1.0`).

Defaults: the six gamepad axes at `deadzone = 0.1`, `Linear`, `Centered`; joystick axes 0, 1 and 3 at
`deadzone = 0.05`, `Centered`; joystick axis 2 at `deadzone = 0.05`, `Absolute`. An axis with no entry
of its own falls back to the same axis with no `device` set, and then to those defaults.

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

