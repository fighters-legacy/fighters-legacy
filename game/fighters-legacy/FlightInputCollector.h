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
    std::optional<MsgClientInput> poll(const SimRenderBridge& bridge, CameraInput& camInput, const GameConsole& console,
                                       IInput& input, IJoystick* joystick, const ControlsSettings& cs);

    // True if the most recent poll() that returned a message had the weapon
    // trigger bit set. Resets to false on each poll() call.
    [[nodiscard]] bool wasWeaponFired() const noexcept {
        return m_weaponFired;
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
    const IClock* m_clock{&SystemClock::instance()};
    std::chrono::steady_clock::time_point m_lastInputTime{};
    bool m_weaponFired{false};

    InputBindings m_bindings{};     // default: built-in alt axis defaults
    AxisConfigTable m_axisConfig{}; // default: deadzone=0.1, Linear, no invert, scale=1
};

} // namespace fl
