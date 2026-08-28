// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IClock.h"
#include "input/AxisConfig.h"    // fl::AxisConfigTable, fl::AxisConfig — also pulls in IInput.h
#include "input/BindingQuery.h"  // fl::actionDown / fl::actionAxis / fl::ActionEdgeTracker
#include "input/InputBindings.h" // fl::InputBindings, fl::Binding, fl::BindingSource, fl::InputAction
#include "input/InputSources.h"  // fl::InputSources — the live hardware, one struct (#1061)
#include "net/GameProtocol.h"

#include <chrono>
#include <cstdint>
#include <optional>

namespace fl {

class CameraInput;
class GameConsole;

class SimRenderBridge;

// Assembles a MsgClientInput from keyboard, mouse, gamepad and raw joystick / HOTAS input each frame,
// rate-limited to 60 Hz to avoid triggering the server's per-peer flood guard.
//
// Every control — including the four HOTAS axes, which had their own parallel config path until
// #1061 — resolves through InputBindings against the InputSources handed to poll(). There is no
// device-specific block left in this file.
//
// Usage (once per frame):
//   if (auto msg = flightInput.poll(...))
//       clientNet->send(0, &*msg, sizeof(*msg), /*reliable=*/false);
// The fixed client->server input send cadence: the server steps at 60 Hz, so this is a rate
// matched to it, not an assumption about how fast frames arrive (#1241).
inline constexpr float kInputSendPeriodS = 1.0f / 60.0f;
// The longest gap a single poll may bill for. Without it, a stall or a debugger pause would hand
// the throttle ramp a multi-second step and slam it to an end stop on the resuming frame.
inline constexpr float kMaxInputElapsedS = 0.25f;

class FlightInputCollector {
  public:
    // Returns a populated MsgClientInput if kInputSendPeriodS has elapsed since the last
    // packet, otherwise returns nullopt. Never call from the server thread.
    // uiFocused: an in-flight overlay (the radio menu, #610) has the discrete keys/buttons this
    // frame. The AXES keep working — the menu is non-modal on purpose, because suppressing flight
    // input would leave throttle at 0, which is what opening the console already does and is exactly
    // what a combat radio menu must not do. Only the buttons the overlay consumes are gated.
    // textEntry: a text field owns the keyboard this frame (the chat input box, #646). Every
    // KEYBOARD-source binding is suppressed (Space would otherwise fire the gun while you type), but
    // the gamepad and HOTAS axes stay live and the throttle holds — a partner keeps flying while
    // chatting. The suppression is per binding SOURCE, not per input block, so a control bound to
    // both a key and a pad button keeps working from the pad.
    std::optional<MsgClientInput> poll(const SimRenderBridge& bridge, CameraInput& camInput, const GameConsole& console,
                                       const InputSources& sources, bool uiFocused = false, bool textEntry = false);

    // True if the most recent poll() that returned a message had the weapon
    // trigger bit set. Resets to false on each poll() call.
    [[nodiscard]] bool wasWeaponFired() const noexcept {
        return m_weaponFired;
    }

    // Weapon-station cycling (#625). Game.cpp sets the count from the player's entity def
    // (hardpoints.size()) once the type is known; 0 = cycling off. Selection is client-local and
    // sent ABSOLUTE on the wire; the server confirms via the own-entity snapshot record.
    void setStationCount(uint8_t count) noexcept {
        m_stationCount = count;
    }
    [[nodiscard]] uint8_t selectedStation() const noexcept {
        return m_selectedStation;
    }

    // Live articulation switch state (#639), for the HUD's gear/flap readout. `flaps` is the commanded
    // detent as a 0..1 fraction; the actual POSITION lags it through the actuator's transit.
    [[nodiscard]] bool gearDown() const noexcept {
        return m_gearDown;
    }
    [[nodiscard]] float flapCommand() const noexcept {
        return static_cast<float>(m_flapDetent) * 0.5f;
    }
    [[nodiscard]] bool hookDown() const noexcept {
        return m_hookDown;
    }

    // Master arm (#641): ARM/SAFE, toggled by InputAction::MasterArm. When SAFE, poll() suppresses
    // the gun and fire-store trigger bits — the safety is real, not a cosmetic HUD flag.
    // Defaults to ARM.
    [[nodiscard]] bool masterArm() const noexcept {
        return m_masterArm;
    }

    // The radar mode last requested (#526/#528), for the MFD annunciation (#642). 255 = keep-server.
    [[nodiscard]] uint8_t radarMode() const noexcept {
        return m_radarMode;
    }

    void setClock(const IClock& clock);

    // Apply a loaded InputBindings table. EVERY control this collector reads resolves through it
    // (#1050) — keyboard, mouse, gamepad — so this is what makes the whole flight surface
    // rebindable, not just the gamepad axis mapping it used to cover.
    void setBindings(InputBindings bindings);

    // Apply a loaded AxisConfigTable for per-axis deadzone / curve / mode / invert / scale. Since
    // #1061 it is keyed by (source, device, axis index), so a stick and a pad can be tuned apart.
    void setAxisConfig(AxisConfigTable cfg);

  private:
    uint32_t m_inputSeq{0};
    uint8_t m_stationCount{0};      // stations on the player's aircraft; 0 = unknown
    uint8_t m_selectedStation{255}; // 255 = none
    // ONE edge tracker for every latched control (#1050). Each control used to carry its own
    // `m_prevXKey` bool, and the gamepad path carried a SECOND copy of the same edge for the same
    // action (m_prevPadGear/m_prevPadFlap/m_prevPadNext/m_prevPadPrev) — two edge detectors for one
    // switch. Resolving an action across all its slots collapses both into a single state.
    ActionEdgeTracker m_edges{};
    // Radar mode (#526/#528): Silent/Search/TWS/STT. Starts at TWS (the server spawn default) and
    // sends 255 (keep) until the player first touches it, so the spawn mode is respected.
    uint8_t m_radarMode{2}; // fl::sensor::RadarMode::Tws ordinal
    bool m_radarModeTouched{false};
    bool m_ecmOn{false};    // #529: the jammer latch
    bool m_masterArm{true}; // #641: ARM by default; MasterArm toggles to SAFE (gates the fire bits)

    // ── articulation switches (#639) ─────────────────────────────────────────
    // Latched client-side and sent as ABSOLUTE STATE every packet, so a dropped packet costs one tick
    // of lag rather than leaving the aircraft in the wrong configuration for the rest of the sortie.
    // Gear starts DOWN because that is the configuration an aircraft is parked in.
    bool m_gearDown{true};
    bool m_hookDown{false};
    bool m_canopyOpen{false};
    // Flap detents: clean / manoeuvre / full, the three positions a real flap lever has. A continuous
    // axis would be a worse control on a keyboard and matches no cockpit.
    uint8_t m_flapDetent{0};
    float m_speedbrake{0.f}; // held, not latched: the airbrake is a momentary control
    const IClock* m_clock{&SystemClock::instance()};
    std::chrono::steady_clock::time_point m_lastInputTime{};
    bool m_haveLastInput{false};
    bool m_weaponFired{false};

    InputBindings m_bindings{};     // default: the shipped key map
    AxisConfigTable m_axisConfig{}; // default: deadzone=0.1, Linear, no invert, scale=1
    // Where every observed axis sat last poll, so an Absolute lever commands the throttle only while
    // it is the thing being moved and the keyboard stays usable beside it (#1358). Poll-cadence state
    // for the same reason m_edges is.
    AxisMotionTracker m_axisMotion{};
};

} // namespace fl
