// SPDX-License-Identifier: GPL-3.0-or-later
#include "InputNames.h"
#include <iterator>

namespace fl {

namespace {

template <typename E> struct NameRow {
    E value;
    const char* name;
};

constexpr NameRow<BindingSource> kSourceNames[] = {
    {BindingSource::None, "None"},
    {BindingSource::Keyboard, "Keyboard"},
    {BindingSource::MouseButton, "MouseButton"},
    {BindingSource::GamepadButton, "GamepadButton"},
    {BindingSource::GamepadAxis, "GamepadAxis"},
    {BindingSource::JoystickButton, "JoystickButton"},
    {BindingSource::JoystickAxis, "JoystickAxis"},
    {BindingSource::JoystickHat, "JoystickHat"},
};

constexpr NameRow<Key> kKeyNames[] = {
    {Key::A, "A"},
    {Key::B, "B"},
    {Key::C, "C"},
    {Key::D, "D"},
    {Key::E, "E"},
    {Key::F, "F"},
    {Key::G, "G"},
    {Key::H, "H"},
    {Key::I, "I"},
    {Key::J, "J"},
    {Key::K, "K"},
    {Key::L, "L"},
    {Key::M, "M"},
    {Key::N, "N"},
    {Key::O, "O"},
    {Key::P, "P"},
    {Key::Q, "Q"},
    {Key::R, "R"},
    {Key::S, "S"},
    {Key::T, "T"},
    {Key::U, "U"},
    {Key::V, "V"},
    {Key::W, "W"},
    {Key::X, "X"},
    {Key::Y, "Y"},
    {Key::Z, "Z"},
    {Key::Num0, "0"},
    {Key::Num1, "1"},
    {Key::Num2, "2"},
    {Key::Num3, "3"},
    {Key::Num4, "4"},
    {Key::Num5, "5"},
    {Key::Num6, "6"},
    {Key::Num7, "7"},
    {Key::Num8, "8"},
    {Key::Num9, "9"},
    {Key::Space, "Space"},
    {Key::Enter, "Enter"},
    {Key::Tab, "Tab"},
    {Key::Backspace, "Backspace"},
    {Key::Delete, "Delete"},
    {Key::Escape, "Escape"},
    {Key::ArrowUp, "ArrowUp"},
    {Key::ArrowDown, "ArrowDown"},
    {Key::ArrowLeft, "ArrowLeft"},
    {Key::ArrowRight, "ArrowRight"},
    {Key::Home, "Home"},
    {Key::End, "End"},
    {Key::PageUp, "PageUp"},
    {Key::PageDown, "PageDown"},
    {Key::Insert, "Insert"},
    {Key::F1, "F1"},
    {Key::F2, "F2"},
    {Key::F3, "F3"},
    {Key::F4, "F4"},
    {Key::F5, "F5"},
    {Key::F6, "F6"},
    {Key::F7, "F7"},
    {Key::F8, "F8"},
    {Key::F9, "F9"},
    {Key::F10, "F10"},
    {Key::F11, "F11"},
    {Key::F12, "F12"},
    {Key::Minus, "Minus"},
    {Key::Equals, "Equals"},
    {Key::Comma, "Comma"},
    {Key::Period, "Period"},
    {Key::Slash, "Slash"},
    {Key::Semicolon, "Semicolon"},
    {Key::Apostrophe, "Apostrophe"},
    {Key::LeftBracket, "LeftBracket"},
    {Key::RightBracket, "RightBracket"},
    {Key::Backslash, "Backslash"},
    {Key::Grave, "Grave"},
    {Key::Numpad0, "Numpad0"},
    {Key::Numpad1, "Numpad1"},
    {Key::Numpad2, "Numpad2"},
    {Key::Numpad3, "Numpad3"},
    {Key::Numpad4, "Numpad4"},
    {Key::Numpad5, "Numpad5"},
    {Key::Numpad6, "Numpad6"},
    {Key::Numpad7, "Numpad7"},
    {Key::Numpad8, "Numpad8"},
    {Key::Numpad9, "Numpad9"},
    {Key::NumpadPlus, "NumpadPlus"},
    {Key::NumpadMinus, "NumpadMinus"},
    {Key::NumpadMultiply, "NumpadMultiply"},
    {Key::NumpadDivide, "NumpadDivide"},
    {Key::NumpadPeriod, "NumpadPeriod"},
    {Key::NumpadEnter, "NumpadEnter"},
    {Key::LeftShift, "LeftShift"},
    {Key::RightShift, "RightShift"},
    {Key::LeftCtrl, "LeftCtrl"},
    {Key::RightCtrl, "RightCtrl"},
    {Key::LeftAlt, "LeftAlt"},
    {Key::RightAlt, "RightAlt"},
};
// Every Key except Unknown and Count must be nameable, or a default could serialize as "Unknown"
// and fail to round-trip through bindings.toml.
static_assert(std::size(kKeyNames) == static_cast<size_t>(Key::Count) - 1,
              "kKeyNames must name every Key value except Unknown");

constexpr NameRow<MouseButton> kMouseButtonNames[] = {
    {MouseButton::Left, "Left"},
    {MouseButton::Middle, "Middle"},
    {MouseButton::Right, "Right"},
};
static_assert(std::size(kMouseButtonNames) == static_cast<size_t>(MouseButton::Count));

constexpr NameRow<GamepadButton> kGamepadButtonNames[] = {
    {GamepadButton::A, "A"},
    {GamepadButton::B, "B"},
    {GamepadButton::X, "X"},
    {GamepadButton::Y, "Y"},
    {GamepadButton::LeftShoulder, "LeftShoulder"},
    {GamepadButton::RightShoulder, "RightShoulder"},
    {GamepadButton::LeftTrigger, "LeftTrigger"},
    {GamepadButton::RightTrigger, "RightTrigger"},
    {GamepadButton::LeftStick, "LeftStick"},
    {GamepadButton::RightStick, "RightStick"},
    {GamepadButton::DpadUp, "DpadUp"},
    {GamepadButton::DpadDown, "DpadDown"},
    {GamepadButton::DpadLeft, "DpadLeft"},
    {GamepadButton::DpadRight, "DpadRight"},
    {GamepadButton::Start, "Start"},
    {GamepadButton::Back, "Back"},
};
static_assert(std::size(kGamepadButtonNames) == static_cast<size_t>(GamepadButton::Count));

constexpr NameRow<GamepadAxis> kGamepadAxisNames[] = {
    {GamepadAxis::LeftX, "LeftX"},
    {GamepadAxis::LeftY, "LeftY"},
    {GamepadAxis::RightX, "RightX"},
    {GamepadAxis::RightY, "RightY"},
    {GamepadAxis::TriggerLeft, "TriggerLeft"},
    {GamepadAxis::TriggerRight, "TriggerRight"},
};
static_assert(std::size(kGamepadAxisNames) == static_cast<size_t>(GamepadAxis::Count));

constexpr NameRow<HatPosition> kHatNames[] = {
    {HatPosition::Centered, "Centered"},   {HatPosition::Up, "Up"},
    {HatPosition::UpRight, "UpRight"},     {HatPosition::Right, "Right"},
    {HatPosition::DownRight, "DownRight"}, {HatPosition::Down, "Down"},
    {HatPosition::DownLeft, "DownLeft"},   {HatPosition::Left, "Left"},
    {HatPosition::UpLeft, "UpLeft"},
};

template <typename E, size_t N> const char* nameOf(const NameRow<E> (&rows)[N], E value) {
    for (const auto& r : rows)
        if (r.value == value)
            return r.name;
    return nullptr;
}

template <typename E, size_t N> std::optional<E> valueOf(const NameRow<E> (&rows)[N], const std::string& name) {
    for (const auto& r : rows)
        if (name == r.name)
            return r.value;
    return std::nullopt;
}

} // namespace

const char* bindingSourceName(BindingSource s) {
    const char* n = nameOf(kSourceNames, s);
    return n ? n : "None";
}
std::optional<BindingSource> bindingSourceFromName(const std::string& name) {
    return valueOf(kSourceNames, name);
}

const char* keyName(Key k) {
    return nameOf(kKeyNames, k);
}
std::optional<Key> keyFromName(const std::string& name) {
    return valueOf(kKeyNames, name);
}

const char* mouseButtonName(MouseButton b) {
    return nameOf(kMouseButtonNames, b);
}
std::optional<MouseButton> mouseButtonFromName(const std::string& name) {
    return valueOf(kMouseButtonNames, name);
}

const char* gamepadButtonName(GamepadButton b) {
    return nameOf(kGamepadButtonNames, b);
}
std::optional<GamepadButton> gamepadButtonFromName(const std::string& name) {
    return valueOf(kGamepadButtonNames, name);
}

const char* gamepadAxisName(GamepadAxis a) {
    return nameOf(kGamepadAxisNames, a);
}
std::optional<GamepadAxis> gamepadAxisFromName(const std::string& name) {
    return valueOf(kGamepadAxisNames, name);
}

const char* hatPositionName(HatPosition p) {
    const char* n = nameOf(kHatNames, p);
    return n ? n : "Centered";
}
std::optional<HatPosition> hatPositionFromName(const std::string& name) {
    return valueOf(kHatNames, name);
}

} // namespace fl
