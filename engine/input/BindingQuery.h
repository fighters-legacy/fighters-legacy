// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "AxisConfig.h" // AxisConfigTable / AxisSample — the analog path
#include "Binding.h"    // pulls in IInput.h + IJoystick.h (Key / MouseButton / Gamepad* / HatPosition)
#include "IJoystick.h"
#include "InputBindings.h"
#include "InputSources.h"
#include "JoystickDevices.h"

#include <array>
#include <cmath>
#include <vector>

// Resolve bindings against live input hardware (#689, extended #1050, extended #1061, #1358). One
// place translates a Binding's source into the matching HAL query, so every consumer edge-detects a
// rebindable, multi-device control identically instead of re-implementing the switch — or, as happened
// before #1050, reading a raw `Key::V` and bypassing the binding table altogether, or, as happened
// before #1061, reading four HOTAS axes by index out of a parallel config section.
//
// AN `Any`-DEVICE JOYSTICK BINDING READS EVERY PRESENT STICK (#1358). It used to resolve to whichever
// device enumerated first, which on a split stick+throttle HOTAS put all four flight axes on one
// physical unit and left the other one entirely unbound — USB enumeration order deciding which half of
// the rig works. The shipped defaults cannot name a GUID (they compile before any device exists), so
// `Any` has to mean what it says: the queries below scan the present devices and take the first one
// whose input is actually live. The conflict checker already treated `Any` as overlapping every
// device (devicesMayOverlap), so this is the read side catching up with the model.
//
// AXES HAVE NO RISING EDGE: bindingJustPressed returns false for a GamepadAxis or JoystickAxis
// binding, and bindingDown reports the axis crossed a ±0.5 threshold (respecting the negative-direction
// flag). HATS DO have an edge, derived by JoystickDevices from the position it saw last frame, because
// otherwise a POV hat could only ever drive held controls.

namespace fl {

// The live joystick index a binding's device names, or JoystickDevices::kAbsent.
//
// For an `Any` ref this is the FIRST present device only — callers that need one index. The query
// functions below do not use it for `Any`: they scan every present device (#1358). With no device
// table only an `Any` binding is addressable, and it resolves to device 0 — which is exactly what
// every joystick read did before #1061, so a caller that has not wired a table (a unit test, a
// headless tool) keeps the old behaviour rather than losing its stick entirely.
[[nodiscard]] inline int resolveJoystickIndex(const InputSources& src, const DeviceRef& ref) {
    if (src.devices)
        return src.devices->resolve(ref);
    return ref.isAny() ? 0 : JoystickDevices::kAbsent;
}

// Runs `fn(deviceIndex)` over every live device this ref can resolve to, stopping at the first that
// returns true. `Any` = every present device in live index order (#1358); a named GUID = that device
// alone; no device table = index 0, the documented test/headless fallback.
template <typename Fn>
[[nodiscard]] inline bool anyResolvedDevice(const InputSources& src, const DeviceRef& ref, Fn&& fn) {
    if (ref.isAny() && src.devices) {
        for (const auto& d : src.devices->present())
            if (fn(d.index))
                return true;
        return false;
    }
    const int dev = resolveJoystickIndex(src, ref);
    return dev != JoystickDevices::kAbsent && fn(dev);
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
        // A device that is not connected leaves the binding preserved but inert; `Any` reads every
        // present stick, so a trigger works no matter which unit of a split HOTAS it is on (#1358).
        return anyResolvedDevice(src, b.device,
                                 [&](int dev) { return src.joystick->isButtonDown(dev, static_cast<int>(b.id)); });
    }
    case BindingSource::JoystickAxis: {
        if (!src.joystick)
            return false;
        return anyResolvedDevice(src, b.device, [&](int dev) {
            const float v = src.joystick->getAxisValue(dev, static_cast<int>(b.id));
            return b.axisNegative ? v < -0.5f : v > 0.5f;
        });
    }
    case BindingSource::JoystickHat: {
        if (!src.joystick)
            return false;
        return anyResolvedDevice(src, b.device, [&](int dev) {
            return hatMatches(b.hat, src.joystick->getHatPosition(dev, static_cast<int>(b.id)));
        });
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
        return anyResolvedDevice(
            src, b.device, [&](int dev) { return src.joystick->isButtonJustPressed(dev, static_cast<int>(b.id)); });
    }
    case BindingSource::JoystickHat: {
        // The HAL has no just-pressed for hats, so the edge comes from the device table's previous
        // sample. Without it a hat could only drive held controls (#1061).
        if (!src.devices)
            return false;
        return anyResolvedDevice(src, b.device, [&](int dev) { return src.devices->hatJustPressed(dev, b.id, b.hat); });
    }
    case BindingSource::GamepadAxis:
    case BindingSource::JoystickAxis:
    default:
        return false; // an analog axis has no discrete rising edge
    }
}

