// SPDX-License-Identifier: GPL-3.0-or-later
#include "InputBindings.h"
#include <iterator>
#include <sstream>
#include <toml++/toml.hpp>

namespace fl {

// ---------------------------------------------------------------------------
// Action table — name + the session modes the game reads the action in
// ---------------------------------------------------------------------------
//
// The context column is load-bearing, not documentation: it is what lets the conflict checker tell
// "Space is the gun and also the replay pause key" (fine — never live together) from "V is the radio
// PTT and also master arm" (#1050 — both live while flying, so keying the radio safed the guns).

namespace {

struct ActionInfo {
    const char* name;
    InputContext contexts;
};

constexpr ActionInfo kActionInfo[] = {
    // Continuous axes
    {"PitchAxis", kCtxFlight},
    {"RollAxis", kCtxFlight},
    {"YawAxis", kCtxFlight},
    {"ThrottleAxis", kCtxFlight},
    // Digital flight controls
    {"PitchUp", kCtxFlight},
    {"PitchDown", kCtxFlight},
    {"RollLeft", kCtxFlight},
    {"RollRight", kCtxFlight},
    {"YawLeft", kCtxFlight},
    {"YawRight", kCtxFlight},
    {"ThrottleUp", kCtxFlight},
    {"ThrottleDown", kCtxFlight},
    {"ThrottleMax", kCtxFlight},
    {"Airbrake", kCtxFlight},
    {"Afterburner", kCtxFlight},
    {"WheelBrake", kCtxFlight},
    // Weapons
    {"FireWeapon", kCtxFlight},
    {"FireStore", kCtxFlight},
    {"NextWeapon", kCtxFlight},
    {"PrevWeapon", kCtxFlight},
    {"MasterArm", kCtxFlight},
    {"RadarModeCycle", kCtxFlight},
    {"CountermeasureDispense", kCtxFlight},
    {"EcmToggle", kCtxFlight},
    // Airframe configuration
    {"LandingGear", kCtxFlight},
    {"Flaps", kCtxFlight},
    {"ArrestorHook", kCtxFlight},
    {"CanopyToggle", kCtxFlight},
    // Cockpit view pan — your own cockpit, so flight only
    {"ViewUp", kCtxFlight},
    {"ViewDown", kCtxFlight},
    {"ViewLeft", kCtxFlight},
    {"ViewRight", kCtxFlight},
    // Free-fly camera — reachable in every mode, including while flying, which is why these keys
    // must be distinct from the flight controls rather than merely "usually not pressed together".
    {"FreeCamForward", kCtxAnyMode},
    {"FreeCamBack", kCtxAnyMode},
    {"FreeCamLeft", kCtxAnyMode},
    {"FreeCamRight", kCtxAnyMode},
    {"FreeCamUp", kCtxAnyMode},
    {"FreeCamDown", kCtxAnyMode},
    {"FreeCamFaster", kCtxAnyMode},
    {"FreeCamSlower", kCtxAnyMode},
    {"FreeCamReset", kCtxAnyMode},
    // Shell / overlays
    {"Pause", kCtxAnyMode},
    {"ConsoleToggle", kCtxAnyMode},
    {"PerfOverlayCycle", kCtxAnyMode},
    // Comms
    {"WingmanMenu", kCtxFlight},
    {"CommsMenu", kCtxFlight},
    {"Eject", kCtxFlight},
    {"Respawn", kCtxFlight},
    {"Scoreboard", kCtxLive},
    {"ChatAll", kCtxLive},
    {"ChatTeam", kCtxLive},
    // Camera modes
    {"CameraCockpit", kCtxAnyMode},
    {"CameraChase", kCtxAnyMode},
    {"CameraFree", kCtxAnyMode},
    // Target designation family — needs an ownship
    {"PadlockToggle", kCtxFlight},
    {"NextTarget", kCtxFlight},
    {"PrevTarget", kCtxFlight},
    {"TargetInsetToggle", kCtxFlight},
    // Autopilot holds
    {"AutopilotAltHold", kCtxFlight},
    {"AutopilotHdgHold", kCtxFlight},
    {"AutopilotSpdHold", kCtxFlight},
    // Radar MFD
    {"MfdPage", kCtxFlight},
    {"MfdRange", kCtxFlight},
    // Night vision — a tonemap gain, so it applies to anything you are looking at
    {"NvgToggle", kCtxAnyMode},
    {"AircraftManual", kCtxFlight},
    {"ManualScrollUp", kCtxFlight},
    {"ManualScrollDown", kCtxFlight},
    {"GmMap", kCtxLive},
    // Multi-crew seats — an observer joins a gunner seat too
    {"CrewSeatCycle", kCtxLive},
    {"CrewSeatJoin", kCtxLive},
    {"CrewSeatLeave", kCtxLive},
    // Voice
    {"PushToTalkPrimary", kCtxLive},
    {"PushToTalkSecondary", kCtxLive},
    {"VoiceNetCycle", kCtxLive},
    {"WingmanVoiceCommand", kCtxFlight},
    // Spectator entity picker
    {"SpectateNext", kCtxNoOwnship},
    {"SpectatePrev", kCtxNoOwnship},
    // Replay transport. Seeking stays live inside photo mode; the SPEED keys do not, which is what
    // frees Minus/Equals for the photo exposure control.
    {"ReplayPauseToggle", kCtxPlayback},
    {"ReplaySeekBack", kCtxPlayback},
    {"ReplaySeekForward", kCtxPlayback},
    {"ReplaySeekBackFar", kCtxPlayback},
    {"ReplaySeekForwardFar", kCtxPlayback},
    {"ReplaySeekStart", kCtxPlayback},
    {"ReplaySeekEnd", kCtxPlayback},
    {"ReplaySpeedDown", InputContext::Replay},
    {"ReplaySpeedUp", InputContext::Replay},
    // Photo mode
    {"PhotoModeToggle", kCtxPlayback},
    {"PhotoFovIn", InputContext::Photo},
    {"PhotoFovOut", InputContext::Photo},
    {"PhotoFovFine", InputContext::Photo},
    {"PhotoRollLeft", InputContext::Photo},
    {"PhotoRollRight", InputContext::Photo},
    {"PhotoEvDown", InputContext::Photo},
    {"PhotoEvUp", InputContext::Photo},
    {"PhotoCapture", InputContext::Photo},
};
static_assert(std::size(kActionInfo) == static_cast<size_t>(InputAction::Count),
              "kActionInfo must have one entry per InputAction");

} // namespace

const char* InputBindings::actionName(InputAction action) {
    return kActionInfo[static_cast<int>(action)].name;
}

InputContext InputBindings::contexts(InputAction action) noexcept {
    return kActionInfo[static_cast<int>(action)].contexts;
}

std::optional<InputAction> InputBindings::actionFromName(const std::string& name) {
    for (int i = 0; i < static_cast<int>(InputAction::Count); ++i) {
        if (name == kActionInfo[i].name)
            return static_cast<InputAction>(i);
    }
    return std::nullopt;
}

const char* InputBindings::slotName(BindingSlot slot) {
    switch (slot) {
    case BindingSlot::Primary:
        return "primary";
    case BindingSlot::Secondary:
        return "secondary";
    case BindingSlot::Gamepad:
        return "gamepad";
    default:
        return "unknown";
    }
}

// ---------------------------------------------------------------------------
// Source / ID name tables
//
// One table per enum, scanned in both directions. Two hand-written switch statements per enum
// (name->id and id->name) are two chances to disagree; a single table cannot drift from itself.
// ---------------------------------------------------------------------------

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

template <typename E, size_t N> const char* nameOf(const NameRow<E> (&rows)[N], E value, const char* fallback) {
    for (const auto& r : rows)
        if (r.value == value)
            return r.name;
    return fallback;
}

template <typename E, size_t N> E valueOf(const NameRow<E> (&rows)[N], const std::string& name, E fallback) {
    for (const auto& r : rows)
        if (name == r.name)
            return r.value;
    return fallback;
}

} // namespace

