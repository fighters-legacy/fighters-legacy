// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "IInput.h"
#include "IJoystick.h" // HatPosition — a hat direction is part of a Binding (#1061)
#include <cstdint>

namespace fl {

enum class BindingSource : uint8_t {
    None,
    Keyboard,
    MouseButton,
    GamepadButton,
    GamepadAxis,
    // Raw joystick / HOTAS (#1061). IJoystick is a PEER HAL to IInput covering devices whose axis and
    // button counts do not fit the fixed GamepadAxis / GamepadButton enums, so these three carry a raw
    // INDEX rather than an enum ordinal. Before #1061 they did not exist, which meant a HOTAS trigger
    // could not be bound to any action at all.
    JoystickButton,
    JoystickAxis,
    JoystickHat,
};

// 32 hex characters, the SDL GUID text form.
inline constexpr int kDeviceGuidChars = 32;

// Which physical device a binding names.
//
// Identity is the GUID, NOT the device index: SDL renumbers indices on hot-plug, so an index-keyed
// binding silently migrates to a different stick the first time one is unplugged. IJoystick's own
// comment on getJoystickGuid already says it is "for persistent binding saves so bindings survive
// device reconnect"; #1061 is the table taking it up.
//
// An EMPTY guid means "any device of this source's class" — EVERY present one, read by the query
// layer as a scan across the connected devices (#1358; it used to collapse onto whichever enumerated
// first, which left half of a split HOTAS unbound). That is not a convenience, it is what makes a
// shipped default expressible: the default table is compiled before any device exists, and a fresh
// install has to fly without the player rebinding anything, however many sticks are plugged in. A
// concrete GUID is what dedicates a binding to one stick, and keeps it there across a replug.
struct DeviceRef {
    char guid[kDeviceGuidChars + 1]{}; // NUL-terminated; empty = any device

    [[nodiscard]] constexpr bool isAny() const noexcept {
        return guid[0] == '\0';
    }
};

// Exact identity: two refs name the same thing. `Any` equals only `Any`.
[[nodiscard]] constexpr bool sameDeviceRef(const DeviceRef& a, const DeviceRef& b) noexcept {
    for (int i = 0; i <= kDeviceGuidChars; ++i) {
        if (a.guid[i] != b.guid[i])
            return false;
        if (a.guid[i] == '\0')
            return true;
    }
    return true;
}

// COULD these two refs resolve to the same physical device? `Any` may resolve to any device, so it
// overlaps everything. Used by the conflict checker, which must err toward reporting a clash.
[[nodiscard]] constexpr bool devicesMayOverlap(const DeviceRef& a, const DeviceRef& b) noexcept {
    return a.isAny() || b.isAny() || sameDeviceRef(a, b);
}

// Truncating copy from a NUL-terminated GUID string; nullptr or "" yields `Any`.
[[nodiscard]] constexpr DeviceRef makeDeviceRef(const char* guid) noexcept {
    DeviceRef out{};
    if (!guid)
        return out;
    int i = 0;
    for (; i < kDeviceGuidChars && guid[i] != '\0'; ++i)
        out.guid[i] = guid[i];
    out.guid[i] = '\0';
    return out;
}

struct Binding {
    BindingSource source{BindingSource::None};
    // Keyboard / MouseButton / GamepadButton / GamepadAxis: the enum ordinal cast to uint32_t.
    // JoystickButton / JoystickAxis / JoystickHat: the raw zero-based index on the device.
    uint32_t id{0};
    bool axisNegative{false};               // true = the negative axis direction triggers a digital action
    HatPosition hat{HatPosition::Centered}; // JoystickHat only: the direction that triggers. Centered = unset.
    DeviceRef device{};                     // joystick sources only; empty elsewhere

    [[nodiscard]] constexpr bool isNone() const {
        return source == BindingSource::None;
    }
};

// Keyboard and mouse are the DESKTOP sources: a text field owns them (and the pointer that clicks its
// Send button) while it has focus, but it does not own the stick, so a partner keeps flying while you
// type. The suppression is per source, which is why this predicate exists rather than an
// is-this-a-pad test — a HOTAS is no more suppressible by a chat box than a gamepad is.
[[nodiscard]] constexpr bool isDesktopSource(BindingSource s) noexcept {
    return s == BindingSource::Keyboard || s == BindingSource::MouseButton;
}

[[nodiscard]] constexpr bool isJoystickSource(BindingSource s) noexcept {
    return s == BindingSource::JoystickButton || s == BindingSource::JoystickAxis || s == BindingSource::JoystickHat;
}

// Sources that carry an analog value. An axis has no discrete rising edge, so bindingJustPressed
// reports false for one and consumers derive an edge from the level they observed.
[[nodiscard]] constexpr bool isAxisSource(BindingSource s) noexcept {
    return s == BindingSource::GamepadAxis || s == BindingSource::JoystickAxis;
}

// True when this source addresses a specific device, i.e. when Binding::device is meaningful.
[[nodiscard]] constexpr bool sourceUsesDevice(BindingSource s) noexcept {
    return isJoystickSource(s);
}

// Does a live hat position satisfy a binding on `bound`?
//
// A CARDINAL binding also matches its two adjacent diagonals, because a POV hat pressed a few degrees
// off is still a press: a four-way view control bound to Up/Down/Left/Right must not go dead the
// moment the player's thumb rolls onto UpRight. A DIAGONAL binding matches only itself — someone who
// deliberately bound UpRight asked for the corner, not for the quadrant.
[[nodiscard]] constexpr bool hatMatches(HatPosition bound, HatPosition live) noexcept {
    if (bound == HatPosition::Centered || live == HatPosition::Centered)
        return false;
    if (bound == live)
        return true;
    switch (bound) {
    case HatPosition::Up:
        return live == HatPosition::UpRight || live == HatPosition::UpLeft;
    case HatPosition::Right:
        return live == HatPosition::UpRight || live == HatPosition::DownRight;
    case HatPosition::Down:
        return live == HatPosition::DownRight || live == HatPosition::DownLeft;
    case HatPosition::Left:
        return live == HatPosition::DownLeft || live == HatPosition::UpLeft;
    default:
        return false; // a diagonal binding is exact
    }
}

// Two bindings that fire from the same physical input. The device comparison is deliberately the
// OVERLAP test, not exact identity: "any device, axis 3" and "that specific stick, axis 3" can be the
// same axis at runtime, and a conflict checker that missed that would let the defaults ship a clash.
[[nodiscard]] constexpr bool sameInput(const Binding& a, const Binding& b) noexcept {
    if (a.isNone() || b.isNone())
        return false;
    if (a.source != b.source || a.id != b.id)
        return false;
    if (isAxisSource(a.source) && a.axisNegative != b.axisNegative)
        return false;
    if (a.source == BindingSource::JoystickHat && !hatMatches(a.hat, b.hat) && !hatMatches(b.hat, a.hat))
        return false;
    if (sourceUsesDevice(a.source) && !devicesMayOverlap(a.device, b.device))
        return false;
    return true;
}

// Exact value equality, for erasing a specific binding from an action's list.
[[nodiscard]] constexpr bool sameBinding(const Binding& a, const Binding& b) noexcept {
    return a.source == b.source && a.id == b.id && a.axisNegative == b.axisNegative && a.hat == b.hat &&
           sameDeviceRef(a.device, b.device);
}

} // namespace fl
