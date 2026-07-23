// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Binding.h" // pulls in IInput.h (Key / MouseButton / Gamepad* + the IInput interface)

// Resolve a Binding against live IInput state (#689). One place translates a Binding's source into the
// matching IInput query, so every consumer (camera-mode keys, target cycling, HUD/view toggles) edge-
// detects a rebindable, gamepad-capable control identically instead of re-implementing the switch.
//
// A GamepadAxis binding has no rising edge — bindingJustPressed returns false for it; bindingDown
// reports the axis crossed a ±0.5 threshold (respecting the binding's negative-direction flag).

namespace fl {

[[nodiscard]] inline bool bindingDown(const IInput& in, const Binding& b, int gamepadId = 0) {
    switch (b.source) {
    case BindingSource::Keyboard:
        return in.isKeyDown(static_cast<Key>(b.id));
    case BindingSource::MouseButton:
        return in.isMouseButtonDown(static_cast<MouseButton>(b.id));
    case BindingSource::GamepadButton:
        return in.isGamepadButtonDown(gamepadId, static_cast<GamepadButton>(b.id));
    case BindingSource::GamepadAxis: {
        const float v = in.getGamepadAxis(gamepadId, static_cast<GamepadAxis>(b.id));
        return b.axisNegative ? v < -0.5f : v > 0.5f;
    }
    default:
        return false;
    }
}

[[nodiscard]] inline bool bindingJustPressed(const IInput& in, const Binding& b, int gamepadId = 0) {
    switch (b.source) {
    case BindingSource::Keyboard:
        return in.isKeyJustPressed(static_cast<Key>(b.id));
    case BindingSource::MouseButton:
        return in.isMouseButtonJustPressed(static_cast<MouseButton>(b.id));
    case BindingSource::GamepadButton:
        return in.isGamepadButtonJustPressed(gamepadId, static_cast<GamepadButton>(b.id));
    case BindingSource::GamepadAxis:
    default:
        return false; // an analog axis has no discrete rising edge
    }
}

} // namespace fl
