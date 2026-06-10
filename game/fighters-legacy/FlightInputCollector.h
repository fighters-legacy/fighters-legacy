// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "net/GameProtocol.h"

#include <chrono>
#include <cstdint>
#include <optional>

class CameraInput;
class DebugConsole;
class IInput;
class IJoystick;
struct ControlsSettings;

namespace fl {
class SimRenderBridge;
} // namespace fl

// Assembles a MsgClientInput from keyboard, gamepad, and HOTAS inputs each frame,
// rate-limited to 60 Hz to avoid triggering the server's per-peer flood guard.
//
// Usage (once per frame):
//   if (auto msg = flightInput.poll(...))
//       clientNet->send(0, &*msg, sizeof(*msg), /*reliable=*/true);
class FlightInputCollector {
  public:
    // Returns a populated MsgClientInput if 1/60 s has elapsed since the last
    // packet, otherwise returns nullopt. Never call from the server thread.
    std::optional<fl::MsgClientInput> poll(const fl::SimRenderBridge& bridge, CameraInput& camInput,
                                           const DebugConsole& console, IInput& input, IJoystick* joystick,
                                           const ControlsSettings& cs);

    // True if the most recent poll() that returned a message had the weapon
    // trigger bit set. Resets to false on each poll() call.
    [[nodiscard]] bool wasWeaponFired() const noexcept {
        return m_weaponFired;
    }

  private:
    uint32_t m_inputSeq{0};
    std::chrono::steady_clock::time_point m_lastInputTime{std::chrono::steady_clock::now()};
    bool m_weaponFired{false};
};
