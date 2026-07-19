# Haptic Feedback Design Reference

This document is for game-system implementors wiring rumble events to the `IInput` haptic API. It is not a user-facing guide.

## Interface summary

Five methods on `IInput` (`platform/IInput.h`) cover the complete haptic lifecycle:

| Method | Purpose |
|---|---|
| `supportsRumble(gamepadId)` | Check main-motor capability before calling `rumble()` |
| `supportsTriggerRumble(gamepadId)` | Check trigger-motor capability before calling `rumbleTriggers()` |
| `rumble(gamepadId, lowFreq, highFreq, durationMs)` | Fire main-motor vibration; low-freq targets the left motor (body vibration), high-freq the right (detail/texture) |
| `rumbleTriggers(gamepadId, leftRumble, rightRumble, durationMs)` | Fire per-trigger motor vibration (Xbox Elite, DualSense) |
| `stopRumble(gamepadId)` | Cancel all in-progress rumble — main motors and triggers — immediately |

All intensity values are normalised `[0.0, 1.0]`. The backend scales to hardware range.

## Guidance for implementors

- Always call `supportsRumble` / `supportsTriggerRumble` before firing effects; skip gracefully on hardware without motors.
- Call `stopRumble` on pause, menu entry, and game exit — never leave a rumble running in a paused state.
- Keep durations short on repetitive events (gun bursts) to avoid fatigue.
- `stopRumble` silences both main motors and triggers in one call; no need to stop them separately.

## Event catalogue

Suggested haptic events for flight-sim game systems. Tuning values are starting points; adjust by playtesting.

| Event | Low-freq | High-freq | Trigger | Duration (ms) | Notes |
|---|---|---|---|---|---|
| **Gun burst** | 0.0 | 0.8 | — | 80 per burst | Short, high-freq pulse per trigger pull; repeat cadence matches fire rate |
| **Missile launch** | 0.6 | 0.6 | — | 150 | Single pulse, both motors |
| **Missile warning** | 0.7 | 0.0 | — | 3 × 50 ms bursts | Distinct from gun fire — left-only, pulsed; mirrors audio lock tone |
| **Landing gear touchdown** | 0.9 | 0.3 | — | 200 | Heavy low-freq on impact |
| **Hit taken** | 0.8 | 0.4 | — | 120 | Asymmetric if direction known: port hit → left motor heavier |
| **Stall buffet** | 0.3 | 0.1 | — | continuous | Sustain while AoA exceeds stall threshold; stop on recovery |
| **Afterburner ignition** | 0.4 | 0.2 | — | ramp 300 then sustain | Driven by `EntityRenderEntry::abEngaged` (set by `FlightIntegrator` when `ctrl.afterburner` commanded and aircraft has an `ab_thrust` table); replaces former `throttle == 100` proxy |
| **Engine failure (single)** | 0.5 | 0.0 | — | continuous | Driven by `EntityRenderEntry::engineFailFlags`; `kEngineFailLeft` (bit 1) → left motor only; `kEngineFailRight` (bit 2) → right motor only; both or `kEngineFailGeneric` (bit 0) → symmetric; currently only `kEngineFailGeneric` is populated (from `damageLevel ≥ Heavy`) |
| **G-LOC onset** | 0.0 | 0.6 | — | continuous | Intensity proportional to G load above 6G; peak just before grey-out |
| **Compressor stall** | 0.6 | 0.0 | — | 4 × 30 ms irregular | Stutter pattern — uneven spacing distinguishes it from gun fire |
| **GPWS / terrain warning** | 0.5 | 0.5 | — | 2 × 100 ms | Distinct double-pulse; easily distinguished from the 3-pulse missile warning |
| **Carrier trap** | — | — | 0.9 / 0.9 | 300 | Trigger-motor pull on arrestor-wire engagement; use `rumbleTriggers` |
| **Hydraulic failure** | 0.2 | 0.0 | — | continuous on input | Low continuous rumble whenever a control surface is deflected |
| **Transonic buffet** | 0.3 | 0.3 | — | 400 | Brief oscillation at Mach 0.85–1.05 transition |
| **Bomb / ordinance release** | 0.4 | 0.0 | — | 80 | Single low-freq thud per store dropped |

## Platform notes

- **Windows:** SDL3 XInput/DirectInput handles capability detection. Xbox controllers work over Bluetooth or USB without extra setup.
- **macOS:** SDL3 uses the GameController framework transparently. MFi and DualSense controllers report rumble support correctly.
- **Linux:** Rumble requires xpadneo + hidraw udev rules and `input` group membership. `supportsRumble` accuracy depends on whether xpadneo is active; without it the stock `hid_microsoft` driver connects the hardware but SDL3 may correctly report no rumble capability. See [linux-gamepad.md](linux-gamepad.md).

## Lua scripting (#128)

Mod authors writing custom missions or AI behaviours can trigger haptic feedback from Lua. The binding
lives in the engine's Lua runtime (`LuaController`) and is deliberately **separate from `IInput`**:

- `IInput` is a platform HAL; Lua scripts never call it directly.
- The Lua-facing API **abstracts `gamepadId` away** — it always targets the current player's gamepad.
  Because a mission script runs server-side, the call is routed to clients (a reliable `MsgHaptic`),
  and each client plays it on its own local gamepad (id 0). There is no gamepad id in the Lua API.
- Sandbox guards live in the engine binding: intensities clamp to `[0, 1]` and a single request is
  capped at **5000 ms**, so an untrusted mod cannot latch rumble on. `stop_rumble()` is always available.

The bindings are plain globals (not under `world.*`):

```lua
rumble(low_freq, high_freq, duration_ms)      -- both motors; low = heavy, high = buzz
rumble_triggers(left, right, duration_ms)     -- impulse triggers (Xbox One / Series pads)
stop_rumble()                                 -- cancel all rumble immediately
```

Routing: `LuaController` → the host `WorldApi` seam (`engine/script/WorldApi.h`) → `fl-server`
broadcasts `MsgHaptic` (0x17) → the client plays it via `IInput` on gamepad 0. On a headless server
with no clients (or a null-input mock), the whole path is a clean no-op. Full API reference:
[`docs/modding/ai.md`](modding/ai.md).
