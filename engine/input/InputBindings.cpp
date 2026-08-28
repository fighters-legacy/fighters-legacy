// SPDX-License-Identifier: GPL-3.0-or-later
#include "InputBindings.h"
#include "AxisConfig.h" // kHotasAxis* — the shipped HOTAS axis layout, stated once
#include "InputNames.h"
#include <algorithm>
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

// ---------------------------------------------------------------------------
// Binding serialization
// ---------------------------------------------------------------------------

std::string InputBindings::serializeBinding(const Binding& b) {
    std::ostringstream out;
    out << "{ source = \"" << bindingSourceName(b.source) << "\"";
    switch (b.source) {
    case BindingSource::Keyboard: {
        const char* n = keyName(static_cast<Key>(b.id));
        out << ", id = \"" << (n ? n : "Unknown") << "\"";
        break;
    }
    case BindingSource::MouseButton: {
        const char* n = mouseButtonName(static_cast<MouseButton>(b.id));
        out << ", id = \"" << (n ? n : "Unknown") << "\"";
        break;
    }
    case BindingSource::GamepadButton: {
        const char* n = gamepadButtonName(static_cast<GamepadButton>(b.id));
        out << ", id = \"" << (n ? n : "Unknown") << "\"";
        break;
    }
    case BindingSource::GamepadAxis: {
        const char* n = gamepadAxisName(static_cast<GamepadAxis>(b.id));
        out << ", id = \"" << (n ? n : "Unknown") << "\"";
        out << ", negative = " << (b.axisNegative ? "true" : "false");
        break;
    }
    case BindingSource::JoystickButton:
        out << ", index = " << b.id;
        break;
    case BindingSource::JoystickAxis:
        out << ", index = " << b.id << ", negative = " << (b.axisNegative ? "true" : "false");
        break;
    case BindingSource::JoystickHat:
        out << ", index = " << b.id << ", direction = \"" << hatPositionName(b.hat) << "\"";
        break;
    case BindingSource::None:
    default:
        return "{ source = \"None\" }";
    }
    if (sourceUsesDevice(b.source) && !b.device.isAny())
        out << ", device = \"" << b.device.guid << "\"";
    out << " }";
    return out.str();
}

