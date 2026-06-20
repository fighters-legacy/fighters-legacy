// SPDX-License-Identifier: GPL-3.0-or-later
#include "InputBindings.h"
#include <sstream>
#include <toml++/toml.hpp>

namespace fl {

// ---------------------------------------------------------------------------
// Action name table
// ---------------------------------------------------------------------------

static constexpr const char* kActionNames[] = {
    "PitchAxis",  "RollAxis",    "YawAxis",    "ThrottleAxis", "PitchUp",      "PitchDown", "RollLeft",
    "RollRight",  "YawLeft",     "YawRight",   "ThrottleUp",   "ThrottleDown", "Airbrake",  "Afterburner",
    "FireWeapon", "FireMissile", "NextWeapon", "PrevWeapon",   "ViewUp",       "ViewDown",  "ViewLeft",
    "ViewRight",  "LandingGear", "Flaps",      "Pause",        "Menu",
};
static_assert(std::size(kActionNames) == static_cast<size_t>(InputAction::Count),
              "kActionNames must have one entry per InputAction");

const char* InputBindings::actionName(InputAction action) {
    return kActionNames[static_cast<int>(action)];
}

std::optional<InputAction> InputBindings::actionFromName(const std::string& name) {
    for (int i = 0; i < static_cast<int>(InputAction::Count); ++i) {
        if (name == kActionNames[i])
            return static_cast<InputAction>(i);
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Source / ID name tables
// ---------------------------------------------------------------------------

static const char* sourceName(BindingSource s) {
    switch (s) {
    case BindingSource::Keyboard:
        return "Keyboard";
    case BindingSource::MouseButton:
        return "MouseButton";
    case BindingSource::GamepadButton:
        return "GamepadButton";
    case BindingSource::GamepadAxis:
        return "GamepadAxis";
    default:
        return "None";
    }
}

static const char* keyName(Key k) {
    switch (k) {
    case Key::A:
        return "A";
    case Key::B:
        return "B";
    case Key::C:
        return "C";
    case Key::D:
        return "D";
    case Key::E:
        return "E";
    case Key::F:
        return "F";
    case Key::G:
        return "G";
    case Key::H:
        return "H";
    case Key::I:
        return "I";
    case Key::J:
        return "J";
    case Key::K:
        return "K";
    case Key::L:
        return "L";
    case Key::M:
        return "M";
    case Key::N:
        return "N";
    case Key::O:
        return "O";
    case Key::P:
        return "P";
    case Key::Q:
        return "Q";
    case Key::R:
        return "R";
    case Key::S:
        return "S";
    case Key::T:
        return "T";
    case Key::U:
        return "U";
    case Key::V:
        return "V";
    case Key::W:
        return "W";
    case Key::X:
        return "X";
    case Key::Y:
        return "Y";
    case Key::Z:
        return "Z";
    case Key::Num0:
        return "0";
    case Key::Num1:
        return "1";
    case Key::Num2:
        return "2";
    case Key::Num3:
        return "3";
    case Key::Num4:
        return "4";
    case Key::Num5:
        return "5";
    case Key::Num6:
        return "6";
    case Key::Num7:
        return "7";
    case Key::Num8:
        return "8";
    case Key::Num9:
        return "9";
    case Key::Space:
        return "Space";
    case Key::Enter:
        return "Enter";
    case Key::Tab:
        return "Tab";
    case Key::Backspace:
        return "Backspace";
    case Key::Delete:
        return "Delete";
    case Key::Escape:
        return "Escape";
    case Key::ArrowUp:
        return "ArrowUp";
    case Key::ArrowDown:
        return "ArrowDown";
    case Key::ArrowLeft:
        return "ArrowLeft";
    case Key::ArrowRight:
        return "ArrowRight";
    case Key::Home:
        return "Home";
    case Key::End:
        return "End";
    case Key::PageUp:
        return "PageUp";
    case Key::PageDown:
        return "PageDown";
    case Key::Insert:
        return "Insert";
    case Key::F1:
        return "F1";
    case Key::F2:
        return "F2";
    case Key::F3:
        return "F3";
    case Key::F4:
        return "F4";
    case Key::F5:
        return "F5";
    case Key::F6:
        return "F6";
    case Key::F7:
        return "F7";
    case Key::F8:
        return "F8";
    case Key::F9:
        return "F9";
    case Key::F10:
        return "F10";
    case Key::F11:
        return "F11";
    case Key::F12:
        return "F12";
    case Key::LeftShift:
        return "LeftShift";
    case Key::RightShift:
        return "RightShift";
    case Key::LeftCtrl:
        return "LeftCtrl";
    case Key::RightCtrl:
        return "RightCtrl";
    case Key::LeftAlt:
        return "LeftAlt";
    case Key::RightAlt:
        return "RightAlt";
    default:
        return "Unknown";
    }
}

static Key keyFromName(const std::string& n) {
    if (n == "A")
        return Key::A;
    if (n == "B")
        return Key::B;
    if (n == "C")
        return Key::C;
    if (n == "D")
        return Key::D;
    if (n == "E")
        return Key::E;
    if (n == "F")
        return Key::F;
    if (n == "G")
        return Key::G;
    if (n == "H")
        return Key::H;
    if (n == "I")
        return Key::I;
    if (n == "J")
        return Key::J;
    if (n == "K")
        return Key::K;
    if (n == "L")
        return Key::L;
    if (n == "M")
        return Key::M;
    if (n == "N")
        return Key::N;
    if (n == "O")
        return Key::O;
    if (n == "P")
        return Key::P;
    if (n == "Q")
        return Key::Q;
    if (n == "R")
        return Key::R;
    if (n == "S")
        return Key::S;
    if (n == "T")
        return Key::T;
    if (n == "U")
        return Key::U;
    if (n == "V")
        return Key::V;
    if (n == "W")
        return Key::W;
    if (n == "X")
        return Key::X;
    if (n == "Y")
        return Key::Y;
    if (n == "Z")
        return Key::Z;
    if (n == "0")
        return Key::Num0;
    if (n == "1")
        return Key::Num1;
    if (n == "2")
        return Key::Num2;
    if (n == "3")
        return Key::Num3;
    if (n == "4")
        return Key::Num4;
    if (n == "5")
        return Key::Num5;
    if (n == "6")
        return Key::Num6;
    if (n == "7")
        return Key::Num7;
    if (n == "8")
        return Key::Num8;
    if (n == "9")
        return Key::Num9;
    if (n == "Space")
        return Key::Space;
    if (n == "Enter")
        return Key::Enter;
    if (n == "Tab")
        return Key::Tab;
    if (n == "Backspace")
        return Key::Backspace;
    if (n == "Delete")
        return Key::Delete;
    if (n == "Escape")
        return Key::Escape;
    if (n == "ArrowUp")
        return Key::ArrowUp;
    if (n == "ArrowDown")
        return Key::ArrowDown;
    if (n == "ArrowLeft")
        return Key::ArrowLeft;
    if (n == "ArrowRight")
        return Key::ArrowRight;
    if (n == "Home")
        return Key::Home;
    if (n == "End")
        return Key::End;
    if (n == "PageUp")
        return Key::PageUp;
    if (n == "PageDown")
        return Key::PageDown;
    if (n == "Insert")
        return Key::Insert;
    if (n == "F1")
        return Key::F1;
    if (n == "F2")
        return Key::F2;
    if (n == "F3")
        return Key::F3;
    if (n == "F4")
        return Key::F4;
    if (n == "F5")
        return Key::F5;
    if (n == "F6")
        return Key::F6;
    if (n == "F7")
        return Key::F7;
    if (n == "F8")
        return Key::F8;
    if (n == "F9")
        return Key::F9;
    if (n == "F10")
        return Key::F10;
    if (n == "F11")
        return Key::F11;
    if (n == "F12")
        return Key::F12;
    if (n == "LeftShift")
        return Key::LeftShift;
    if (n == "RightShift")
        return Key::RightShift;
    if (n == "LeftCtrl")
        return Key::LeftCtrl;
    if (n == "RightCtrl")
        return Key::RightCtrl;
    if (n == "LeftAlt")
        return Key::LeftAlt;
    if (n == "RightAlt")
        return Key::RightAlt;
    return Key::Unknown;
}

static const char* mouseButtonName(MouseButton b) {
    switch (b) {
    case MouseButton::Left:
        return "Left";
    case MouseButton::Middle:
        return "Middle";
    case MouseButton::Right:
        return "Right";
    default:
        return "Unknown";
    }
}

static MouseButton mouseButtonFromName(const std::string& n) {
    if (n == "Left")
        return MouseButton::Left;
    if (n == "Middle")
        return MouseButton::Middle;
    if (n == "Right")
        return MouseButton::Right;
    return MouseButton::Count;
}

static const char* gamepadButtonName(GamepadButton b) {
    switch (b) {
    case GamepadButton::A:
        return "A";
    case GamepadButton::B:
        return "B";
    case GamepadButton::X:
        return "X";
    case GamepadButton::Y:
        return "Y";
    case GamepadButton::LeftShoulder:
        return "LeftShoulder";
    case GamepadButton::RightShoulder:
        return "RightShoulder";
    case GamepadButton::LeftTrigger:
        return "LeftTrigger";
    case GamepadButton::RightTrigger:
        return "RightTrigger";
    case GamepadButton::LeftStick:
        return "LeftStick";
    case GamepadButton::RightStick:
        return "RightStick";
    case GamepadButton::DpadUp:
        return "DpadUp";
    case GamepadButton::DpadDown:
        return "DpadDown";
    case GamepadButton::DpadLeft:
        return "DpadLeft";
    case GamepadButton::DpadRight:
        return "DpadRight";
    case GamepadButton::Start:
        return "Start";
    case GamepadButton::Back:
        return "Back";
    default:
        return "Unknown";
    }
}

static GamepadButton gamepadButtonFromName(const std::string& n) {
    if (n == "A")
        return GamepadButton::A;
    if (n == "B")
        return GamepadButton::B;
    if (n == "X")
        return GamepadButton::X;
    if (n == "Y")
        return GamepadButton::Y;
    if (n == "LeftShoulder")
        return GamepadButton::LeftShoulder;
    if (n == "RightShoulder")
        return GamepadButton::RightShoulder;
    if (n == "LeftTrigger")
        return GamepadButton::LeftTrigger;
    if (n == "RightTrigger")
        return GamepadButton::RightTrigger;
    if (n == "LeftStick")
        return GamepadButton::LeftStick;
    if (n == "RightStick")
        return GamepadButton::RightStick;
    if (n == "DpadUp")
        return GamepadButton::DpadUp;
    if (n == "DpadDown")
        return GamepadButton::DpadDown;
    if (n == "DpadLeft")
        return GamepadButton::DpadLeft;
    if (n == "DpadRight")
        return GamepadButton::DpadRight;
    if (n == "Start")
        return GamepadButton::Start;
    if (n == "Back")
        return GamepadButton::Back;
    return GamepadButton::Count;
}

static const char* gamepadAxisName(GamepadAxis a) {
    switch (a) {
    case GamepadAxis::LeftX:
        return "LeftX";
    case GamepadAxis::LeftY:
        return "LeftY";
    case GamepadAxis::RightX:
        return "RightX";
    case GamepadAxis::RightY:
        return "RightY";
    case GamepadAxis::TriggerLeft:
        return "TriggerLeft";
    case GamepadAxis::TriggerRight:
        return "TriggerRight";
    default:
        return "Unknown";
    }
}

static GamepadAxis gamepadAxisFromName(const std::string& n) {
    if (n == "LeftX")
        return GamepadAxis::LeftX;
    if (n == "LeftY")
        return GamepadAxis::LeftY;
    if (n == "RightX")
        return GamepadAxis::RightX;
    if (n == "RightY")
        return GamepadAxis::RightY;
    if (n == "TriggerLeft")
        return GamepadAxis::TriggerLeft;
    if (n == "TriggerRight")
        return GamepadAxis::TriggerRight;
    return GamepadAxis::Count;
}

// ---------------------------------------------------------------------------
// Binding serialization helpers
// ---------------------------------------------------------------------------

std::string InputBindings::serializeBinding(const Binding& b) {
    if (b.isNone())
        return "{ source = \"None\" }";
    std::string id;
    switch (b.source) {
    case BindingSource::Keyboard:
        id = keyName(static_cast<Key>(b.id));
        break;
    case BindingSource::MouseButton:
        id = mouseButtonName(static_cast<MouseButton>(b.id));
        break;
    case BindingSource::GamepadButton:
        id = gamepadButtonName(static_cast<GamepadButton>(b.id));
        break;
    case BindingSource::GamepadAxis:
        id = gamepadAxisName(static_cast<GamepadAxis>(b.id));
        break;
    default:
        return "{ source = \"None\" }";
    }
    if (b.source == BindingSource::GamepadAxis) {
        return "{ source = \"" + std::string(sourceName(b.source)) + "\", id = \"" + id +
               "\", negative = " + (b.axisNegative ? "true" : "false") + " }";
    }
    return "{ source = \"" + std::string(sourceName(b.source)) + "\", id = \"" + id + "\" }";
}

bool InputBindings::parseBinding(const std::string& source, const std::string& id, bool axisNegative, Binding& out) {
    if (source == "None" || source.empty()) {
        out = Binding{};
        return true;
    }
    if (source == "Keyboard") {
        Key k = keyFromName(id);
        if (k == Key::Unknown)
            return false;
        out = {BindingSource::Keyboard, static_cast<uint32_t>(k), false};
        return true;
    }
    if (source == "MouseButton") {
        MouseButton mb = mouseButtonFromName(id);
        if (mb == MouseButton::Count)
            return false;
        out = {BindingSource::MouseButton, static_cast<uint32_t>(mb), false};
        return true;
    }
    if (source == "GamepadButton") {
        GamepadButton gb = gamepadButtonFromName(id);
        if (gb == GamepadButton::Count)
            return false;
        out = {BindingSource::GamepadButton, static_cast<uint32_t>(gb), false};
        return true;
    }
    if (source == "GamepadAxis") {
        GamepadAxis ga = gamepadAxisFromName(id);
        if (ga == GamepadAxis::Count)
            return false;
        out = {BindingSource::GamepadAxis, static_cast<uint32_t>(ga), axisNegative};
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Constructor / defaults
// ---------------------------------------------------------------------------

InputBindings::InputBindings() {
    applyDefaults();
}

void InputBindings::applyDefaults() {
    for (auto& b : m_primary)
        b = Binding{};
    for (auto& b : m_alt)
        b = Binding{};

    // Keyboard primary defaults
    m_primary[static_cast<int>(InputAction::PitchUp)] = {BindingSource::Keyboard, static_cast<uint32_t>(Key::S), false};
    m_primary[static_cast<int>(InputAction::PitchDown)] = {BindingSource::Keyboard, static_cast<uint32_t>(Key::W),
                                                           false};
    m_primary[static_cast<int>(InputAction::RollLeft)] = {BindingSource::Keyboard, static_cast<uint32_t>(Key::A),
                                                          false};
    m_primary[static_cast<int>(InputAction::RollRight)] = {BindingSource::Keyboard, static_cast<uint32_t>(Key::D),
                                                           false};
    m_primary[static_cast<int>(InputAction::YawLeft)] = {BindingSource::Keyboard, static_cast<uint32_t>(Key::Q), false};
    m_primary[static_cast<int>(InputAction::YawRight)] = {BindingSource::Keyboard, static_cast<uint32_t>(Key::E),
                                                          false};
    m_primary[static_cast<int>(InputAction::ThrottleUp)] = {BindingSource::Keyboard,
                                                            static_cast<uint32_t>(Key::ArrowUp), false};
    m_primary[static_cast<int>(InputAction::ThrottleDown)] = {BindingSource::Keyboard,
                                                              static_cast<uint32_t>(Key::ArrowDown), false};
    m_primary[static_cast<int>(InputAction::Airbrake)] = {BindingSource::Keyboard, static_cast<uint32_t>(Key::Space),
                                                          false};
    m_primary[static_cast<int>(InputAction::Afterburner)] = {BindingSource::Keyboard,
                                                             static_cast<uint32_t>(Key::LeftShift), false};
    m_primary[static_cast<int>(InputAction::FireWeapon)] = {BindingSource::MouseButton,
                                                            static_cast<uint32_t>(MouseButton::Left), false};
    m_primary[static_cast<int>(InputAction::FireMissile)] = {BindingSource::MouseButton,
                                                             static_cast<uint32_t>(MouseButton::Right), false};
    m_primary[static_cast<int>(InputAction::NextWeapon)] = {BindingSource::Keyboard, static_cast<uint32_t>(Key::Num1),
                                                            false};
    m_primary[static_cast<int>(InputAction::PrevWeapon)] = {BindingSource::Keyboard, static_cast<uint32_t>(Key::Num2),
                                                            false};
    m_primary[static_cast<int>(InputAction::LandingGear)] = {BindingSource::Keyboard, static_cast<uint32_t>(Key::G),
                                                             false};
    m_primary[static_cast<int>(InputAction::Flaps)] = {BindingSource::Keyboard, static_cast<uint32_t>(Key::F), false};
    m_primary[static_cast<int>(InputAction::Pause)] = {BindingSource::Keyboard, static_cast<uint32_t>(Key::Escape),
                                                       false};
    m_primary[static_cast<int>(InputAction::Menu)] = {BindingSource::Keyboard, static_cast<uint32_t>(Key::Tab), false};

    // Gamepad alt defaults
    m_alt[static_cast<int>(InputAction::PitchAxis)] = {BindingSource::GamepadAxis,
                                                       static_cast<uint32_t>(GamepadAxis::RightY), false};
    m_alt[static_cast<int>(InputAction::RollAxis)] = {BindingSource::GamepadAxis,
                                                      static_cast<uint32_t>(GamepadAxis::RightX), false};
    m_alt[static_cast<int>(InputAction::YawAxis)] = {BindingSource::GamepadAxis,
                                                     static_cast<uint32_t>(GamepadAxis::LeftX), false};
    m_alt[static_cast<int>(InputAction::ThrottleAxis)] = {BindingSource::GamepadAxis,
                                                          static_cast<uint32_t>(GamepadAxis::TriggerLeft), false};
    m_alt[static_cast<int>(InputAction::FireWeapon)] = {BindingSource::GamepadButton,
                                                        static_cast<uint32_t>(GamepadButton::RightShoulder), false};
    m_alt[static_cast<int>(InputAction::FireMissile)] = {BindingSource::GamepadButton,
                                                         static_cast<uint32_t>(GamepadButton::LeftTrigger), false};
    m_alt[static_cast<int>(InputAction::NextWeapon)] = {BindingSource::GamepadButton,
                                                        static_cast<uint32_t>(GamepadButton::RightShoulder), false};
    m_alt[static_cast<int>(InputAction::PrevWeapon)] = {BindingSource::GamepadButton,
                                                        static_cast<uint32_t>(GamepadButton::LeftShoulder), false};
    m_alt[static_cast<int>(InputAction::LandingGear)] = {BindingSource::GamepadButton,
                                                         static_cast<uint32_t>(GamepadButton::DpadDown), false};
    m_alt[static_cast<int>(InputAction::Airbrake)] = {BindingSource::GamepadButton,
                                                      static_cast<uint32_t>(GamepadButton::B), false};
    m_alt[static_cast<int>(InputAction::Afterburner)] = {BindingSource::GamepadButton,
                                                         static_cast<uint32_t>(GamepadButton::LeftShoulder), false};
    m_alt[static_cast<int>(InputAction::Pause)] = {BindingSource::GamepadButton,
                                                   static_cast<uint32_t>(GamepadButton::Start), false};
    m_alt[static_cast<int>(InputAction::Menu)] = {BindingSource::GamepadButton,
                                                  static_cast<uint32_t>(GamepadButton::Back), false};
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

Binding InputBindings::get(InputAction action, bool alt) const {
    return alt ? m_alt[static_cast<int>(action)] : m_primary[static_cast<int>(action)];
}

void InputBindings::set(InputAction action, Binding binding, bool alt) {
    if (alt)
        m_alt[static_cast<int>(action)] = binding;
    else
        m_primary[static_cast<int>(action)] = binding;
}

void InputBindings::clear(InputAction action, bool alt) {
    set(action, Binding{}, alt);
}

std::optional<InputAction> InputBindings::conflictsWith(InputAction skipAction, const Binding& binding) const {
    if (binding.isNone())
        return std::nullopt;
    for (int i = 0; i < kActionCount; ++i) {
        if (static_cast<InputAction>(i) == skipAction)
            continue;
        auto check = [&](const Binding& b) -> bool {
            return b.source == binding.source && b.id == binding.id && b.axisNegative == binding.axisNegative;
        };
        if (check(m_primary[i]) || check(m_alt[i]))
            return static_cast<InputAction>(i);
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// TOML serialization
// ---------------------------------------------------------------------------

std::string InputBindings::serialize() const {
    std::ostringstream out;
    out << "# Fighters Legacy — input bindings\n"
           "# Edit this file to customise controls. Restart the game to apply changes.\n"
           "# File location: <user data>/config/bindings.toml\n\n";

    out << "[primary]\n";
    for (int i = 0; i < kActionCount; ++i) {
        out << kActionNames[i] << " = " << serializeBinding(m_primary[i]) << "\n";
    }
    out << "\n[alt]\n";
    for (int i = 0; i < kActionCount; ++i) {
        out << kActionNames[i] << " = " << serializeBinding(m_alt[i]) << "\n";
    }
    return out.str();
}

bool InputBindings::deserialize(const std::string& toml) {
    toml::table tbl;
    try {
        tbl = toml::parse(toml);
    } catch (const toml::parse_error&) {
        return false;
    }

    // Parse a section ([primary] or [alt]) into the given array.
    auto parseSection = [&](const char* section, std::array<Binding, kActionCount>& target) -> bool {
        auto* sec = tbl[section].as_table();
        if (!sec)
            return true; // section absent is fine; leave defaults
        for (auto& [key, val] : *sec) {
            auto actionOpt = actionFromName(std::string(key.str()));
            if (!actionOpt)
                continue;
            auto* entry = val.as_table();
            if (!entry)
                continue;
            std::string src = entry->get("source") ? entry->get("source")->value_or(std::string{}) : std::string{};
            std::string id = entry->get("id") ? entry->get("id")->value_or(std::string{}) : std::string{};
            bool neg = entry->get("negative") ? entry->get("negative")->value_or(false) : false;
            Binding b;
            if (!parseBinding(src, id, neg, b))
                return false;
            target[static_cast<int>(*actionOpt)] = b;
        }
        return true;
    };

    // Parse into temporaries so a mid-parse failure leaves the original intact.
    auto tmpPrimary = m_primary;
    auto tmpAlt = m_alt;
    if (!parseSection("primary", tmpPrimary) || !parseSection("alt", tmpAlt))
        return false;
    m_primary = tmpPrimary;
    m_alt = tmpAlt;
    return true;
}

} // namespace fl