// The raw, unprocessed value of an axis binding, or 0 when it is not an axis / its device is absent.
// For an `Any` joystick binding this is the FIRST present device's value; actionAxis() does not use it
// for that case — it samples every present device itself (#1358).
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

// Detects physical motion on an axis, so an Absolute-mode lever is in command only when it is the
// thing the player last touched (#1358).
//
// One reference raw value per axis, keyed by the config key PLUS the live device index. The index is
// load-bearing: two units of one model report the IDENTICAL SDL GUID (it encodes vendor/product, not
// a serial number), and a shared reference would see their two resting positions as endless motion —
// the throttle flapping between them, keyboard locked out, which is the very defect this class
// removes. update() reports motion when the live value sits more than kAxisMotionEpsilon from the
// reference, and moves the reference when it fires — comparing against the LAST FIRING point, not the
// last poll, so a slow lever sweep whose per-poll delta is tiny keeps firing every epsilon of travel
// while sensor noise never accumulates into a phantom touch. The first sighting of an axis only seeds
// the reference: a lever parked at 60% (or a dead channel stuck at −1.0) does not command anything
// until it MOVES.
//
// This is consumer-side state for the same reason ActionEdgeTracker is: the collector polls at 60 Hz
// while the device table updates per frame, so a frame-cadence "moved" flag could rise and clear
// between two polls and a fast lever slam would be lost.
class AxisMotionTracker {
  public:
    // Well above the couple-of-LSB noise floor of a real potentiometer or Hall sensor, and irrelevant
    // to a deliberate gesture: 0.0075 of raw travel is under 0.4% of the mapped throttle range.
    static constexpr float kAxisMotionEpsilon = 0.0075f;

    // Forget every reference when the connected device set changes. SDL renumbers indices on
    // hot-plug, so a kept reference could bill one device's parked position to another and
    // manufacture a phantom touch — the throttle snapping to a lever nobody moved on the unplug
    // frame. Reseeding costs one epsilon of the next deliberate gesture; trusting a stale reference
    // costs an uncommanded throttle change.
    void syncDeviceGeneration(uint64_t generation) {
        if (generation == m_generation)
            return;
        m_generation = generation;
        m_axes.clear();
    }

    // Records the axis's current raw value and returns whether it moved. Call once per poll per axis
    // observed — the reference goes stale for an axis that stops being polled, which only means its
    // next observation reports the motion that happened while it was masked.
    [[nodiscard]] bool update(const AxisKey& key, int deviceIndex, float raw) {
        for (auto& s : m_axes) {
            if (s.deviceIndex == deviceIndex && s.key == key) {
                if (std::abs(raw - s.reference) <= kAxisMotionEpsilon)
                    return false;
                s.reference = raw;
                return true;
            }
        }
        m_axes.push_back({key, deviceIndex, raw});
        return false; // first sighting seeds the reference; being present is not being touched
    }

    void reset() {
        m_axes.clear();
    }

  private:
    struct State {
        AxisKey key;
        int deviceIndex{0};
        float reference{0.0f};
    };
    std::vector<State> m_axes;
    uint64_t m_generation{0};
};