namespace {

std::string tomlString(const toml::table& t, const char* key) {
    const auto* n = t.get(key);
    return n ? n->value_or(std::string{}) : std::string{};
}

// Parses one binding table. Returns false on anything it does not recognise: a name we cannot resolve
// must not degrade into a plausible-looking different control.
bool parseBindingTable(const toml::table& entry, Binding& out) {
    const std::string srcName = tomlString(entry, "source");
    if (srcName.empty() || srcName == "None") {
        out = Binding{};
        return true;
    }
    const auto src = bindingSourceFromName(srcName);
    if (!src)
        return false;

    Binding b{};
    b.source = *src;
    const std::string id = tomlString(entry, "id");
    const auto* negNode = entry.get("negative");
    b.axisNegative = negNode ? negNode->value_or(false) : false;

    switch (*src) {
    case BindingSource::Keyboard: {
        const auto k = keyFromName(id);
        if (!k)
            return false;
        b.id = static_cast<uint32_t>(*k);
        b.axisNegative = false;
        break;
    }
    case BindingSource::MouseButton: {
        const auto m = mouseButtonFromName(id);
        if (!m)
            return false;
        b.id = static_cast<uint32_t>(*m);
        b.axisNegative = false;
        break;
    }
    case BindingSource::GamepadButton: {
        const auto g = gamepadButtonFromName(id);
        if (!g)
            return false;
        b.id = static_cast<uint32_t>(*g);
        b.axisNegative = false;
        break;
    }
    case BindingSource::GamepadAxis: {
        const auto a = gamepadAxisFromName(id);
        if (!a)
            return false;
        b.id = static_cast<uint32_t>(*a);
        break;
    }
    case BindingSource::JoystickButton:
    case BindingSource::JoystickAxis:
    case BindingSource::JoystickHat: {
        const auto* idxNode = entry.get("index");
        const long long idx = idxNode ? idxNode->value_or(-1LL) : -1LL;
        if (idx < 0)
            return false;
        b.id = static_cast<uint32_t>(idx);
        if (*src != BindingSource::JoystickAxis)
            b.axisNegative = false;
        if (*src == BindingSource::JoystickHat) {
            const auto dir = hatPositionFromName(tomlString(entry, "direction"));
            // A hat binding with no direction, or one on Centered, names nothing: a centered hat is a
            // hat nobody is touching, so it would be a control that can never fire.
            if (!dir || *dir == HatPosition::Centered)
                return false;
            b.hat = *dir;
        }
        b.device = makeDeviceRef(tomlString(entry, "device").c_str());
        break;
    }
    case BindingSource::None:
    default:
        out = Binding{};
        return true;
    }
    out = b;
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Constructor / defaults
// ---------------------------------------------------------------------------

namespace {

struct DefaultBinding {
    InputAction action;
    Binding binding;
};

constexpr Binding kb(Key k) {
    return Binding{BindingSource::Keyboard, static_cast<uint32_t>(k), false, HatPosition::Centered, DeviceRef{}};
}
constexpr Binding mb(MouseButton b) {
    return Binding{BindingSource::MouseButton, static_cast<uint32_t>(b), false, HatPosition::Centered, DeviceRef{}};
}
constexpr Binding gb(GamepadButton b) {
    return Binding{BindingSource::GamepadButton, static_cast<uint32_t>(b), false, HatPosition::Centered, DeviceRef{}};
}
constexpr Binding ga(GamepadAxis a) {
    return Binding{BindingSource::GamepadAxis, static_cast<uint32_t>(a), false, HatPosition::Centered, DeviceRef{}};
}
// Raw joystick axis on ANY device (#1061). A shipped default cannot name a GUID — the table is
// compiled before any device exists — and a fresh install with one stick must fly with no rebinding.
constexpr Binding ja(uint32_t axis) {
    return Binding{BindingSource::JoystickAxis, axis, false, HatPosition::Centered, DeviceRef{}};
}

// THE SHIPPED KEY MAP. `tests/test_input.cpp` asserts this table has no two actions on one input in
// an overlapping context, so a default added here is held to the same rule as a user rebind (#1050).
// It reads flat on purpose: this is the map a player and a reviewer both want to see in one place,
// and the previous scattered assignment is how V ended up on two live actions.
//
// Since #1061 an action's rows are simply consecutive, in priority order, with no slot column — the
// gun's five bindings are five adjacent lines rather than three named pigeonholes and a hardcoded
// leftover. For the four ANALOG AXES the joystick row comes FIRST deliberately: if a HOTAS is plugged
// in it is the control, which is exactly what the pre-#1061 "HOTAS overrides gamepad" ordering did.
constexpr DefaultBinding kDefaults[] = {
    // ── Flight axes ──────────────────────────────────────────────────────────
    // The stick axes reach the table for the first time in #1061; before it they lived in
    // `[controls] hotas_*_axis` in user.toml, read by index on device 0, invisible to this table and
    // to the conflict checker — the last parallel input-config path in the game.
    {InputAction::PitchAxis, ja(kHotasAxisPitch)},
    {InputAction::PitchAxis, ga(GamepadAxis::RightY)},
    {InputAction::RollAxis, ja(kHotasAxisRoll)},
    {InputAction::RollAxis, ga(GamepadAxis::RightX)},
    {InputAction::YawAxis, ja(kHotasAxisYaw)},
    {InputAction::YawAxis, ga(GamepadAxis::LeftX)},
    {InputAction::ThrottleAxis, ja(kHotasAxisThrottle)},
    {InputAction::ThrottleAxis, ga(GamepadAxis::TriggerLeft)},

    // ── Digital flight controls ──────────────────────────────────────────────
    // The arrows fly the aircraft and the keypad pans the view, not the other way round: the arrows
    // are what the game has always read for pitch/roll, and the table used to claim S/W/A/D while
    // ALSO claiming the arrows for the cockpit view pan — a documented double-duty that was really
    // two actions sharing a key.
    {InputAction::PitchUp, kb(Key::ArrowDown)},
    {InputAction::PitchDown, kb(Key::ArrowUp)},
    {InputAction::RollLeft, kb(Key::ArrowLeft)},
    {InputAction::RollRight, kb(Key::ArrowRight)},
    {InputAction::YawLeft, kb(Key::Z)},
    {InputAction::YawRight, kb(Key::X)},
    {InputAction::ThrottleUp, kb(Key::PageUp)},
    {InputAction::ThrottleDown, kb(Key::PageDown)},
    {InputAction::ThrottleMax, kb(Key::LeftShift)},
    // K, not Space (#639): Space is the gun trigger, so an Airbrake default of Space was a conflict
    // waiting for the first person to read the table and believe it. K also puts the flight
    // configuration switches on one row: F flaps, G gear, H hook, K airbrake.
    {InputAction::Airbrake, kb(Key::K)},
    {InputAction::Airbrake, gb(GamepadButton::B)},
    {InputAction::Afterburner, kb(Key::Tab)},
    {InputAction::Afterburner, gb(GamepadButton::LeftShoulder)},
    {InputAction::WheelBrake, kb(Key::B)},

    // ── Weapons ──────────────────────────────────────────────────────────────
    // Space AND the left mouse button AND a shoulder button — three bindings on one action, which is
    // the case that outgrew two slots in #1050 and now simply lists. A fourth and fifth (a HOTAS
    // trigger on each of two sticks) is a line each, which is what #1061 exists for.
    {InputAction::FireWeapon, kb(Key::Space)},
    {InputAction::FireWeapon, mb(MouseButton::Left)},
    {InputAction::FireWeapon, gb(GamepadButton::RightShoulder)},
    {InputAction::FireStore, kb(Key::Enter)},
    {InputAction::FireStore, mb(MouseButton::Right)},
    {InputAction::FireStore, gb(GamepadButton::LeftTrigger)},
    // D-pad, NOT the shoulders: RightShoulder is FireWeapon and LeftShoulder is Afterburner, and a
    // default that fires the gun every time you cycle weapons is a defect, not a binding (#625).
    {InputAction::NextWeapon, kb(Key::Num1)},
    {InputAction::NextWeapon, gb(GamepadButton::DpadRight)},
    {InputAction::PrevWeapon, kb(Key::Num2)},
    {InputAction::PrevWeapon, gb(GamepadButton::DpadLeft)},
    // Master arm (#641) moves off V and onto the armament digit row (1 next station, 2 previous,
    // 3 MFD range, 4 master arm). V stays with the radio: push-to-talk on V is a near-universal
    // convention, an ARM/SAFE switch on V is not, and only one of the two could keep the key.
    {InputAction::MasterArm, kb(Key::Num4)},
    {InputAction::RadarModeCycle, kb(Key::R)},
    // Chaff/flare moves off E, which is the free camera's pan-up key in every mode including
    // Flight — dispensing countermeasures while raising the camera was a live collision.
    {InputAction::CountermeasureDispense, kb(Key::Delete)},
    {InputAction::EcmToggle, kb(Key::J)},

    // ── Airframe configuration switches (#639) ───────────────────────────────
    {InputAction::LandingGear, kb(Key::G)},
    {InputAction::LandingGear, gb(GamepadButton::DpadDown)},
    {InputAction::Flaps, kb(Key::F)},
    {InputAction::ArrestorHook, kb(Key::H)},
    // The canopy was Shift+C, which the binding table cannot express — and a chord whose modifier is
    // itself a bound action (LeftShift = max throttle) cannot be made to behave. Its own key instead.
    {InputAction::CanopyToggle, kb(Key::LeftBracket)},

    // ── Cockpit view pan ─────────────────────────────────────────────────────
    // The keypad cross, the classic flight-sim cockpit view scheme. Frees the arrows to fly.
    {InputAction::ViewUp, kb(Key::Numpad8)},
    {InputAction::ViewDown, kb(Key::Numpad2)},
    {InputAction::ViewLeft, kb(Key::Numpad4)},
    {InputAction::ViewRight, kb(Key::Numpad6)},

    // ── Free-fly camera ──────────────────────────────────────────────────────
    {InputAction::FreeCamForward, kb(Key::W)},
    {InputAction::FreeCamBack, kb(Key::S)},
    {InputAction::FreeCamLeft, kb(Key::A)},
    {InputAction::FreeCamRight, kb(Key::D)},
    {InputAction::FreeCamUp, kb(Key::E)},
    {InputAction::FreeCamDown, kb(Key::Q)},
    // Keypad +/-, not Minus/Equals: those are the replay speed and photo exposure controls, and the
    // free camera is live during playback too.
    {InputAction::FreeCamFaster, kb(Key::NumpadPlus)},
    {InputAction::FreeCamSlower, kb(Key::NumpadMinus)},
    {InputAction::FreeCamReset, kb(Key::Insert)},

    // ── Shell / overlays ─────────────────────────────────────────────────────
    {InputAction::Pause, kb(Key::Escape)},
    {InputAction::Pause, gb(GamepadButton::Start)},
    {InputAction::ConsoleToggle, kb(Key::Grave)},
    {InputAction::PerfOverlayCycle, kb(Key::F3)},

    // ── Comms ────────────────────────────────────────────────────────────────
    {InputAction::WingmanMenu, kb(Key::C)},
    {InputAction::WingmanMenu, gb(GamepadButton::Back)},
    {InputAction::CommsMenu, kb(Key::T)},
    // Ejection (#672): End — deliberate and out of the way, so it is never hit by accident on the
    // flight-control cluster. The server edge-detects it; a held key is one ejection, not many.
    {InputAction::Eject, kb(Key::End)},
    {InputAction::Respawn, kb(Key::Backspace)},
    {InputAction::Scoreboard, kb(Key::I)},
    // Y all-channel, U team — U rather than H, because H is the arresting hook and both are live
    // while flying.
    {InputAction::ChatAll, kb(Key::Y)},
    {InputAction::ChatTeam, kb(Key::U)},

    // ── Camera modes (#689) ──────────────────────────────────────────────────
    {InputAction::CameraCockpit, kb(Key::F1)},
    {InputAction::CameraChase, kb(Key::F2)},
    {InputAction::CameraFree, kb(Key::F4)},

    // ── Target designation family (#671/#689/#696) ───────────────────────────
    {InputAction::PadlockToggle, kb(Key::F5)},
    {InputAction::PadlockToggle, gb(GamepadButton::RightStick)},
    {InputAction::TargetInsetToggle, kb(Key::F6)},
    {InputAction::NextTarget, kb(Key::N)},
    {InputAction::NextTarget, gb(GamepadButton::DpadUp)},
    {InputAction::PrevTarget, kb(Key::P)},

    // ── Autopilot holds (#640) ───────────────────────────────────────────────
    // A/D/S were rejected: they are the free camera's pan keys, live in the same session.
    {InputAction::AutopilotAltHold, kb(Key::F9)},
    {InputAction::AutopilotHdgHold, kb(Key::F10)},
    {InputAction::AutopilotSpdHold, kb(Key::F11)},

    // ── Avionics ─────────────────────────────────────────────────────────────
    {InputAction::MfdPage, kb(Key::O)},
    {InputAction::MfdRange, kb(Key::Num3)},
    {InputAction::NvgToggle, kb(Key::F7)},
    {InputAction::AircraftManual, kb(Key::M)},
    // The manual is non-modal, so its scroll keys cannot be PageUp/PageDown — those are the
    // throttle, and they stay live while it is open. The keypad's page positions instead.
    {InputAction::ManualScrollUp, kb(Key::Numpad9)},
    {InputAction::ManualScrollDown, kb(Key::Numpad3)},
    // The GM map moves off M and onto F12, joining the other overlay toggles on the function row.
    // The manual keeps M: every pilot opens it, only a GM-capable peer opens the map.
    {InputAction::GmMap, kb(Key::F12)},

    // ── Multi-crew seats (#975) ──────────────────────────────────────────────
    // The bottom-row cluster, moved off K/L/U as a group: K is the airbrake and U is team chat.
    {InputAction::CrewSeatCycle, kb(Key::Comma)},
    {InputAction::CrewSeatJoin, kb(Key::Period)},
    {InputAction::CrewSeatLeave, kb(Key::Slash)},

    // ── Voice (Epic J, #531) ─────────────────────────────────────────────────
    // All HELD, never latched: a latched mic is how a lobby ends up listening to someone's kitchen.
    {InputAction::PushToTalkPrimary, kb(Key::V)},
    // LeftCtrl, not B: B is the wheel brake. A modifier-class key is a good PTT — comfortable to
    // hold, on the left hand beside the flight cluster, and bound to nothing else.
    {InputAction::PushToTalkSecondary, kb(Key::LeftCtrl)},
    {InputAction::VoiceNetCycle, kb(Key::Backslash)},
    {InputAction::WingmanVoiceCommand, kb(Key::F8)},

    // ── Spectator picker (#860) ──────────────────────────────────────────────
    // Same keys as the weapon-station cycle, and that is not a collision: a spectator has no
    // ownship, so the two are never read in the same session mode.
    {InputAction::SpectateNext, kb(Key::Num1)},
    {InputAction::SpectatePrev, kb(Key::Num2)},

    // ── Replay transport (#41) ───────────────────────────────────────────────
    {InputAction::ReplayPauseToggle, kb(Key::Space)},
    {InputAction::ReplaySeekBack, kb(Key::ArrowLeft)},
    {InputAction::ReplaySeekForward, kb(Key::ArrowRight)},
    {InputAction::ReplaySeekBackFar, kb(Key::ArrowDown)},
    {InputAction::ReplaySeekForwardFar, kb(Key::ArrowUp)},
    {InputAction::ReplaySeekStart, kb(Key::Home)},
    {InputAction::ReplaySeekEnd, kb(Key::End)},
    {InputAction::ReplaySpeedDown, kb(Key::Minus)},
    {InputAction::ReplaySpeedUp, kb(Key::Equals)},

    // ── Photo mode (#41) ─────────────────────────────────────────────────────
    {InputAction::PhotoModeToggle, kb(Key::P)},
    {InputAction::PhotoFovIn, kb(Key::PageUp)},
    {InputAction::PhotoFovOut, kb(Key::PageDown)},
    {InputAction::PhotoFovFine, kb(Key::LeftShift)},
    // Not Q/E: those move the free camera, which is what you frame the shot with.
    {InputAction::PhotoRollLeft, kb(Key::Semicolon)},
    {InputAction::PhotoRollRight, kb(Key::Apostrophe)},
    {InputAction::PhotoEvDown, kb(Key::Minus)},
    {InputAction::PhotoEvUp, kb(Key::Equals)},
    {InputAction::PhotoCapture, kb(Key::Enter)},
};

} // namespace

InputBindings::InputBindings() {
    applyDefaults();
}

void InputBindings::applyDefaults() {
    for (auto& list : m_bindings)
        list.clear();
    for (const auto& d : kDefaults)
        m_bindings[static_cast<size_t>(d.action)].push_back(d.binding);
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

std::span<const Binding> InputBindings::get(InputAction action) const {
    return m_bindings[static_cast<size_t>(action)];
}

Binding InputBindings::first(InputAction action) const {
    const auto& list = m_bindings[static_cast<size_t>(action)];
    return list.empty() ? Binding{} : list.front();
}

int InputBindings::count(InputAction action) const {
    return static_cast<int>(m_bindings[static_cast<size_t>(action)].size());
}

void InputBindings::set(InputAction action, std::span<const Binding> bindings) {
    auto& list = m_bindings[static_cast<size_t>(action)];
    list.clear();
    for (const Binding& b : bindings)
        if (!b.isNone())
            list.push_back(b);
}

bool InputBindings::add(InputAction action, const Binding& binding) {
    if (binding.isNone())
        return false;
    auto& list = m_bindings[static_cast<size_t>(action)];
    for (const Binding& existing : list)
        if (sameBinding(existing, binding))
            return false;
    list.push_back(binding);
    return true;
}

bool InputBindings::remove(InputAction action, const Binding& binding) {
    auto& list = m_bindings[static_cast<size_t>(action)];
    const auto before = list.size();
    list.erase(std::remove_if(list.begin(), list.end(), [&](const Binding& b) { return sameBinding(b, binding); }),
               list.end());
    return list.size() != before;
}

void InputBindings::clear(InputAction action) {
    m_bindings[static_cast<size_t>(action)].clear();
}

// ---------------------------------------------------------------------------
// Device registry
// ---------------------------------------------------------------------------

void InputBindings::noteDevice(const DeviceRef& ref, std::string name) {
    if (ref.isAny())
        return;
    for (auto& d : m_devices) {
        if (sameDeviceRef(d.ref, ref)) {
            if (!name.empty())
                d.name = std::move(name);
            return;
        }
    }
    m_devices.push_back({ref, std::move(name)});
}

const char* InputBindings::deviceName(const DeviceRef& ref) const {
    for (const auto& d : m_devices)
        if (sameDeviceRef(d.ref, ref))
            return d.name.c_str();
    return "";
}

std::vector<InputBindings::DeviceUse> InputBindings::deviceUsage() const {
    std::vector<DeviceUse> out;
    for (int i = 0; i < kActionCount; ++i) {
        for (const Binding& b : m_bindings[static_cast<size_t>(i)]) {
            if (!sourceUsesDevice(b.source) || b.device.isAny())
                continue;
            auto it = std::find_if(out.begin(), out.end(),
                                   [&](const DeviceUse& u) { return sameDeviceRef(u.ref, b.device); });
            if (it == out.end())
                out.push_back({b.device, deviceName(b.device), 1});
            else
                ++it->bindingCount;
        }
    }
    return out;
}

bool InputBindings::joystickBindingsAllWildcard() const {
    bool sawJoystick = false;
    for (const auto& list : m_bindings) {
        for (const Binding& b : list) {
            if (!sourceUsesDevice(b.source))
                continue;
            if (!b.device.isAny())
                return false;
            sawJoystick = true;
        }
    }
    return sawJoystick;
}

// ---------------------------------------------------------------------------
// Conflicts
// ---------------------------------------------------------------------------

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
        for (const Binding& b : m_bindings[static_cast<size_t>(i)])
            if (sameInput(binding, b))
                return other;
    }
    return std::nullopt;
}

std::vector<InputBindings::Conflict> InputBindings::findConflicts() const {
    std::vector<Conflict> out;
    for (int i = 0; i < kActionCount; ++i) {
        for (int j = i + 1; j < kActionCount; ++j) {
            if (!contextsOverlap(contexts(static_cast<InputAction>(i)), contexts(static_cast<InputAction>(j))))
                continue;
            const auto& listA = m_bindings[static_cast<size_t>(i)];
            const auto& listB = m_bindings[static_cast<size_t>(j)];
            for (int si = 0; si < static_cast<int>(listA.size()); ++si) {
                for (int sj = 0; sj < static_cast<int>(listB.size()); ++sj) {
                    if (sameInput(listA[static_cast<size_t>(si)], listB[static_cast<size_t>(sj)])) {
                        out.push_back({static_cast<InputAction>(i), si, static_cast<InputAction>(j), sj,
                                       listA[static_cast<size_t>(si)]});
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
           "# Each action under [bindings] holds an ARBITRARY-LENGTH list of bindings, all live at once:\n"
           "# a control can be a key, a mouse button, a gamepad button and a button on each of two\n"
           "# joysticks simultaneously. Order matters only for analog axes, where the first axis that is\n"
           "# driving wins — past its deadzone for a Centered axis, moving for an Absolute one.\n"
           "#\n"
           "# A joystick binding names its device by GUID (see [[devices]] below). Omit `device`, or leave\n"
           "# it empty, to read EVERY connected stick — that is what the shipped defaults do, and whichever\n"
           "# stick you move is in command.\n"
           "# A binding whose device is not connected is KEPT and simply does nothing; plug the device\n"
           "# back in and the control returns, with no re-binding.\n"
           "#\n"
           "# Two actions may share an input only when the game never reads both in the same session mode\n"
           "# (flying / spectating / replay / photo); the game logs a warning at startup for any binding\n"
           "# that breaks that rule.\n"
           "#\n"
           "# `version` marks which shipped key map this file was written from. Lowering or removing it\n"
           "# makes the game regenerate the file from the current defaults.\n"
        << "version = " << kFormatVersion << "\n";

    if (!m_devices.empty()) {
        out << "\n# Input devices this file refers to. The name is informational — the GUID is the identity,\n"
               "# because device INDICES are renumbered whenever something is plugged in or out.\n";
        for (const auto& d : m_devices) {
            out << "\n[[devices]]\n";
            out << "guid = \"" << d.ref.guid << "\"\n";
            out << "name = \"" << d.name << "\"\n";
        }
    }

    out << "\n[bindings]\n";
    for (int i = 0; i < kActionCount; ++i) {
        const auto action = static_cast<InputAction>(i);
        const auto& list = m_bindings[static_cast<size_t>(i)];
        out << actionName(action) << " = [";
        if (list.empty()) {
            out << "]\n";
            continue;
        }
        out << "\n";
        for (const Binding& b : list)
            out << "  " << serializeBinding(b) << ",\n";
        out << "]\n";
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

    // Parse into temporaries so a mid-parse failure leaves the original intact.
    auto tmp = m_bindings;
    auto tmpDevices = m_devices;

    if (auto* devs = tbl["devices"].as_array()) {
        for (const auto& elem : *devs) {
            const auto* t = elem.as_table();
            if (!t)
                continue;
            const auto guid = tomlString(*t, "guid");
            if (guid.empty())
                continue;
            const DeviceRef ref = makeDeviceRef(guid.c_str());
            const auto name = tomlString(*t, "name");
            auto it = std::find_if(tmpDevices.begin(), tmpDevices.end(),
                                   [&](const KnownDevice& d) { return sameDeviceRef(d.ref, ref); });
            if (it == tmpDevices.end())
                tmpDevices.push_back({ref, name});
            else if (!name.empty())
                it->name = name;
        }
    }

    if (auto* sec = tbl["bindings"].as_table()) {
        // Version 3: one array of binding tables per action.
        for (auto& [key, val] : *sec) {
            const auto actionOpt = actionFromName(std::string(key.str()));
            if (!actionOpt)
                continue; // an action name we do not know is ignored, not an error
            auto* arr = val.as_array();
            if (!arr)
                return false;
            std::vector<Binding> list;
            for (const auto& elem : *arr) {
                const auto* entry = elem.as_table();
                if (!entry)
                    return false;
                Binding b;
                if (!parseBindingTable(*entry, b))
                    return false;
                if (!b.isNone())
                    list.push_back(b);
            }
            tmp[static_cast<size_t>(*actionOpt)] = std::move(list);
        }
    } else {
        // Version 1/2: `[primary]` / `[secondary]` / `[gamepad]` (or the version-1 `[alt]`), one
        // binding per action per section. Read so that an install carrying an older file keeps the
        // player's customisation rather than silently reverting to defaults; the sections concatenate
        // into the new list in slot order, which is what they always meant.
        const char* padSection = tbl["gamepad"].as_table() ? "gamepad" : "alt";
        const char* sections[] = {"primary", "secondary", padSection};
        bool sawAny = false;
        std::array<std::vector<Binding>, static_cast<std::size_t>(kActionCount)> legacy{};
        for (const char* section : sections) {
            auto* legacySec = tbl[section].as_table();
            if (!legacySec)
                continue;
            sawAny = true;
            for (auto& [key, val] : *legacySec) {
                const auto actionOpt = actionFromName(std::string(key.str()));
                if (!actionOpt)
                    continue;
                const auto* entry = val.as_table();
                if (!entry)
                    continue;
                Binding b;
                if (!parseBindingTable(*entry, b))
                    return false;
                if (!b.isNone())
                    legacy[static_cast<size_t>(*actionOpt)].push_back(b);
            }
        }
        if (!sawAny)
            return true; // no binding sections at all: leave the table alone
        // A legacy file is a FULL table, so an action it lists at None really is unbound. Only actions
        // the file never mentions keep their defaults — and since a version-2 file lists every action
        // in every section, in practice that is none of them.
        for (int i = 0; i < kActionCount; ++i)
            tmp[static_cast<size_t>(i)] = std::move(legacy[static_cast<size_t>(i)]);
    }

    m_bindings = std::move(tmp);
    m_devices = std::move(tmpDevices);
    return true;
}

} // namespace fl
