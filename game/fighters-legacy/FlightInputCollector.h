// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IClock.h"
#include "input/AxisConfig.h"    // fl::AxisConfigTable, fl::AxisConfig — also pulls in IInput.h
#include "input/InputBindings.h" // fl::InputBindings, fl::Binding, fl::BindingSource, fl::InputAction
#include "net/GameProtocol.h"

#include <chrono>
#include <cstdint>
#include <optional>

namespace fl {

class CameraInput;
class GameConsole;
class IJoystick;
struct ControlsSettings;

class SimRenderBridge;

// Assembles a MsgClientInput from keyboard, gamepad, and HOTAS inputs each frame,
// rate-limited to 60 Hz to avoid triggering the server's per-peer flood guard.
//
// Usage (once per frame):
//   if (auto msg = flightInput.poll(...))
//       clientNet->send(0, &*msg, sizeof(*msg), /*reliable=*/false);
class FlightInputCollector {
  public:
    // Returns a populated MsgClientInput if 1/60 s has elapsed since the last
    // packet, otherwise returns nullopt. Never call from the server thread.
    // uiFocused: an in-flight overlay (the radio menu, #610) has the discrete keys/buttons this
    // frame. The AXES keep working — the menu is non-modal on purpose, because suppressing flight
    // input would leave throttle at 0, which is what opening the console already does and is exactly
    // what a combat radio menu must not do. Only the buttons the overlay consumes are gated.
    // textEntry: a text field owns the keyboard this frame (the chat input box, #646). The whole
    // KEYBOARD control block is suppressed (Space would otherwise fire the gun while you type), but the
    // gamepad and HOTAS axes stay live and the throttle holds — a partner can keep flying while chatting.
    std::optional<MsgClientInput> poll(const SimRenderBridge& bridge, CameraInput& camInput, const GameConsole& console,
                                       IInput& input, IJoystick* joystick, const ControlsSettings& cs,
                                       bool uiFocused = false, bool textEntry = false);

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

    // Master arm (#641): ARM/SAFE, toggled by V. When SAFE, poll() suppresses the gun and fire-store
    // trigger bits — the safety is real, not a cosmetic HUD flag. Defaults to ARM.
    [[nodiscard]] bool masterArm() const noexcept {
        return m_masterArm;
    }

    // The radar mode last requested (#526/#528), for the MFD annunciation (#642). 255 = keep-server.
    [[nodiscard]] uint8_t radarMode() const noexcept {
        return m_radarMode;
    }

    void setClock(const IClock& clock);

    // Apply a loaded InputBindings table so gamepad axis mapping is user-configurable.
    // Default-constructed InputBindings uses the built-in alt axis defaults.
    void setBindings(InputBindings bindings);

    // Apply a loaded AxisConfigTable for per-axis deadzone/curve/invert/scale.
    // Default-constructed AxisConfigTable uses deadzone=0.1, Linear, no invert, scale=1.
    void setAxisConfig(AxisConfigTable cfg);

  private:
    uint32_t m_inputSeq{0};
    uint8_t m_stationCount{0};      // stations on the player's aircraft; 0 = unknown
    uint8_t m_selectedStation{255}; // 255 = none
    bool m_prevNextKey{false};      // edge detectors for the cycle keys
    bool m_prevPrevKey{false};
    // Radar mode (#526/#528), cycled with R: Silent/Search/TWS/STT. Starts at TWS (the server spawn
    // default) and sends 255 (keep) until the player first touches it, so the spawn mode is respected.
    uint8_t m_radarMode{2}; // fl::sensor::RadarMode::Tws ordinal
    bool m_radarModeTouched{false};
    bool m_prevRadarKey{false};
    // Electronic warfare (#529): E dispenses (level bit, server edge-detects), J toggles the jammer.
    bool m_ecmOn{false};
    bool m_prevEcmKey{false};
    bool m_masterArm{true}; // #641: ARM by default; V toggles to SAFE (gates the fire bits)
    bool m_prevArmKey{false};
    bool m_prevPadNext{false}; // and for the gamepad D-pad
    bool m_prevPadPrev{false};
    const IClock* m_clock{&SystemClock::instance()};
    std::chrono::steady_clock::time_point m_lastInputTime{};
    bool m_weaponFired{false};

    InputBindings m_bindings{};     // default: built-in alt axis defaults
    AxisConfigTable m_axisConfig{}; // default: deadzone=0.1, Linear, no invert, scale=1
};

} // namespace fl