namespace detail {

// One axis sample: config lookup, the value transform, and the Absolute-mode command gate.
//
// A CENTERED axis is in command while it is outside its deadzone — its rest position is "not an
// input", so the gate is the value itself. An ABSOLUTE axis has no such rest position: every point of
// its travel is a valid setting, so "is it in command" cannot be read off the value and is gated on
// MOTION instead (#1358). The lever takes the control the moment it moves and releases it when it
// stops, which is what lets a rate control (the keyboard throttle) and a position control (the lever)
// drive the same accumulator — last mover wins. The alternative it replaces, Absolute = always in
// command, is how an unfitted channel stuck at −1.0 latched the throttle at idle and locked the
// keyboard out. With no tracker an Absolute axis NEVER asserts: a consumer that cannot remember
// motion must not let a mode claim a control unconditionally.
inline AxisSample sampleAxis(const AxisConfigTable& axes, const AxisKey& key, int deviceIndex, float raw,
                             AxisMotionTracker* motion) {
    const AxisConfig cfg = axes.effective(key);
    AxisSample s = cfg.apply(raw);
    if (cfg.mode == AxisMode::Absolute)
        s.active = motion && motion->update(key, deviceIndex, raw);
    return s;
}

} // namespace detail

// The analog value driving this action, processed through the per-axis config.
//
// Scans the action's bindings IN ORDER and returns the first ACTIVE one — so the list is the player's
// priority, and the shipped defaults put the joystick axis ahead of the gamepad axis for exactly the
// reason the pre-#1061 code applied HOTAS after gamepad: if there is a stick, the stick is the control.
// `active` is false when every axis is inside its deadzone, has an Absolute config but has not moved
// (see sampleAxis above, #1358), or the action has no axis binding at all — which is what lets the
// keyboard keep the control.
//
// An `Any` joystick binding samples EVERY present device and takes the first active one (#1358), so a
// split HOTAS works from whichever unit the player moves. Each device is sampled under its own GUID
// for the CONFIG lookup, which keeps a per-device [[axis_config]] entry authoritative over the
// wildcard tuning — while the MOTION reference is keyed by live index, because two units of one model
// share a GUID and the config is rightly shared between them but a motion reference must never be:
// one reference seeing two resting positions reads as endless motion, and the throttle flaps.
//
// `motion` may be null only for a caller with no Absolute-mode axes in play; without it they never
// assert (never the pre-#1358 always-on).
[[nodiscard]] inline AxisSample actionAxis(const InputSources& src, const InputBindings& bindings,
                                           const AxisConfigTable& axes, InputAction action,
                                           AxisMotionTracker* motion = nullptr) {
    // A hot-plug between two polls renumbers device indices; drop the motion references before any
    // of them can bill one device's parked position to another.
    if (motion && src.devices)
        motion->syncDeviceGeneration(src.devices->generation());
    for (const Binding& b : bindings.get(action)) {
        if (!isAxisSource(b.source))
            continue;
        if (b.source == BindingSource::GamepadAxis) {
            // A gamepad axis on a machine with no pad connected would read a flat 0 and, for an
            // Absolute config, claim to be an active idle throttle. Ask whether the pad is there.
            if (!src.input || src.input->getGamepadCount() <= 0)
                continue;
            const AxisSample s = detail::sampleAxis(axes, axisKeyOf(b), src.gamepadId, bindingAxisRaw(src, b), motion);
            if (s.active)
                return s;
            continue;
        }
        if (!src.joystick)
            continue;
        if (b.device.isAny() && src.devices) {
            bool found = false;
            AxisSample out{};
            for (const auto& d : src.devices->present()) {
                const float raw = src.joystick->getAxisValue(d.index, static_cast<int>(b.id));
                const DeviceRef ref = makeDeviceRef(d.guid.c_str());
                const AxisSample s = detail::sampleAxis(axes, AxisKey{b.source, b.id, ref}, d.index, raw, motion);
                if (s.active) {
                    out = s;
                    found = true;
                    break;
                }
            }
            if (found)
                return out;
            continue;
        }
        const int dev = resolveJoystickIndex(src, b.device);
        if (dev == JoystickDevices::kAbsent)
            continue;
        const float raw = src.joystick->getAxisValue(dev, static_cast<int>(b.id));
        const AxisSample s = detail::sampleAxis(axes, axisKeyOf(b), dev, raw, motion);
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