// ---------------------------------------------------------------------------
// Binding serialization helpers
// ---------------------------------------------------------------------------

std::string InputBindings::serializeBinding(const Binding& b) {
    if (b.isNone())
        return "{ source = \"None\" }";
    std::string id;
    switch (b.source) {
    case BindingSource::Keyboard:
        id = nameOf(kKeyNames, static_cast<Key>(b.id), "Unknown");
        break;
    case BindingSource::MouseButton:
        id = nameOf(kMouseButtonNames, static_cast<MouseButton>(b.id), "Unknown");
        break;
    case BindingSource::GamepadButton:
        id = nameOf(kGamepadButtonNames, static_cast<GamepadButton>(b.id), "Unknown");
        break;
    case BindingSource::GamepadAxis:
        id = nameOf(kGamepadAxisNames, static_cast<GamepadAxis>(b.id), "Unknown");
        break;
    default:
        return "{ source = \"None\" }";
    }
    const std::string src = nameOf(kSourceNames, b.source, "None");
    if (b.source == BindingSource::GamepadAxis) {
        return "{ source = \"" + src + "\", id = \"" + id + "\", negative = " + (b.axisNegative ? "true" : "false") +
               " }";
    }
    return "{ source = \"" + src + "\", id = \"" + id + "\" }";
}

