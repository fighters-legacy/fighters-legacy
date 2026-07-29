// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Binding.h" // pulls in IInput.h (Key / MouseButton / Gamepad* + the IInput interface)
#include "InputBindings.h"

#include <array>

// Resolve bindings against live IInput state (#689, extended #1050). One place translates a
// Binding's source into the matching IInput query, so every consumer edge-detects a rebindable,
// gamepad-capable control identically instead of re-implementing the switch — or, as happened
// before #1050, reading a raw `Key::V` and bypassing the binding table altogether.
//
// A GamepadAxis binding has no rising edge — bindingJustPressed returns false for it; bindingDown
// reports the axis crossed a ±0.5 threshold (respecting the binding's negative-direction flag).

namespace fl {

[[nodiscard]] inline bool isGamepadSource(BindingSource s) {
    return s == BindingSource::GamepadButton || s == BindingSource::GamepadAxis;
}

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

// True when ANY slot of the action is active. `suppressDesktop` drops the keyboard- and
// mouse-source slots only — a chat box owns the keyboard and the pointer that clicks its Send
// button, not the stick, so a partner keeps flying from the pad while you type.
[[nodiscard]] inline bool actionDown(const IInput& in, const InputBindings& bindings, InputAction action,
                                     int gamepadId = 0, bool suppressDesktop = false) {
    for (int s = 0; s < InputBindings::kSlotCount; ++s) {
        const Binding b = bindings.get(action, static_cast<BindingSlot>(s));
        if (b.isNone())
            continue;
        if (suppressDesktop && !isGamepadSource(b.source))
            continue;
        if (bindingDown(in, b, gamepadId))
            return true;
    }
    return false;
}

[[nodiscard]] inline bool actionJustPressed(const IInput& in, const InputBindings& bindings, InputAction action,
                                            int gamepadId = 0, bool suppressDesktop = false) {
    for (int s = 0; s < InputBindings::kSlotCount; ++s) {
        const Binding b = bindings.get(action, static_cast<BindingSlot>(s));
        if (b.isNone())
            continue;
        if (suppressDesktop && !isGamepadSource(b.source))
            continue;
        if (bindingJustPressed(in, b, gamepadId))
            return true;
    }
    return false;
}

// Returns the axis a slot of this action is bound to, or GamepadAxis::Count when none is.
[[nodiscard]] inline GamepadAxis actionAxisId(const InputBindings& bindings, InputAction action) {
    for (int s = 0; s < InputBindings::kSlotCount; ++s) {
        const Binding b = bindings.get(action, static_cast<BindingSlot>(s));
        if (b.source == BindingSource::GamepadAxis)
            return static_cast<GamepadAxis>(b.id);
    }
    return GamepadAxis::Count;
}

// Level-plus-own-edge tracking, for consumers that do not sample every frame. FlightInputCollector
// is rate-limited to 60 Hz, so IInput's own one-frame just-pressed flag can be raised and cleared
// between two polls — the edge has to be derived from the level state the poll actually observed.
class ActionEdgeTracker {
  public:
    // Records the action's current level and returns its rising edge.
    bool update(InputAction action, bool downNow) noexcept {
        const auto i = static_cast<size_t>(action);
        const bool edge = downNow && !m_prev[i];
        m_prev[i] = downNow;
        return edge;
    }

    [[nodiscard]] bool wasDown(InputAction action) const noexcept {
        return m_prev[static_cast<size_t>(action)];
    }

    void reset() noexcept {
        m_prev.fill(false);
    }

  private:
    std::array<bool, static_cast<size_t>(InputBindings::kActionCount)> m_prev{};
};

} // namespace fl
