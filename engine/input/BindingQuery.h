// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "AxisConfig.h" // AxisConfigTable / AxisSample — the analog path
#include "Binding.h"    // pulls in IInput.h + IJoystick.h (Key / MouseButton / Gamepad* / HatPosition)
#include "IJoystick.h"
#include "InputBindings.h"
#include "InputSources.h"
#include "JoystickDevices.h"

#include <array>

// Resolve bindings against live input hardware (#689, extended #1050, extended #1061). One place
// translates a Binding's source into the matching HAL query, so every consumer edge-detects a
// rebindable, multi-device control identically instead of re-implementing the switch — or, as happened
// before #1050, reading a raw `Key::V` and bypassing the binding table altogether, or, as happened
// before #1061, reading four HOTAS axes by index out of a parallel config section.
//
// AXES HAVE NO RISING EDGE: bindingJustPressed returns false for a GamepadAxis or JoystickAxis
// binding, and bindingDown reports the axis crossed a ±0.5 threshold (respecting the negative-direction
// flag). HATS DO have an edge, derived by JoystickDevices from the position it saw last frame, because
// otherwise a POV hat could only ever drive held controls.

namespace fl {

// The live joystick index a binding's device names, or JoystickDevices::kAbsent.
//
// With no device table only an `Any` binding is addressable, and it resolves to device 0 — which is
// exactly what every joystick read did before #1061, so a caller that has not wired a table (a unit
// test, a headless tool) keeps the old behaviour rather than losing its stick entirely.
[[nodiscard]] inline int resolveJoystickIndex(const InputSources& src, const DeviceRef& ref) {
    if (src.devices)
        return src.devices->resolve(ref);
    return ref.isAny() ? 0 : JoystickDevices::kAbsent;
}

// --- Single-binding resolution ---------------------------------------------------------------

[[nodiscard]] inline bool bindingDown(const InputSources& src, const Binding& b) {
    switch (b.source) {
    case BindingSource::Keyboard:
        return src.input && src.input->isKeyDown(static_cast<Key>(b.id));
    case BindingSource::MouseButton:
        return src.input && src.input->isMouseButtonDown(static_cast<MouseButton>(b.id));
    case BindingSource::GamepadButton:
        return src.input && src.input->isGamepadButtonDown(src.gamepadId, static_cast<GamepadButton>(b.id));
    case BindingSource::GamepadAxis: {
        if (!src.input)
            return false;
        const float v = src.input->getGamepadAxis(src.gamepadId, static_cast<GamepadAxis>(b.id));
        return b.axisNegative ? v < -0.5f : v > 0.5f;
    }
    case BindingSource::JoystickButton: {
        if (!src.joystick)
            return false;
        const int dev = resolveJoystickIndex(src, b.device);
        if (dev == JoystickDevices::kAbsent)
            return false; // the device is not connected: the binding is preserved but inert
        return src.joystick->isButtonDown(dev, static_cast<int>(b.id));
    }
    case BindingSource::JoystickAxis: {
        if (!src.joystick)
            return false;
        const int dev = resolveJoystickIndex(src, b.device);
        if (dev == JoystickDevices::kAbsent)
            return false;
        const float v = src.joystick->getAxisValue(dev, static_cast<int>(b.id));
        return b.axisNegative ? v < -0.5f : v > 0.5f;
    }
    case BindingSource::JoystickHat: {
        if (!src.joystick)
            return false;
        const int dev = resolveJoystickIndex(src, b.device);
        if (dev == JoystickDevices::kAbsent)
            return false;
        return hatMatches(b.hat, src.joystick->getHatPosition(dev, static_cast<int>(b.id)));
    }
    default:
        return false;
    }
}

[[nodiscard]] inline bool bindingJustPressed(const InputSources& src, const Binding& b) {
    switch (b.source) {
    case BindingSource::Keyboard:
        return src.input && src.input->isKeyJustPressed(static_cast<Key>(b.id));
    case BindingSource::MouseButton:
        return src.input && src.input->isMouseButtonJustPressed(static_cast<MouseButton>(b.id));
    case BindingSource::GamepadButton:
        return src.input && src.input->isGamepadButtonJustPressed(src.gamepadId, static_cast<GamepadButton>(b.id));
    case BindingSource::JoystickButton: {
        if (!src.joystick)
            return false;
        const int dev = resolveJoystickIndex(src, b.device);
        if (dev == JoystickDevices::kAbsent)
            return false;
        return src.joystick->isButtonJustPressed(dev, static_cast<int>(b.id));
    }
    case BindingSource::JoystickHat: {
        // The HAL has no just-pressed for hats, so the edge comes from the device table's previous
        // sample. Without it a hat could only drive held controls (#1061).
        if (!src.devices)
            return false;
        const int dev = src.devices->resolve(b.device);
        if (dev == JoystickDevices::kAbsent)
            return false;
        return src.devices->hatJustPressed(dev, b.id, b.hat);
    }
    case BindingSource::GamepadAxis:
    case BindingSource::JoystickAxis:
    default:
        return false; // an analog axis has no discrete rising edge
    }
}

// The raw, unprocessed value of an axis binding, or 0 when it is not an axis / its device is absent.
[[nodiscard]] inline float bindingAxisRaw(const InputSources& src, const Binding& b) {
    if (b.source == BindingSource::GamepadAxis)
        return src.input ? src.input->getGamepadAxis(src.gamepadId, static_cast<GamepadAxis>(b.id)) : 0.0f;
    if (b.source == BindingSource::JoystickAxis) {
        if (!src.joystick)
            return 0.0f;
        const int dev = resolveJoystickIndex(src, b.device);
        if (dev == JoystickDevices::kAbsent)
            return 0.0f;
        return src.joystick->getAxisValue(dev, static_cast<int>(b.id));
    }
    return 0.0f;
}

// True when the binding's device is connected (or the source does not name one). A binding on an
// absent device is preserved and inert — never pruned, because silently destroying a HOTAS map the
// first time someone launches without the stick plugged in is the worse failure.
[[nodiscard]] inline bool bindingDevicePresent(const InputSources& src, const Binding& b) {
    if (!sourceUsesDevice(b.source))
        return true;
    if (!src.joystick)
        return false;
    if (!src.devices)
        return b.device.isAny();
    return src.devices->isPresent(b.device);
}

// --- Action resolution -----------------------------------------------------------------------

// True when ANY binding of the action is active. `suppressDesktop` drops the keyboard and mouse
// bindings only — a chat box owns the keyboard and the pointer that clicks its Send button, not the
// stick, so a partner keeps flying from the pad or the HOTAS while you type.
[[nodiscard]] inline bool actionDown(const InputSources& src, const InputBindings& bindings, InputAction action,
                                     bool suppressDesktop = false) {
    for (const Binding& b : bindings.get(action)) {
        if (suppressDesktop && isDesktopSource(b.source))
            continue;
        if (bindingDown(src, b))
            return true;
    }
    return false;
}

[[nodiscard]] inline bool actionJustPressed(const InputSources& src, const InputBindings& bindings, InputAction action,
                                            bool suppressDesktop = false) {
    for (const Binding& b : bindings.get(action)) {
        if (suppressDesktop && isDesktopSource(b.source))
            continue;
        if (bindingJustPressed(src, b))
            return true;
    }
    return false;
}

// The analog value driving this action, processed through the per-axis config.
//
// Scans the action's bindings IN ORDER and returns the first ACTIVE one — so the list is the player's
// priority, and the shipped defaults put the joystick axis ahead of the gamepad axis for exactly the
// reason the pre-#1061 code applied HOTAS after gamepad: if there is a stick, the stick is the control.
// `active` is false when every axis is inside its deadzone (or the action has no axis binding at all),
// which is what lets the keyboard keep the control.
[[nodiscard]] inline AxisSample actionAxis(const InputSources& src, const InputBindings& bindings,
                                           const AxisConfigTable& axes, InputAction action) {
    for (const Binding& b : bindings.get(action)) {
        if (!isAxisSource(b.source))
            continue;
        if (!bindingDevicePresent(src, b))
            continue;
        // A gamepad axis on a machine with no pad connected would read a flat 0 and, for an Absolute
        // config, claim to be an active idle throttle. Ask whether the pad is there at all.
        if (b.source == BindingSource::GamepadAxis && (!src.input || src.input->getGamepadCount() <= 0))
            continue;
        const AxisSample s = axes.effective(axisKeyOf(b)).apply(bindingAxisRaw(src, b));
        if (s.active)
            return s;
    }
    return {};
}

// Level-plus-own-edge tracking, for consumers that do not sample every frame. FlightInputCollector
// is rate-limited to 60 Hz, so a HAL's own one-frame just-pressed flag can be raised and cleared
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