bool InputBindings::parseBinding(const std::string& source, const std::string& id, bool axisNegative, Binding& out) {
    if (source == "None" || source.empty()) {
        out = Binding{};
        return true;
    }
    if (source == "Keyboard") {
        Key k = valueOf(kKeyNames, id, Key::Unknown);
        if (k == Key::Unknown)
            return false;
        out = {BindingSource::Keyboard, static_cast<uint32_t>(k), false};
        return true;
    }
    if (source == "MouseButton") {
        MouseButton mb = valueOf(kMouseButtonNames, id, MouseButton::Count);
        if (mb == MouseButton::Count)
            return false;
        out = {BindingSource::MouseButton, static_cast<uint32_t>(mb), false};
        return true;
    }
    if (source == "GamepadButton") {
        GamepadButton gb = valueOf(kGamepadButtonNames, id, GamepadButton::Count);
        if (gb == GamepadButton::Count)
            return false;
        out = {BindingSource::GamepadButton, static_cast<uint32_t>(gb), false};
        return true;
    }
    if (source == "GamepadAxis") {
        GamepadAxis ga = valueOf(kGamepadAxisNames, id, GamepadAxis::Count);
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

namespace {

struct DefaultBinding {
    InputAction action;
    BindingSlot slot;
    Binding binding;
};

constexpr Binding kb(Key k) {
    return Binding{BindingSource::Keyboard, static_cast<uint32_t>(k), false};
}
constexpr Binding mb(MouseButton b) {
    return Binding{BindingSource::MouseButton, static_cast<uint32_t>(b), false};
}
constexpr Binding gb(GamepadButton b) {
    return Binding{BindingSource::GamepadButton, static_cast<uint32_t>(b), false};
}
constexpr Binding ga(GamepadAxis a) {
    return Binding{BindingSource::GamepadAxis, static_cast<uint32_t>(a), false};
}

constexpr BindingSlot kP = BindingSlot::Primary;
constexpr BindingSlot kS = BindingSlot::Secondary;
constexpr BindingSlot kG = BindingSlot::Gamepad;

// THE SHIPPED KEY MAP. `tests/test_input.cpp` asserts this table has no two actions on one input in
// an overlapping context, so a default added here is held to the same rule as a user rebind (#1050).
// It reads flat on purpose: this is the map a player and a reviewer both want to see in one place,
// and the previous scattered assignment is how V ended up on two live actions.
constexpr DefaultBinding kDefaults[] = {
    // ── Flight axes (gamepad) ────────────────────────────────────────────────
    {InputAction::PitchAxis, kG, ga(GamepadAxis::RightY)},
    {InputAction::RollAxis, kG, ga(GamepadAxis::RightX)},
    {InputAction::YawAxis, kG, ga(GamepadAxis::LeftX)},
    {InputAction::ThrottleAxis, kG, ga(GamepadAxis::TriggerLeft)},

    // ── Digital flight controls ──────────────────────────────────────────────
    // The arrows fly the aircraft and the keypad pans the view, not the other way round: the arrows
    // are what the game has always read for pitch/roll, and the table used to claim S/W/A/D while
    // ALSO claiming the arrows for the cockpit view pan — a documented double-duty that was really
    // two actions sharing a key.
    {InputAction::PitchUp, kP, kb(Key::ArrowDown)},
    {InputAction::PitchDown, kP, kb(Key::ArrowUp)},
    {InputAction::RollLeft, kP, kb(Key::ArrowLeft)},
    {InputAction::RollRight, kP, kb(Key::ArrowRight)},
    {InputAction::YawLeft, kP, kb(Key::Z)},
    {InputAction::YawRight, kP, kb(Key::X)},
    {InputAction::ThrottleUp, kP, kb(Key::PageUp)},
    {InputAction::ThrottleDown, kP, kb(Key::PageDown)},
    {InputAction::ThrottleMax, kP, kb(Key::LeftShift)},
    // K, not Space (#639): Space is the gun trigger, so an Airbrake default of Space was a conflict
    // waiting for the first person to read the table and believe it. K also puts the flight
    // configuration switches on one row: F flaps, G gear, H hook, K airbrake.
    {InputAction::Airbrake, kP, kb(Key::K)},
    {InputAction::Airbrake, kG, gb(GamepadButton::B)},
    {InputAction::Afterburner, kP, kb(Key::Tab)},
    {InputAction::Afterburner, kG, gb(GamepadButton::LeftShoulder)},
    {InputAction::WheelBrake, kP, kb(Key::B)},

    // ── Weapons ──────────────────────────────────────────────────────────────
    // Space AND the left mouse button, in two slots — the reason the table grew a third slot. With
    // two slots the mouse binding lived here and Space was hardcoded in the collector, invisible to
    // both the rebind UI and the conflict check.
    {InputAction::FireWeapon, kP, kb(Key::Space)},
    {InputAction::FireWeapon, kS, mb(MouseButton::Left)},
    {InputAction::FireWeapon, kG, gb(GamepadButton::RightShoulder)},
    {InputAction::FireStore, kP, kb(Key::Enter)},
    {InputAction::FireStore, kS, mb(MouseButton::Right)},
    {InputAction::FireStore, kG, gb(GamepadButton::LeftTrigger)},
    // D-pad, NOT the shoulders: RightShoulder is FireWeapon and LeftShoulder is Afterburner, and a
    // default that fires the gun every time you cycle weapons is a defect, not a binding (#625).
    {InputAction::NextWeapon, kP, kb(Key::Num1)},
    {InputAction::NextWeapon, kG, gb(GamepadButton::DpadRight)},
    {InputAction::PrevWeapon, kP, kb(Key::Num2)},
    {InputAction::PrevWeapon, kG, gb(GamepadButton::DpadLeft)},
    // Master arm (#641) moves off V and onto the armament digit row (1 next station, 2 previous,
    // 3 MFD range, 4 master arm). V stays with the radio: push-to-talk on V is a near-universal
    // convention, an ARM/SAFE switch on V is not, and only one of the two could keep the key.
    {InputAction::MasterArm, kP, kb(Key::Num4)},
    {InputAction::RadarModeCycle, kP, kb(Key::R)},
    // Chaff/flare moves off E, which is the free camera's pan-up key in every mode including
    // Flight — dispensing countermeasures while raising the camera was a live collision.
    {InputAction::CountermeasureDispense, kP, kb(Key::Delete)},
    {InputAction::EcmToggle, kP, kb(Key::J)},

    // ── Airframe configuration switches (#639) ───────────────────────────────
    {InputAction::LandingGear, kP, kb(Key::G)},
    {InputAction::LandingGear, kG, gb(GamepadButton::DpadDown)},
    {InputAction::Flaps, kP, kb(Key::F)},
    {InputAction::ArrestorHook, kP, kb(Key::H)},
    // The canopy was Shift+C, which the binding table cannot express — and a chord whose modifier is
    // itself a bound action (LeftShift = max throttle) cannot be made to behave. Its own key instead.
    {InputAction::CanopyToggle, kP, kb(Key::LeftBracket)},

    // ── Cockpit view pan ─────────────────────────────────────────────────────
    // The keypad cross, the classic flight-sim cockpit view scheme. Frees the arrows to fly.
    {InputAction::ViewUp, kP, kb(Key::Numpad8)},
    {InputAction::ViewDown, kP, kb(Key::Numpad2)},
    {InputAction::ViewLeft, kP, kb(Key::Numpad4)},
    {InputAction::ViewRight, kP, kb(Key::Numpad6)},

    // ── Free-fly camera ──────────────────────────────────────────────────────
    {InputAction::FreeCamForward, kP, kb(Key::W)},
    {InputAction::FreeCamBack, kP, kb(Key::S)},
    {InputAction::FreeCamLeft, kP, kb(Key::A)},
    {InputAction::FreeCamRight, kP, kb(Key::D)},
    {InputAction::FreeCamUp, kP, kb(Key::E)},
    {InputAction::FreeCamDown, kP, kb(Key::Q)},
    // Keypad +/-, not Minus/Equals: those are the replay speed and photo exposure controls, and the
    // free camera is live during playback too.
    {InputAction::FreeCamFaster, kP, kb(Key::NumpadPlus)},
    {InputAction::FreeCamSlower, kP, kb(Key::NumpadMinus)},
    {InputAction::FreeCamReset, kP, kb(Key::Insert)},

    // ── Shell / overlays ─────────────────────────────────────────────────────
    {InputAction::Pause, kP, kb(Key::Escape)},
    {InputAction::Pause, kG, gb(GamepadButton::Start)},
    {InputAction::ConsoleToggle, kP, kb(Key::Grave)},
    {InputAction::PerfOverlayCycle, kP, kb(Key::F3)},

    // ── Comms ────────────────────────────────────────────────────────────────
    {InputAction::WingmanMenu, kP, kb(Key::C)},
    {InputAction::WingmanMenu, kG, gb(GamepadButton::Back)},
    {InputAction::CommsMenu, kP, kb(Key::T)},
    // Ejection (#672): End — deliberate and out of the way, so it is never hit by accident on the
    // flight-control cluster. The server edge-detects it; a held key is one ejection, not many.
    {InputAction::Eject, kP, kb(Key::End)},
    {InputAction::Respawn, kP, kb(Key::Backspace)},
    {InputAction::Scoreboard, kP, kb(Key::I)},
    // Y all-channel, U team — U rather than H, because H is the arresting hook and both are live
    // while flying.
    {InputAction::ChatAll, kP, kb(Key::Y)},
    {InputAction::ChatTeam, kP, kb(Key::U)},

    // ── Camera modes (#689) ──────────────────────────────────────────────────
    {InputAction::CameraCockpit, kP, kb(Key::F1)},
    {InputAction::CameraChase, kP, kb(Key::F2)},
    {InputAction::CameraFree, kP, kb(Key::F4)},

    // ── Target designation family (#671/#689/#696) ───────────────────────────
    {InputAction::PadlockToggle, kP, kb(Key::F5)},
    {InputAction::PadlockToggle, kG, gb(GamepadButton::RightStick)},
    {InputAction::TargetInsetToggle, kP, kb(Key::F6)},
    {InputAction::NextTarget, kP, kb(Key::N)},
    {InputAction::NextTarget, kG, gb(GamepadButton::DpadUp)},
    {InputAction::PrevTarget, kP, kb(Key::P)},

    // ── Autopilot holds (#640) ───────────────────────────────────────────────
    // A/D/S were rejected: they are the free camera's pan keys, live in the same session.
    {InputAction::AutopilotAltHold, kP, kb(Key::F9)},
    {InputAction::AutopilotHdgHold, kP, kb(Key::F10)},
    {InputAction::AutopilotSpdHold, kP, kb(Key::F11)},

    // ── Avionics ─────────────────────────────────────────────────────────────
    {InputAction::MfdPage, kP, kb(Key::O)},
    {InputAction::MfdRange, kP, kb(Key::Num3)},
    {InputAction::NvgToggle, kP, kb(Key::F7)},
    {InputAction::AircraftManual, kP, kb(Key::M)},
    // The manual is non-modal, so its scroll keys cannot be PageUp/PageDown — those are the
    // throttle, and they stay live while it is open. The keypad's page positions instead.
    {InputAction::ManualScrollUp, kP, kb(Key::Numpad9)},
    {InputAction::ManualScrollDown, kP, kb(Key::Numpad3)},
    // The GM map moves off M and onto F12, joining the other overlay toggles on the function row.
    // The manual keeps M: every pilot opens it, only a GM-capable peer opens the map.
    {InputAction::GmMap, kP, kb(Key::F12)},

    // ── Multi-crew seats (#975) ──────────────────────────────────────────────
    // The bottom-row cluster, moved off K/L/U as a group: K is the airbrake and U is team chat.
    {InputAction::CrewSeatCycle, kP, kb(Key::Comma)},
    {InputAction::CrewSeatJoin, kP, kb(Key::Period)},
    {InputAction::CrewSeatLeave, kP, kb(Key::Slash)},

    // ── Voice (Epic J, #531) ─────────────────────────────────────────────────
    // All HELD, never latched: a latched mic is how a lobby ends up listening to someone's kitchen.
    {InputAction::PushToTalkPrimary, kP, kb(Key::V)},
    // LeftCtrl, not B: B is the wheel brake. A modifier-class key is a good PTT — comfortable to
    // hold, on the left hand beside the flight cluster, and bound to nothing else.
    {InputAction::PushToTalkSecondary, kP, kb(Key::LeftCtrl)},
    {InputAction::VoiceNetCycle, kP, kb(Key::Backslash)},
    {InputAction::WingmanVoiceCommand, kP, kb(Key::F8)},

    // ── Spectator picker (#860) ──────────────────────────────────────────────
    // Same keys as the weapon-station cycle, and that is not a collision: a spectator has no
    // ownship, so the two are never read in the same session mode.
    {InputAction::SpectateNext, kP, kb(Key::Num1)},
    {InputAction::SpectatePrev, kP, kb(Key::Num2)},

    // ── Replay transport (#41) ───────────────────────────────────────────────
    {InputAction::ReplayPauseToggle, kP, kb(Key::Space)},
    {InputAction::ReplaySeekBack, kP, kb(Key::ArrowLeft)},
    {InputAction::ReplaySeekForward, kP, kb(Key::ArrowRight)},
    {InputAction::ReplaySeekBackFar, kP, kb(Key::ArrowDown)},
    {InputAction::ReplaySeekForwardFar, kP, kb(Key::ArrowUp)},
    {InputAction::ReplaySeekStart, kP, kb(Key::Home)},
    {InputAction::ReplaySeekEnd, kP, kb(Key::End)},
    {InputAction::ReplaySpeedDown, kP, kb(Key::Minus)},
    {InputAction::ReplaySpeedUp, kP, kb(Key::Equals)},

    // ── Photo mode (#41) ─────────────────────────────────────────────────────
    {InputAction::PhotoModeToggle, kP, kb(Key::P)},
    {InputAction::PhotoFovIn, kP, kb(Key::PageUp)},
    {InputAction::PhotoFovOut, kP, kb(Key::PageDown)},
    {InputAction::PhotoFovFine, kP, kb(Key::LeftShift)},
    // Not Q/E: those move the free camera, which is what you frame the shot with.
    {InputAction::PhotoRollLeft, kP, kb(Key::Semicolon)},
    {InputAction::PhotoRollRight, kP, kb(Key::Apostrophe)},
    {InputAction::PhotoEvDown, kP, kb(Key::Minus)},
    {InputAction::PhotoEvUp, kP, kb(Key::Equals)},
    {InputAction::PhotoCapture, kP, kb(Key::Enter)},
};

} // namespace

InputBindings::InputBindings() {
    applyDefaults();
}

void InputBindings::applyDefaults() {
    for (auto& slot : m_slots)
        for (auto& b : slot)
            b = Binding{};
    for (const auto& d : kDefaults)
        m_slots[static_cast<int>(d.slot)][static_cast<int>(d.action)] = d.binding;
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

Binding InputBindings::get(InputAction action, BindingSlot slot) const {
    return m_slots[static_cast<int>(slot)][static_cast<int>(action)];
}

void InputBindings::set(InputAction action, Binding binding, BindingSlot slot) {
    m_slots[static_cast<int>(slot)][static_cast<int>(action)] = binding;
}

void InputBindings::clear(InputAction action, BindingSlot slot) {
    set(action, Binding{}, slot);
}

namespace {
bool sameInput(const Binding& a, const Binding& b) {
    return !a.isNone() && a.source == b.source && a.id == b.id && a.axisNegative == b.axisNegative;
}
} // namespace

std::optional<InputAction> InputBindings::conflictsWith(InputAction skipAction, const Binding& binding) const {
    if (binding.isNone())
        return std::nullopt;
    const InputContext want = contexts(skipAction);
    for (int i = 0; i < kActionCount; ++i) {
        const auto other = static_cast<InputAction>(i);
        if (other == skipAction)
            continue;
        // Sharing a key with an action the game never reads at the same time is legitimate reuse,
        // not a conflict — Space is the gun in flight and the pause key in a replay.
        if (!contextsOverlap(want, contexts(other)))
            continue;
        for (int s = 0; s < kSlotCount; ++s) {
            if (sameInput(binding, m_slots[s][i]))
                return other;
        }
    }
    return std::nullopt;
}

std::vector<InputBindings::Conflict> InputBindings::findConflicts() const {
    std::vector<Conflict> out;
    for (int i = 0; i < kActionCount; ++i) {
        for (int j = i + 1; j < kActionCount; ++j) {
            if (!contextsOverlap(contexts(static_cast<InputAction>(i)), contexts(static_cast<InputAction>(j))))
                continue;
            for (int si = 0; si < kSlotCount; ++si) {
                const Binding& a = m_slots[si][i];
                if (a.isNone())
                    continue;
                for (int sj = 0; sj < kSlotCount; ++sj) {
                    if (sameInput(a, m_slots[sj][j])) {
                        out.push_back({static_cast<InputAction>(i), static_cast<BindingSlot>(si),
                                       static_cast<InputAction>(j), static_cast<BindingSlot>(sj), a});
                    }
                }
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// TOML serialization
// ---------------------------------------------------------------------------

std::string InputBindings::serialize() const {
    std::ostringstream out;
    out << "# Fighters Legacy — input bindings\n"
           "# Edit this file to customise controls. Restart the game to apply changes.\n"
           "# File location: <user data>/config/bindings.toml\n"
           "#\n"
           "# Three slots per action: [primary] and [secondary] are keyboard/mouse, [gamepad] is the\n"
           "# pad or stick. Two actions may share an input only when the game never reads both in the\n"
           "# same session mode (flying / spectating / replay / photo); the game logs a warning at\n"
           "# startup for any binding that breaks that rule.\n"
           "#\n"
           "# `version` marks which shipped key map this file was written from. Lowering or removing it\n"
           "# makes the game regenerate the file from the current defaults.\n"
        << "version = " << kFormatVersion << "\n\n";

    for (int s = 0; s < kSlotCount; ++s) {
        out << "[" << slotName(static_cast<BindingSlot>(s)) << "]\n";
        for (int i = 0; i < kActionCount; ++i)
            out << actionName(static_cast<InputAction>(i)) << " = " << serializeBinding(m_slots[s][i]) << "\n";
        out << "\n";
    }
    return out.str();
}

int InputBindings::fileFormatVersion(const std::string& toml) {
    try {
        const toml::table tbl = toml::parse(toml);
        return tbl["version"].value_or(0);
    } catch (const toml::parse_error&) {
        return 0;
    }
}

bool InputBindings::deserialize(const std::string& toml) {
    toml::table tbl;
    try {
        tbl = toml::parse(toml);
    } catch (const toml::parse_error&) {
        return false;
    }

    // Parse a section into the given slot array.
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
    auto tmp = m_slots;
    if (!parseSection("primary", tmp[static_cast<int>(BindingSlot::Primary)]) ||
        !parseSection("secondary", tmp[static_cast<int>(BindingSlot::Secondary)]))
        return false;
    // `[alt]` was the gamepad section before the third slot existed; a file written by an older
    // build must not silently lose its pad bindings.
    const char* padSection = tbl["gamepad"].as_table() ? "gamepad" : "alt";
    if (!parseSection(padSection, tmp[static_cast<int>(BindingSlot::Gamepad)]))
        return false;
    m_slots = tmp;
    return true;
}

} // namespace fl
