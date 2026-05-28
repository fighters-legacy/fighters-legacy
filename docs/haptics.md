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
| **Afterburner ignition** | 0.4 | 0.2 | — | ramp 300 then sustain | Ramp up on ignition; hold low-intensity while afterburner active |
| **Engine failure (single)** | 0.5 | 0.0 | — | continuous | Port engine: left motor only; starboard: right motor only — lets player feel which side |
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

## Lua scripting (future / Phase 2)

Mod authors writing custom weapons, missions, or flight behaviours would benefit from triggering haptic feedback from Lua. This requires a dedicated engine-layer binding separate from the AI runtime, because:

- `IInput` is a platform HAL; Lua scripts must not call it directly.
- The Lua-facing API should abstract `gamepadId` away — always targets the current player's gamepad, mediated by the engine game loop.
- Sandbox guards are needed so untrusted mods cannot lock rumble on or call `stopRumble` at arbitrary times.

A tracking issue for the Lua binding is filed separately as a Phase 2 item.
