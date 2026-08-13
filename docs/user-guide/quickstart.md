# Quick start

Getting airborne, in about five minutes. Assumes the game is [installed](installation.md).

## Get in the air

Start the game and pick **Instant Action** from the menu. That drops you straight into a skirmish
with an aircraft, a wingman and something to shoot at. **Free Flight** gives you the same aircraft
in an empty world if you would rather just fly.

You start airborne. There is no takeoff to get wrong.

## Fly it

| Key | Does | Binding |
|---|---|---|
| `↑` / `↓` | Nose down / up | `PitchDown` / `PitchUp` |
| `←` / `→` | Roll left / right | `RollLeft` / `RollRight` |
| `Z` / `X` | Rudder left / right | `YawLeft` / `YawRight` |
| `Page Up` / `Page Down` | Throttle up / down | `ThrottleUp` / `ThrottleDown` |
| `Left Shift` | Max throttle while held | `ThrottleMax` |
| `Tab` | Afterburner | `Afterburner` |
| `K` | Airbrake — held, not latched | `Airbrake` |

Pull gently. These are combat aircraft with real aerodynamics behind them — yank the stick at speed
and you will bleed energy, buffet and depart. The HUD's left column shows airspeed, altitude and
altitude above ground; the right column shows throttle and fuel.

The full map is in [Controls](controls.md).

## Look around

| Key | View | Binding |
|---|---|---|
| `F1` | Cockpit | `CameraCockpit` |
| `F2` | Chase | `CameraChase` |
| `F4` | Free camera — fly the camera anywhere with `WASD`/`QE`, drag with the left mouse button | `CameraFree` |
| `F5` | Padlock — keeps the target in view | `PadlockToggle` |

The cockpit view pans with the **keypad** cross (`Numpad 8` / `2` / `4` / `6`). The arrows fly the
aircraft, so they are not also the view.

Hold the right mouse button in cockpit view to look around without changing where the aircraft is
going.

## Shoot something

Your aircraft starts armed.

| Key | Does | Binding |
|---|---|---|
| `N` / `P` | Cycle to the next / previous target | `NextTarget` / `PrevTarget` |
| `1` / `2` | Select the next / previous weapon station | `NextWeapon` / `PrevWeapon` |
| `Space` | Fire the gun | `FireWeapon` |
| `Enter` | Release the selected store | `FireStore` |

The HUD's right column reads `ARM <weapon> x<rounds>`. In cockpit view you get a gun pipper with
ballistic lead, and a designator box around the target with range and closure.

If nothing fires at all, check master arm: `4` toggles ARM / SAFE, and SAFE really does suppress the
triggers.

## Order your wingman

Press `C`, then a number.

| Item | Order |
|---|---|
| `1` | Attack my target — whatever you are looking at |
| `2` | Engage bandits near itself |
| `3` | Rejoin formation |
| `4` | Cover me |
| `5` | Hold fire |
| `6` | Return to base |

The menu does not freeze the aircraft — you keep flying while it is open, because a radio call is a
sub-second action and being frozen mid-fight to make one would be wrong.

In single-player you always have a wingman. On a dedicated server you only get one if the operator
configured a flight. More in [Wingman & voice](voice-and-wingman.md).

## Other things worth knowing early

- **`` ` `` opens the console.** Careful: it *does* suspend your flight controls, unlike the radio
  menu.
- **`F3` cycles the performance overlay** — frame rate, entity count, tick time.
- **`Escape` pauses** in single-player and opens the menu.
- **Every match is recorded.** A server records by default, and you can watch a match back with a
  scrubbable timeline and any camera. See [Replays & photo mode](replays-and-photo-mode.md).

## Where next

- [Controls](controls.md) — the complete map, gamepads and HOTAS
- [Multiplayer](multiplayer.md) — finding and joining servers
- [Installation](installation.md#content-packs) — installing real aircraft
