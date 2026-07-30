// SPDX-License-Identifier: GPL-3.0-or-later
#include "input/AxisConfig.h"
#include "input/BindingQuery.h"
#include "input/InputBindings.h"
#include "input/JoystickDevices.h"
#include "input/LegacyHotas.h"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "mock_hal.h" // MockInput / MockJoystick (engine-input links platform-hal)

#include <string>
#include <vector>

using namespace fl;

namespace {

// Two distinguishable sticks. Real SDL GUIDs are 32 hex characters; these are legible stand-ins of the
// same length, because the length is what the DeviceRef buffer is sized for.
constexpr const char* kStickA = "03000000a1b2c3d4000000000000aaaa";
constexpr const char* kStickB = "03000000a1b2c3d4000000000000bbbb";

Binding jsButton(uint32_t index, const char* guid = nullptr) {
    Binding b{};
    b.source = BindingSource::JoystickButton;
    b.id = index;
    b.device = makeDeviceRef(guid);
    return b;
}

Binding jsAxis(uint32_t index, const char* guid = nullptr, bool negative = false) {
    Binding b{};
    b.source = BindingSource::JoystickAxis;
    b.id = index;
    b.axisNegative = negative;
    b.device = makeDeviceRef(guid);
    return b;
}

Binding jsHat(uint32_t index, HatPosition dir, const char* guid = nullptr) {
    Binding b{};
    b.source = BindingSource::JoystickHat;
    b.id = index;
    b.hat = dir;
    b.device = makeDeviceRef(guid);
    return b;
}

Binding key(Key k) {
    return Binding{BindingSource::Keyboard, static_cast<uint32_t>(k), false, HatPosition::Centered, DeviceRef{}};
}

Binding mouse(MouseButton m) {
    return Binding{BindingSource::MouseButton, static_cast<uint32_t>(m), false, HatPosition::Centered, DeviceRef{}};
}

Binding padButton(GamepadButton g) {
    return Binding{BindingSource::GamepadButton, static_cast<uint32_t>(g), false, HatPosition::Centered, DeviceRef{}};
}

Binding padAxis(GamepadAxis a, bool negative = false) {
    return Binding{BindingSource::GamepadAxis, static_cast<uint32_t>(a), negative, HatPosition::Centered, DeviceRef{}};
}

// Does this action hold a binding equal to `want`?
bool hasBinding(const InputBindings& b, InputAction action, const Binding& want) {
    for (const Binding& x : b.get(action))
        if (sameBinding(x, want))
            return true;
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// DeviceRef
// ---------------------------------------------------------------------------

TEST_CASE("DeviceRef: empty means any device", "[bindings][device]") {
    CHECK(DeviceRef{}.isAny());
    CHECK(makeDeviceRef(nullptr).isAny());
    CHECK(makeDeviceRef("").isAny());
    CHECK_FALSE(makeDeviceRef(kStickA).isAny());
}

TEST_CASE("DeviceRef: a GUID longer than the buffer is truncated, not overrun", "[bindings][device]") {
    const std::string tooLong(kDeviceGuidChars + 20, 'f');
    const DeviceRef ref = makeDeviceRef(tooLong.c_str());
    CHECK(std::string(ref.guid).size() == static_cast<size_t>(kDeviceGuidChars));
}

TEST_CASE("DeviceRef: exact identity vs may-overlap", "[bindings][device]") {
    const DeviceRef a = makeDeviceRef(kStickA);
    const DeviceRef b = makeDeviceRef(kStickB);
    CHECK(sameDeviceRef(a, a));
    CHECK_FALSE(sameDeviceRef(a, b));
    CHECK_FALSE(sameDeviceRef(a, DeviceRef{}));

    // "Any device" can resolve to either stick at runtime, so the conflict checker has to treat it as
    // overlapping both — otherwise a shipped wildcard default could clash with a user's concrete
    // binding and nothing would report it.
    CHECK(devicesMayOverlap(a, DeviceRef{}));
    CHECK(devicesMayOverlap(DeviceRef{}, b));
    CHECK_FALSE(devicesMayOverlap(a, b));
}

// ---------------------------------------------------------------------------
// Hat matching
// ---------------------------------------------------------------------------

TEST_CASE("hatMatches: a cardinal binding also fires on its two diagonals", "[bindings][hat]") {
    CHECK(hatMatches(HatPosition::Up, HatPosition::Up));
    CHECK(hatMatches(HatPosition::Up, HatPosition::UpRight));
    CHECK(hatMatches(HatPosition::Up, HatPosition::UpLeft));
    CHECK_FALSE(hatMatches(HatPosition::Up, HatPosition::Right));
    CHECK_FALSE(hatMatches(HatPosition::Up, HatPosition::Down));
    CHECK(hatMatches(HatPosition::Right, HatPosition::DownRight));
    CHECK(hatMatches(HatPosition::Left, HatPosition::UpLeft));
}

TEST_CASE("hatMatches: a diagonal binding is exact, and Centered never matches", "[bindings][hat]") {
    CHECK(hatMatches(HatPosition::UpRight, HatPosition::UpRight));
    CHECK_FALSE(hatMatches(HatPosition::UpRight, HatPosition::Up));
    CHECK_FALSE(hatMatches(HatPosition::UpRight, HatPosition::Right));
    // A centered hat is a hat nobody is pressing.
    CHECK_FALSE(hatMatches(HatPosition::Centered, HatPosition::Centered));
    CHECK_FALSE(hatMatches(HatPosition::Up, HatPosition::Centered));
    CHECK_FALSE(hatMatches(HatPosition::Centered, HatPosition::Up));
}

// ---------------------------------------------------------------------------
// sameInput
// ---------------------------------------------------------------------------

TEST_CASE("sameInput: two joystick bindings on different devices are different inputs", "[bindings]") {
    CHECK(sameInput(jsButton(5, kStickA), jsButton(5, kStickA)));
    CHECK_FALSE(sameInput(jsButton(5, kStickA), jsButton(5, kStickB)));
    CHECK_FALSE(sameInput(jsButton(5, kStickA), jsButton(7, kStickA)));
    // Wildcard could be either stick.
    CHECK(sameInput(jsButton(5), jsButton(5, kStickA)));
}

TEST_CASE("sameInput: source and axis direction are part of the identity", "[bindings]") {
    CHECK_FALSE(sameInput(jsButton(2), jsAxis(2)));
    CHECK_FALSE(sameInput(jsAxis(2, nullptr, false), jsAxis(2, nullptr, true)));
    CHECK(sameInput(jsAxis(2, nullptr, true), jsAxis(2, nullptr, true)));
    CHECK_FALSE(sameInput(Binding{}, Binding{}));
}

// ---------------------------------------------------------------------------
// AxisConfig::apply — centered
// ---------------------------------------------------------------------------

TEST_CASE("AxisConfig dead zone clamps to zero and reports inactive", "[axis_config]") {
    AxisConfig cfg;
    cfg.deadzone = 0.1f;

    CHECK(cfg.apply(0.0f).value == Catch::Approx(0.0f));
    CHECK_FALSE(cfg.apply(0.0f).active);
    CHECK(cfg.apply(0.05f).value == Catch::Approx(0.0f));
    CHECK(cfg.apply(-0.09f).value == Catch::Approx(0.0f));
    // Exactly at the boundary is still clamped
    CHECK(cfg.apply(0.1f).value == Catch::Approx(0.0f));
    CHECK_FALSE(cfg.apply(0.1f).active);
    CHECK(cfg.apply(0.5f).active);
}

TEST_CASE("AxisConfig linear rescaling is correct", "[axis_config]") {
    AxisConfig cfg;
    cfg.deadzone = 0.0f;
    cfg.curve = AxisCurve::Linear;
    cfg.invert = false;
    cfg.scale = 1.0f;

    CHECK(cfg.apply(1.0f).value == Catch::Approx(1.0f));
    CHECK(cfg.apply(-1.0f).value == Catch::Approx(-1.0f));
    CHECK(cfg.apply(0.5f).value == Catch::Approx(0.5f));
}

TEST_CASE("AxisConfig rescales from [deadzone,1] to [0,1]", "[axis_config]") {
    AxisConfig cfg;
    cfg.deadzone = 0.5f;
    cfg.curve = AxisCurve::Linear;
    cfg.scale = 1.0f;

    CHECK(cfg.apply(0.5f + 1e-4f).value > 0.0f);
    CHECK(cfg.apply(1.0f).value == Catch::Approx(1.0f));
}

TEST_CASE("AxisConfig cubic curve produces expected shape", "[axis_config]") {
    AxisConfig cfg;
    cfg.deadzone = 0.0f;
    cfg.curve = AxisCurve::Cubic;
    cfg.scale = 1.0f;

    CHECK(cfg.apply(0.5f).value == Catch::Approx(0.125f).margin(1e-5f));
    CHECK(cfg.apply(0.8f).value < 0.8f);
    CHECK(cfg.apply(0.0f).value == Catch::Approx(0.0f));
    CHECK(cfg.apply(1.0f).value == Catch::Approx(1.0f));
}

TEST_CASE("AxisConfig invert flips sign", "[axis_config]") {
    AxisConfig cfg;
    cfg.deadzone = 0.0f;
    cfg.invert = true;
    cfg.scale = 1.0f;

    CHECK(cfg.apply(1.0f).value == Catch::Approx(-1.0f));
    CHECK(cfg.apply(-1.0f).value == Catch::Approx(1.0f));
    CHECK(cfg.apply(0.5f).value == Catch::Approx(-0.5f));
}

TEST_CASE("AxisConfig scale multiplies output", "[axis_config]") {
    AxisConfig cfg;
    cfg.deadzone = 0.0f;
    cfg.scale = 2.0f;
    CHECK(cfg.apply(0.5f).value == Catch::Approx(1.0f));
}

TEST_CASE("AxisConfig negative input mirrors positive", "[axis_config]") {
    AxisConfig cfg;
    cfg.deadzone = 0.1f;
    cfg.curve = AxisCurve::Cubic;
    cfg.scale = 1.0f;

    const float pos = cfg.apply(0.7f).value;
    const float neg = cfg.apply(-0.7f).value;
    CHECK(pos == Catch::Approx(-neg).margin(1e-5f));
}

TEST_CASE("AxisConfig clamps input beyond 1.0 to 1.0", "[axis_config]") {
    AxisConfig cfg;
    cfg.deadzone = 0.0f;
    CHECK(cfg.apply(2.0f).value == Catch::Approx(1.0f));
    CHECK(cfg.apply(-2.0f).value == Catch::Approx(-1.0f));
}

// ---------------------------------------------------------------------------
// AxisConfig::apply — absolute (#1061)
// ---------------------------------------------------------------------------

TEST_CASE("AxisConfig absolute mode maps full travel onto [0,1]", "[axis_config]") {
    AxisConfig cfg;
    cfg.mode = AxisMode::Absolute;
    cfg.deadzone = 0.05f;

    CHECK(cfg.apply(-1.0f).value == Catch::Approx(0.0f));
    CHECK(cfg.apply(0.0f).value == Catch::Approx(0.5f));
    CHECK(cfg.apply(1.0f).value == Catch::Approx(1.0f));
    CHECK(cfg.apply(0.5f).value == Catch::Approx(0.75f));
}

TEST_CASE("AxisConfig absolute mode is always active, even at idle", "[axis_config]") {
    AxisConfig cfg;
    cfg.mode = AxisMode::Absolute;
    cfg.deadzone = 0.5f; // deliberately huge: an absolute lever has no centre to ignore

    // A throttle closed to idle reads 0.0 and is still in command. If this reported inactive, the
    // keyboard throttle would fight a stick the player had deliberately pulled back.
    const AxisSample idle = cfg.apply(-1.0f);
    CHECK(idle.value == Catch::Approx(0.0f));
    CHECK(idle.active);
    CHECK(cfg.apply(0.0f).active);
}

TEST_CASE("AxisConfig absolute mode inverts before the remap", "[axis_config]") {
    AxisConfig cfg;
    cfg.mode = AxisMode::Absolute;
    cfg.invert = true;
    CHECK(cfg.apply(-1.0f).value == Catch::Approx(1.0f));
    CHECK(cfg.apply(1.0f).value == Catch::Approx(0.0f));
}

// ---------------------------------------------------------------------------
// AxisConfigTable
// ---------------------------------------------------------------------------

TEST_CASE("AxisConfigTable: shipped defaults tune gamepad and HOTAS separately", "[axis_config]") {
    const AxisConfigTable t;
    const AxisConfig pad =
        t.effective(AxisKey{BindingSource::GamepadAxis, static_cast<uint32_t>(GamepadAxis::RightY), DeviceRef{}});
    CHECK(pad.deadzone == Catch::Approx(0.1f));
    CHECK(pad.mode == AxisMode::Centered);

    // A HOTAS potentiometer has far less slop than a thumbstick, and the throttle is a lever.
    const AxisConfig stick = t.effective(AxisKey{BindingSource::JoystickAxis, kHotasAxisPitch, DeviceRef{}});
    CHECK(stick.deadzone == Catch::Approx(kHotasDefaultDeadzone));
    CHECK(stick.mode == AxisMode::Centered);
    const AxisConfig lever = t.effective(AxisKey{BindingSource::JoystickAxis, kHotasAxisThrottle, DeviceRef{}});
    CHECK(lever.mode == AxisMode::Absolute);
}

TEST_CASE("AxisConfigTable: keyed by (source, device, index)", "[axis_config]") {
    AxisConfigTable t;
    AxisConfig cubic;
    cubic.curve = AxisCurve::Cubic;
    cubic.deadzone = 0.2f;
    // "Joystick 2, axis 9" — a key the pre-#1061 six-entry gamepad array could not express at all.
    t.set(AxisKey{BindingSource::JoystickAxis, 9, makeDeviceRef(kStickB)}, cubic);

    CHECK(t.effective(AxisKey{BindingSource::JoystickAxis, 9, makeDeviceRef(kStickB)}).curve == AxisCurve::Cubic);
    // A different device on the same axis index is a different axis.
    CHECK(t.effective(AxisKey{BindingSource::JoystickAxis, 9, makeDeviceRef(kStickA)}).curve == AxisCurve::Linear);
    // A different source on the same index is a different axis too.
    CHECK(t.effective(AxisKey{BindingSource::GamepadAxis, 9, DeviceRef{}}).curve == AxisCurve::Linear);
}

TEST_CASE("AxisConfigTable: a concrete device falls back to the any-device tuning", "[axis_config]") {
    AxisConfigTable t;
    AxisConfig tuned;
    tuned.scale = 0.5f;
    t.set(AxisKey{BindingSource::JoystickAxis, 4, DeviceRef{}}, tuned);

    // A player who tuned "joystick axis 4" without naming a GUID meant it for whatever stick is
    // plugged in; naming the device in a binding must not silently drop that tuning.
    CHECK(t.effective(AxisKey{BindingSource::JoystickAxis, 4, makeDeviceRef(kStickA)}).scale == Catch::Approx(0.5f));
}

TEST_CASE("AxisConfigTable TOML roundtrip", "[axis_config]") {
    AxisConfigTable t;
    AxisConfig cfg;
    cfg.deadzone = 0.15f;
    cfg.curve = AxisCurve::Cubic;
    cfg.invert = true;
    cfg.scale = 0.9f;
    t.set(AxisKey{BindingSource::GamepadAxis, static_cast<uint32_t>(GamepadAxis::LeftY), DeviceRef{}}, cfg);

    AxisConfig joy;
    joy.deadzone = 0.02f;
    joy.mode = AxisMode::Absolute;
    joy.invert = true;
    t.set(AxisKey{BindingSource::JoystickAxis, 7, makeDeviceRef(kStickA)}, joy);

    AxisConfigTable t2;
    REQUIRE(t2.deserialize(t.serialize()));

    const AxisConfig padOut =
        t2.effective(AxisKey{BindingSource::GamepadAxis, static_cast<uint32_t>(GamepadAxis::LeftY), DeviceRef{}});
    CHECK(padOut.deadzone == Catch::Approx(0.15f));
    CHECK(padOut.curve == AxisCurve::Cubic);
    CHECK(padOut.invert);
    CHECK(padOut.scale == Catch::Approx(0.9f));

    const AxisConfig joyOut = t2.effective(AxisKey{BindingSource::JoystickAxis, 7, makeDeviceRef(kStickA)});
    CHECK(joyOut.deadzone == Catch::Approx(0.02f));
    CHECK(joyOut.mode == AxisMode::Absolute);
    CHECK(joyOut.invert);
}

TEST_CASE("AxisConfigTable: a version-2 [axis_config] table still loads (#1061)", "[axis_config]") {
    // The whole reason bindings.toml carries a version: a schema move must not cost the player their
    // axis tuning.
    const char* legacy = "version = 2\n"
                         "[axis_config]\n"
                         "RightY = { deadzone = 0.22, curve = \"Cubic\", invert = true, scale = 0.8 }\n";
    AxisConfigTable t;
    REQUIRE(t.deserialize(legacy));
    const AxisConfig out =
        t.effective(AxisKey{BindingSource::GamepadAxis, static_cast<uint32_t>(GamepadAxis::RightY), DeviceRef{}});
    CHECK(out.deadzone == Catch::Approx(0.22f));
    CHECK(out.curve == AxisCurve::Cubic);
    CHECK(out.invert);
    CHECK(out.scale == Catch::Approx(0.8f));
}

TEST_CASE("AxisConfigTable deserialize: invalid TOML fails, absent section keeps defaults", "[axis_config]") {
    AxisConfigTable t;
    CHECK_FALSE(t.deserialize("not valid toml }{"));
    CHECK(t.deserialize("version = 3\n"));
    CHECK(t.effective(AxisKey{BindingSource::GamepadAxis, 0, DeviceRef{}}).deadzone == Catch::Approx(0.1f));
}

TEST_CASE("AxisConfigTable deserialize rejects an unknown source or a missing index", "[axis_config]") {
    AxisConfigTable t;
    CHECK_FALSE(t.deserialize("[[axis_config]]\nsource = \"Keyboard\"\nid = \"A\"\n"));
    CHECK_FALSE(t.deserialize("[[axis_config]]\nsource = \"JoystickAxis\"\n"));
    CHECK_FALSE(t.deserialize("[[axis_config]]\nsource = \"GamepadAxis\"\nid = \"NoSuchAxis\"\n"));
}

TEST_CASE("AxisConfigTable erase removes an entry", "[axis_config]") {
    AxisConfigTable t;
    const AxisKey k{BindingSource::JoystickAxis, 11, DeviceRef{}};
    AxisConfig c;
    c.scale = 3.0f;
    t.set(k, c);
    CHECK(t.find(k) != nullptr);
    CHECK(t.erase(k));
    CHECK(t.find(k) == nullptr);
    CHECK_FALSE(t.erase(k));
}

// ---------------------------------------------------------------------------
// InputBindings defaults
// ---------------------------------------------------------------------------

TEST_CASE("InputBindings default constructor applies defaults", "[bindings]") {
    InputBindings b;
    // The arrows fly the aircraft (#1050 — the table used to claim S/W/A/D while the game read the
    // arrows, and claim the arrows for the cockpit view pan at the same time).
    CHECK(b.first(InputAction::PitchUp).source == BindingSource::Keyboard);
    CHECK(b.first(InputAction::PitchUp).id == static_cast<uint32_t>(Key::ArrowDown));
    CHECK(b.first(InputAction::PitchDown).id == static_cast<uint32_t>(Key::ArrowUp));
    CHECK(b.first(InputAction::RollLeft).id == static_cast<uint32_t>(Key::ArrowLeft));
    CHECK(b.first(InputAction::RollRight).id == static_cast<uint32_t>(Key::ArrowRight));
    // The gun's three keyboard/mouse/pad bindings are all live at once.
    CHECK(b.count(InputAction::FireWeapon) == 3);
    CHECK(hasBinding(b, InputAction::FireWeapon, key(Key::Space)));
    CHECK(hasBinding(b, InputAction::FireWeapon, mouse(MouseButton::Left)));
    CHECK(hasBinding(b, InputAction::FireWeapon, padButton(GamepadButton::RightShoulder)));
}

TEST_CASE("InputBindings: the four flight axes reach the table from BOTH stick and pad (#1061)", "[bindings]") {
    const InputBindings b;
    // Before #1061 the HOTAS axes lived in `[controls]` in user.toml, read by index on device 0 and
    // invisible to this table — the last parallel input-config path in the game.
    CHECK(hasBinding(b, InputAction::PitchAxis, jsAxis(kHotasAxisPitch)));
    CHECK(hasBinding(b, InputAction::RollAxis, jsAxis(kHotasAxisRoll)));
    CHECK(hasBinding(b, InputAction::YawAxis, jsAxis(kHotasAxisYaw)));
    CHECK(hasBinding(b, InputAction::ThrottleAxis, jsAxis(kHotasAxisThrottle)));
    CHECK(hasBinding(b, InputAction::PitchAxis, padAxis(GamepadAxis::RightY)));
    CHECK(hasBinding(b, InputAction::ThrottleAxis, padAxis(GamepadAxis::TriggerLeft)));

    // Order is the priority: the joystick comes first, which is what the pre-#1061 "HOTAS applied
    // after gamepad" ordering did.
    CHECK(b.first(InputAction::PitchAxis).source == BindingSource::JoystickAxis);
    CHECK(b.first(InputAction::ThrottleAxis).source == BindingSource::JoystickAxis);
}

TEST_CASE("InputBindings: the shipped defaults contain no conflicts", "[bindings]") {
    const InputBindings b;
    const auto conflicts = b.findConflicts();
    for (const auto& c : conflicts) {
        UNSCOPED_INFO("default conflict: " << InputBindings::actionName(c.a) << " (binding " << c.indexA + 1 << ") vs "
                                           << InputBindings::actionName(c.b) << " (binding " << c.indexB + 1 << ")");
    }
    CHECK(conflicts.empty());
}

TEST_CASE("InputBindings: every action a default binds resolves back through its own name", "[bindings]") {
    for (int i = 0; i < InputBindings::kActionCount; ++i) {
        const auto a = static_cast<InputAction>(i);
        CHECK(InputBindings::actionFromName(InputBindings::actionName(a)) == a);
    }
}

TEST_CASE("InputBindings: V is push-to-talk and nothing else live at the same time (#1050)", "[bindings]") {
    const InputBindings b;
    CHECK(b.first(InputAction::PushToTalkPrimary).id == static_cast<uint32_t>(Key::V));
    CHECK_FALSE(b.conflictsWith(InputAction::PushToTalkPrimary, key(Key::V)).has_value());
    CHECK(b.first(InputAction::MasterArm).id == static_cast<uint32_t>(Key::Num4));
}

TEST_CASE("InputBindings: sharing a key across disjoint contexts is not a conflict", "[bindings]") {
    const InputBindings b;
    CHECK(b.first(InputAction::FireWeapon).id == static_cast<uint32_t>(Key::Space));
    CHECK(b.first(InputAction::ReplayPauseToggle).id == static_cast<uint32_t>(Key::Space));
    CHECK_FALSE(contextsOverlap(InputBindings::contexts(InputAction::FireWeapon),
                                InputBindings::contexts(InputAction::ReplayPauseToggle)));

    // The same key across OVERLAPPING contexts still is one.
    InputBindings clash;
    clash.set(InputAction::MasterArm, std::vector<Binding>{key(Key::V)});
    CHECK(clash.conflictsWith(InputAction::MasterArm, key(Key::V)) == InputAction::PushToTalkPrimary);
}

TEST_CASE("InputBindings: no live control shares a key with the free camera", "[bindings]") {
    const InputAction freeCam[] = {InputAction::FreeCamForward, InputAction::FreeCamBack,  InputAction::FreeCamLeft,
                                   InputAction::FreeCamRight,   InputAction::FreeCamUp,    InputAction::FreeCamDown,
                                   InputAction::FreeCamFaster,  InputAction::FreeCamSlower};
    const InputBindings b;
    for (InputAction a : freeCam) {
        INFO("free-camera action " << InputBindings::actionName(a));
        for (const Binding& x : b.get(a))
            CHECK_FALSE(b.conflictsWith(a, x).has_value());
    }
}

// ---------------------------------------------------------------------------
// InputBindings — arbitrary-length lists (#1061)
// ---------------------------------------------------------------------------

TEST_CASE("InputBindings: one action holds five bindings across four devices (#1061)", "[bindings]") {
    // The acceptance case from the issue: the gun on Space, left mouse, a gamepad shoulder, joystick 1
    // button 5 and joystick 2 button 7 — all live simultaneously. Three fixed slots could not hold it.
    InputBindings b;
    b.set(InputAction::FireWeapon,
          std::vector<Binding>{key(Key::Space), mouse(MouseButton::Left), padButton(GamepadButton::RightShoulder),
                               jsButton(5, kStickA), jsButton(7, kStickB)});
    REQUIRE(b.count(InputAction::FireWeapon) == 5);
    CHECK(b.findConflicts().empty());

    MockInput in;
    MockJoystick joy;
    joy.addDevice(kStickA).buttons.assign(8, false);
    joy.addDevice(kStickB).buttons.assign(8, false);
    JoystickDevices devices;
    devices.update(joy);
    const InputSources src{&in, &joy, &devices, 0};

    CHECK_FALSE(actionDown(src, b, InputAction::FireWeapon));

    // Each of the five, on its own, fires the gun.
    in.held.insert(Key::Space);
    CHECK(actionDown(src, b, InputAction::FireWeapon));
    in.held.clear();

    in.mouseDown.insert(MouseButton::Left);
    CHECK(actionDown(src, b, InputAction::FireWeapon));
    in.mouseDown.clear();

    in.gamepadCount = 1;
    in.gpDown.insert({0, GamepadButton::RightShoulder});
    CHECK(actionDown(src, b, InputAction::FireWeapon));
    in.gpDown.clear();

    joy.devices[0].buttons[5] = true;
    CHECK(actionDown(src, b, InputAction::FireWeapon));
    joy.devices[0].buttons[5] = false;

    joy.devices[1].buttons[7] = true;
    CHECK(actionDown(src, b, InputAction::FireWeapon));
    joy.devices[1].buttons[7] = false;
    CHECK_FALSE(actionDown(src, b, InputAction::FireWeapon));
}

TEST_CASE("InputBindings: add / remove / clear / count", "[bindings]") {
    InputBindings b;
    b.clear(InputAction::EcmToggle);
    CHECK(b.count(InputAction::EcmToggle) == 0);
    CHECK(b.first(InputAction::EcmToggle).isNone());

    CHECK(b.add(InputAction::EcmToggle, jsButton(3, kStickA)));
    CHECK(b.add(InputAction::EcmToggle, jsHat(0, HatPosition::Up, kStickA)));
    CHECK(b.count(InputAction::EcmToggle) == 2);

    // A duplicate would fire the same control twice and read as a conflict with itself.
    CHECK_FALSE(b.add(InputAction::EcmToggle, jsButton(3, kStickA)));
    CHECK_FALSE(b.add(InputAction::EcmToggle, Binding{}));
    CHECK(b.count(InputAction::EcmToggle) == 2);

    CHECK(b.remove(InputAction::EcmToggle, jsButton(3, kStickA)));
    CHECK_FALSE(b.remove(InputAction::EcmToggle, jsButton(3, kStickA)));
    CHECK(b.count(InputAction::EcmToggle) == 1);
    // The same index on the OTHER stick is a different binding and is not removed by mistake.
    CHECK_FALSE(b.remove(InputAction::EcmToggle, jsHat(0, HatPosition::Up, kStickB)));
}

TEST_CASE("InputBindings: set() drops None bindings", "[bindings]") {
    InputBindings b;
    b.set(InputAction::Flaps, std::vector<Binding>{key(Key::F), Binding{}, jsButton(1)});
    CHECK(b.count(InputAction::Flaps) == 2);
}

TEST_CASE("InputBindings: two sticks with different GUIDs do not conflict (#1061)", "[bindings]") {
    InputBindings b;
    b.set(InputAction::LandingGear, std::vector<Binding>{jsButton(4, kStickA)});
    b.set(InputAction::ArrestorHook, std::vector<Binding>{jsButton(4, kStickB)});
    CHECK(b.findConflicts().empty());

    // Same index on the same stick IS a conflict — both are live while flying.
    b.set(InputAction::ArrestorHook, std::vector<Binding>{jsButton(4, kStickA)});
    const auto conflicts = b.findConflicts();
    REQUIRE(conflicts.size() == 1);
    CHECK(conflicts[0].a == InputAction::LandingGear);
    CHECK(conflicts[0].b == InputAction::ArrestorHook);
}

TEST_CASE("InputBindings: findConflicts reports the binding position within each list", "[bindings]") {
    InputBindings b;
    b.set(InputAction::LandingGear, std::vector<Binding>{key(Key::G), jsButton(2)});
    b.set(InputAction::ArrestorHook, std::vector<Binding>{key(Key::H), key(Key::L), jsButton(2)});
    const auto conflicts = b.findConflicts();
    REQUIRE(conflicts.size() == 1);
    CHECK(conflicts[0].indexA == 1);
    CHECK(conflicts[0].indexB == 2);
}

TEST_CASE("InputBindings: an action does not conflict with itself", "[bindings]") {
    const InputBindings b;
    CHECK_FALSE(b.conflictsWith(InputAction::FireWeapon, key(Key::Space)).has_value());
    CHECK_FALSE(b.conflictsWith(InputAction::FireWeapon, Binding{}).has_value());
}

// ---------------------------------------------------------------------------
// InputBindings — device registry
// ---------------------------------------------------------------------------

TEST_CASE("InputBindings: the device registry remembers a GUID's name", "[bindings][device]") {
    InputBindings b;
    CHECK(std::string(b.deviceName(makeDeviceRef(kStickA))).empty());
    b.noteDevice(makeDeviceRef(kStickA), "Thrustmaster T.16000M");
    CHECK(std::string(b.deviceName(makeDeviceRef(kStickA))) == "Thrustmaster T.16000M");
    // Re-noting refreshes the name rather than duplicating the entry.
    b.noteDevice(makeDeviceRef(kStickA), "T.16000M FCS");
    CHECK(b.devices().size() == 1);
    CHECK(std::string(b.deviceName(makeDeviceRef(kStickA))) == "T.16000M FCS");
    // "Any device" is not a device.
    b.noteDevice(DeviceRef{}, "nothing");
    CHECK(b.devices().size() == 1);
}

TEST_CASE("InputBindings: deviceUsage counts bindings per concrete device", "[bindings][device]") {
    InputBindings b;
    b.noteDevice(makeDeviceRef(kStickA), "Stick A");
    b.set(InputAction::FireWeapon, std::vector<Binding>{key(Key::Space), jsButton(0, kStickA), jsButton(1, kStickA)});
    b.set(InputAction::FireStore, std::vector<Binding>{jsButton(2, kStickB)});

    const auto usage = b.deviceUsage();
    REQUIRE(usage.size() == 2);
    const auto find = [&](const char* guid) {
        for (const auto& u : usage)
            if (sameDeviceRef(u.ref, makeDeviceRef(guid)))
                return u;
        return InputBindings::DeviceUse{};
    };
    CHECK(find(kStickA).bindingCount == 2);
    CHECK(find(kStickA).name == "Stick A");
    CHECK(find(kStickB).bindingCount == 1);
    CHECK(find(kStickB).name.empty()); // never seen, so nothing to call it
}

TEST_CASE("InputBindings: a wildcard binding is not waiting for a particular device", "[bindings][device]") {
    InputBindings b;
    // The shipped defaults are all wildcard joystick axes; none of them should be reported as an
    // absent device on a machine with no stick.
    CHECK(b.deviceUsage().empty());
}

// ---------------------------------------------------------------------------
// InputBindings — serialization
// ---------------------------------------------------------------------------

TEST_CASE("InputBindings: the serialized table carries its format version", "[bindings]") {
    const InputBindings b;
    CHECK(InputBindings::kFormatVersion == 3);
    CHECK(InputBindings::fileFormatVersion(b.serialize()) == 3);
    CHECK(InputBindings::fileFormatVersion("[bindings]\nPitchUp = []\n") == 0);
    CHECK(InputBindings::fileFormatVersion("not valid toml }{") == 0);
}

TEST_CASE("InputBindings TOML roundtrip reproduces every binding of every action", "[bindings]") {
    InputBindings original;
    original.set(InputAction::FireWeapon,
                 std::vector<Binding>{key(Key::Space), mouse(MouseButton::Middle),
                                      padAxis(GamepadAxis::TriggerRight, true), jsButton(11, kStickA),
                                      jsAxis(5, kStickB, true), jsHat(1, HatPosition::DownLeft, kStickA)});
    original.noteDevice(makeDeviceRef(kStickA), "Stick A");
    original.noteDevice(makeDeviceRef(kStickB), "Quadrant B");

    InputBindings loaded;
    REQUIRE(loaded.deserialize(original.serialize()));

    for (int i = 0; i < InputBindings::kActionCount; ++i) {
        const auto action = static_cast<InputAction>(i);
        INFO("action " << InputBindings::actionName(action));
        const auto a = original.get(action);
        const auto l = loaded.get(action);
        REQUIRE(a.size() == l.size());
        for (size_t k = 0; k < a.size(); ++k)
            CHECK(sameBinding(a[k], l[k]));
    }
    CHECK(std::string(loaded.deviceName(makeDeviceRef(kStickB))) == "Quadrant B");
}

TEST_CASE("InputBindings: every Key, MouseButton, GamepadButton and GamepadAxis round-trips", "[bindings]") {
    // A name we cannot resolve must not degrade into a plausible different control, so every enumerator
    // has to survive the file. Iterating the enums is what stops a new one being added without a name.
    InputBindings b;
    for (int k = 1; k < static_cast<int>(Key::Count); ++k) { // 0 = Unknown, deliberately unnameable
        b.set(InputAction::FireWeapon, std::vector<Binding>{key(static_cast<Key>(k))});
        InputBindings out;
        REQUIRE(out.deserialize(b.serialize()));
        CHECK(out.first(InputAction::FireWeapon).id == static_cast<uint32_t>(k));
    }
    for (int m = 0; m < static_cast<int>(MouseButton::Count); ++m) {
        b.set(InputAction::FireWeapon, std::vector<Binding>{mouse(static_cast<MouseButton>(m))});
        InputBindings out;
        REQUIRE(out.deserialize(b.serialize()));
        CHECK(out.first(InputAction::FireWeapon).source == BindingSource::MouseButton);
        CHECK(out.first(InputAction::FireWeapon).id == static_cast<uint32_t>(m));
    }
    for (int g = 0; g < static_cast<int>(GamepadButton::Count); ++g) {
        b.set(InputAction::FireWeapon, std::vector<Binding>{padButton(static_cast<GamepadButton>(g))});
        InputBindings out;
        REQUIRE(out.deserialize(b.serialize()));
        CHECK(out.first(InputAction::FireWeapon).id == static_cast<uint32_t>(g));
    }
    for (int a = 0; a < static_cast<int>(GamepadAxis::Count); ++a) {
        b.set(InputAction::PitchAxis, std::vector<Binding>{padAxis(static_cast<GamepadAxis>(a), true)});
        InputBindings out;
        REQUIRE(out.deserialize(b.serialize()));
        CHECK(out.first(InputAction::PitchAxis).id == static_cast<uint32_t>(a));
        CHECK(out.first(InputAction::PitchAxis).axisNegative);
    }
}

TEST_CASE("InputBindings: every hat direction round-trips", "[bindings][hat]") {
    const HatPosition dirs[] = {HatPosition::Up,   HatPosition::UpRight,  HatPosition::Right, HatPosition::DownRight,
                                HatPosition::Down, HatPosition::DownLeft, HatPosition::Left,  HatPosition::UpLeft};
    for (HatPosition dir : dirs) {
        InputBindings b;
        b.set(InputAction::NextTarget, std::vector<Binding>{jsHat(2, dir, kStickA)});
        InputBindings out;
        REQUIRE(out.deserialize(b.serialize()));
        const Binding got = out.first(InputAction::NextTarget);
        CHECK(got.source == BindingSource::JoystickHat);
        CHECK(got.id == 2u);
        CHECK(got.hat == dir);
        CHECK(sameDeviceRef(got.device, makeDeviceRef(kStickA)));
    }
}

TEST_CASE("InputBindings deserialize rejects malformed entries", "[bindings]") {
    InputBindings b;
    CHECK_FALSE(b.deserialize("not valid toml }{"));
    CHECK_FALSE(b.deserialize("[bindings]\nPitchUp = [{ source = \"Keyboard\", id = \"NoSuchKey\" }]\n"));
    // A joystick binding needs an index.
    CHECK_FALSE(b.deserialize("[bindings]\nPitchUp = [{ source = \"JoystickButton\" }]\n"));
    // A hat with no direction (or a Centered one) names a control that can never fire.
    CHECK_FALSE(b.deserialize("[bindings]\nPitchUp = [{ source = \"JoystickHat\", index = 0 }]\n"));
    CHECK_FALSE(
        b.deserialize("[bindings]\nPitchUp = [{ source = \"JoystickHat\", index = 0, direction = \"Centered\" }]\n"));
    // A bindings entry that is not an array is a schema error, not a binding.
    CHECK_FALSE(b.deserialize("[bindings]\nPitchUp = \"ArrowDown\"\n"));
    // The defaults are untouched by every failure above.
    CHECK(b.first(InputAction::PitchUp).id == static_cast<uint32_t>(Key::ArrowDown));
}

TEST_CASE("InputBindings deserialize ignores unknown action names", "[bindings]") {
    InputBindings b;
    CHECK(b.deserialize("[bindings]\nNoSuchAction = [{ source = \"Keyboard\", id = \"A\" }]\n"));
    CHECK(b.first(InputAction::PitchUp).id == static_cast<uint32_t>(Key::ArrowDown));
}

TEST_CASE("InputBindings: an explicit empty list unbinds the action", "[bindings]") {
    InputBindings b;
    REQUIRE(b.deserialize("version = 3\n[bindings]\nEcmToggle = []\n"));
    CHECK(b.count(InputAction::EcmToggle) == 0);
}

TEST_CASE("InputBindings: a version-2 slot file loads as a binding list (#1061)", "[bindings]") {
    // Only the schema changed in #1061, so a version-2 file's bindings are still valid and the player's
    // rebinds must survive the format move. The three sections concatenate in slot order.
    const char* v2 = "version = 2\n"
                     "[primary]\n"
                     "FireWeapon = { source = \"Keyboard\", id = \"Q\" }\n"
                     "[secondary]\n"
                     "FireWeapon = { source = \"MouseButton\", id = \"Right\" }\n"
                     "[gamepad]\n"
                     "FireWeapon = { source = \"GamepadButton\", id = \"X\" }\n";
    InputBindings b;
    REQUIRE(b.deserialize(v2));
    REQUIRE(b.count(InputAction::FireWeapon) == 3);
    CHECK(b.get(InputAction::FireWeapon)[0].id == static_cast<uint32_t>(Key::Q));
    CHECK(b.get(InputAction::FireWeapon)[1].source == BindingSource::MouseButton);
    CHECK(b.get(InputAction::FireWeapon)[2].id == static_cast<uint32_t>(GamepadButton::X));
    // A legacy file is a FULL table: an action it does not mention ends up unbound, not defaulted.
    CHECK(b.count(InputAction::PitchUp) == 0);
}

TEST_CASE("InputBindings: a version-1 [alt] section still loads as a pad binding", "[bindings]") {
    const char* v1 = "[primary]\nAirbrake = { source = \"Keyboard\", id = \"K\" }\n"
                     "[alt]\nAirbrake = { source = \"GamepadButton\", id = \"X\" }\n";
    InputBindings b;
    REQUIRE(b.deserialize(v1));
    REQUIRE(b.count(InputAction::Airbrake) == 2);
    CHECK(b.get(InputAction::Airbrake)[1].source == BindingSource::GamepadButton);
    CHECK(b.get(InputAction::Airbrake)[1].id == static_cast<uint32_t>(GamepadButton::X));
}

TEST_CASE("InputBindings: a file with no binding sections leaves the table alone", "[bindings]") {
    InputBindings b;
    REQUIRE(b.deserialize("version = 3\n[[axis_config]]\nsource = \"GamepadAxis\"\nid = \"LeftX\"\n"));
    CHECK(b.first(InputAction::PitchUp).id == static_cast<uint32_t>(Key::ArrowDown));
}

// ---------------------------------------------------------------------------
// BindingQuery
// ---------------------------------------------------------------------------

TEST_CASE("bindingDown / bindingJustPressed resolve every keyboard-class source", "[bindings]") {
    MockInput in;
    in.gamepadCount = 1;
    const InputSources src{&in, nullptr, nullptr, 0};

    const Binding k = key(Key::Space);
    CHECK_FALSE(bindingJustPressed(src, k));
    in.held.insert(Key::Space);
    in.justPressed.insert(Key::Space);
    CHECK(bindingJustPressed(src, k));
    CHECK(bindingDown(src, k));

    const Binding gb = padButton(GamepadButton::A);
    CHECK_FALSE(bindingDown(src, gb));
    in.gpDown.insert({0, GamepadButton::A});
    in.gpJustPressed.insert({0, GamepadButton::A});
    CHECK(bindingDown(src, gb));
    CHECK(bindingJustPressed(src, gb));

    const Binding axPos = padAxis(GamepadAxis::LeftY);
    in.axisValues[{0, GamepadAxis::LeftY}] = 0.8f;
    CHECK(bindingDown(src, axPos));
    CHECK_FALSE(bindingJustPressed(src, axPos)); // analog axes have no discrete edge
    in.axisValues[{0, GamepadAxis::LeftY}] = 0.3f;
    CHECK_FALSE(bindingDown(src, axPos)); // below threshold

    const Binding axNeg = padAxis(GamepadAxis::LeftY, true);
    in.axisValues[{0, GamepadAxis::LeftY}] = -0.8f;
    CHECK(bindingDown(src, axNeg));

    CHECK_FALSE(bindingDown(src, Binding{}));
    CHECK_FALSE(bindingJustPressed(src, Binding{}));
}

TEST_CASE("bindingDown resolves joystick buttons, axes and hats (#1061)", "[bindings]") {
    MockInput in;
    MockJoystick joy;
    auto& dev = joy.addDevice(kStickA);
    dev.buttons.assign(12, false);
    dev.justPressed.assign(12, false);
    dev.axes.assign(8, 0.0f);
    dev.hats.assign(2, HatPosition::Centered);
    JoystickDevices devices;
    devices.update(joy);
    const InputSources src{&in, &joy, &devices, 0};

    // A HOTAS BUTTON reaching an action at all is the headline of #1061: before it, BindingSource had
    // no joystick entry and nothing in game/ or engine/ called IJoystick::isButtonDown.
    const Binding btn = jsButton(5, kStickA);
    CHECK_FALSE(bindingDown(src, btn));
    joy.devices[0].buttons[5] = true;
    CHECK(bindingDown(src, btn));
    CHECK_FALSE(bindingJustPressed(src, btn));
    joy.devices[0].justPressed[5] = true;
    CHECK(bindingJustPressed(src, btn));

    const Binding ax = jsAxis(3, kStickA);
    joy.devices[0].axes[3] = 0.9f;
    CHECK(bindingDown(src, ax));
    joy.devices[0].axes[3] = -0.9f;
    CHECK_FALSE(bindingDown(src, ax));
    CHECK(bindingDown(src, jsAxis(3, kStickA, /*negative=*/true)));

    const Binding hat = jsHat(1, HatPosition::Right, kStickA);
    CHECK_FALSE(bindingDown(src, hat));
    joy.devices[0].hats[1] = HatPosition::DownRight;
    CHECK(bindingDown(src, hat)); // a cardinal binding fires on its diagonals
    joy.devices[0].hats[1] = HatPosition::Left;
    CHECK_FALSE(bindingDown(src, hat));

    // Out-of-range indices are safe, not undefined.
    CHECK_FALSE(bindingDown(src, jsButton(99, kStickA)));
    CHECK_FALSE(bindingDown(src, jsAxis(99, kStickA)));
    CHECK_FALSE(bindingDown(src, jsHat(99, HatPosition::Up, kStickA)));
}

TEST_CASE("bindingDown: a binding on an absent device is inert, not an error (#1061)", "[bindings][device]") {
    MockInput in;
    MockJoystick joy;
    auto& dev = joy.addDevice(kStickA);
    dev.buttons.assign(8, true); // every button held on the stick that IS present
    JoystickDevices devices;
    devices.update(joy);
    const InputSources src{&in, &joy, &devices, 0};

    CHECK(bindingDown(src, jsButton(1, kStickA)));
    // Stick B is not connected. Its bindings do nothing — and crucially they are not resolved against
    // stick A, which is exactly what an index-keyed binding would have done after a replug.
    CHECK_FALSE(bindingDown(src, jsButton(1, kStickB)));
    CHECK(bindingDevicePresent(src, jsButton(1, kStickA)));
    CHECK_FALSE(bindingDevicePresent(src, jsButton(1, kStickB)));
}

TEST_CASE("bindingDown: null hardware makes every binding on it inert", "[bindings]") {
    const InputSources empty{};
    CHECK_FALSE(bindingDown(empty, key(Key::Space)));
    CHECK_FALSE(bindingDown(empty, padButton(GamepadButton::A)));
    CHECK_FALSE(bindingDown(empty, jsButton(0)));
    CHECK_FALSE(bindingJustPressed(empty, key(Key::Space)));
    CHECK_FALSE(bindingJustPressed(empty, jsHat(0, HatPosition::Up)));
}

TEST_CASE("actionDown: suppressDesktop drops keyboard and mouse but keeps stick and pad", "[bindings]") {
    InputBindings b;
    b.set(InputAction::FireWeapon,
          std::vector<Binding>{key(Key::Space), mouse(MouseButton::Left), jsButton(2, kStickA)});

    MockInput in;
    MockJoystick joy;
    joy.addDevice(kStickA).buttons.assign(4, false);
    JoystickDevices devices;
    devices.update(joy);
    const InputSources src{&in, &joy, &devices, 0};

    in.held.insert(Key::Space);
    CHECK(actionDown(src, b, InputAction::FireWeapon));
    // A chat box owns the keyboard and the pointer that clicks its Send button; it does not own the
    // stick, so a partner keeps flying while you type.
    CHECK_FALSE(actionDown(src, b, InputAction::FireWeapon, /*suppressDesktop=*/true));
    joy.devices[0].buttons[2] = true;
    CHECK(actionDown(src, b, InputAction::FireWeapon, /*suppressDesktop=*/true));
}

TEST_CASE("actionAxis: the first ACTIVE axis in the list wins", "[bindings][axis]") {
    InputBindings b;
    AxisConfigTable axes;
    MockInput in;
    in.gamepadCount = 1;
    MockJoystick joy;
    auto& dev = joy.addDevice(kStickA);
    dev.axes.assign(8, 0.0f);
    JoystickDevices devices;
    devices.update(joy);
    const InputSources src{&in, &joy, &devices, 0};

    // The shipped PitchAxis list is [joystick axis 1, gamepad RightY].
    in.axisValues[{0, GamepadAxis::RightY}] = 0.6f;
    CHECK(actionAxis(src, b, axes, InputAction::PitchAxis).active);
    CHECK(actionAxis(src, b, axes, InputAction::PitchAxis).value > 0.0f);

    // With the stick past its own deadzone, the stick wins — the pre-#1061 "HOTAS overrides gamepad"
    // precedence, now expressed as list order rather than as two blocks in the collector.
    joy.devices[0].axes[kHotasAxisPitch] = -0.5f;
    const AxisSample s = actionAxis(src, b, axes, InputAction::PitchAxis);
    CHECK(s.active);
    CHECK(s.value < 0.0f);
}

TEST_CASE("actionAxis: an idle absolute throttle still commands the control", "[bindings][axis]") {
    const InputBindings b;
    const AxisConfigTable axes;
    MockInput in;
    MockJoystick joy;
    joy.addDevice(kStickA).axes.assign(8, 0.0f);
    joy.devices[0].axes[kHotasAxisThrottle] = -1.0f; // lever fully closed
    JoystickDevices devices;
    devices.update(joy);
    const InputSources src{&in, &joy, &devices, 0};

    const AxisSample thr = actionAxis(src, b, axes, InputAction::ThrottleAxis);
    CHECK(thr.active);
    CHECK(thr.value == Catch::Approx(0.0f));
}

TEST_CASE("actionAxis: no hardware means no axis is driving the control", "[bindings][axis]") {
    const InputBindings b;
    const AxisConfigTable axes;
    const InputSources empty{};
    CHECK_FALSE(actionAxis(empty, b, axes, InputAction::ThrottleAxis).active);

    // A gamepad axis on a machine with no pad reads a flat 0; for an Absolute config that would look
    // like an active idle throttle, so presence is checked rather than assumed.
    MockInput in;
    in.gamepadCount = 0;
    const InputSources noPad{&in, nullptr, nullptr, 0};
    InputBindings padOnly;
    padOnly.set(InputAction::ThrottleAxis, std::vector<Binding>{padAxis(GamepadAxis::TriggerLeft)});
    AxisConfigTable absolutePad;
    AxisConfig abs;
    abs.mode = AxisMode::Absolute;
    absolutePad.set(AxisKey{BindingSource::GamepadAxis, static_cast<uint32_t>(GamepadAxis::TriggerLeft), DeviceRef{}},
                    abs);
    CHECK_FALSE(actionAxis(noPad, padOnly, absolutePad, InputAction::ThrottleAxis).active);
    in.gamepadCount = 1;
    CHECK(actionAxis(noPad, padOnly, absolutePad, InputAction::ThrottleAxis).active);
}

TEST_CASE("actionAxis: a digital-only action has no axis", "[bindings][axis]") {
    const InputBindings b;
    const AxisConfigTable axes;
    MockInput in;
    const InputSources src{&in, nullptr, nullptr, 0};
    CHECK_FALSE(actionAxis(src, b, axes, InputAction::FireWeapon).active);
}

TEST_CASE("ActionEdgeTracker derives an edge from the level it observed", "[bindings]") {
    ActionEdgeTracker t;
    CHECK(t.update(InputAction::LandingGear, true));
    CHECK_FALSE(t.update(InputAction::LandingGear, true));
    CHECK(t.wasDown(InputAction::LandingGear));
    CHECK_FALSE(t.update(InputAction::LandingGear, false));
    CHECK(t.update(InputAction::LandingGear, true));
    t.reset();
    CHECK(t.update(InputAction::LandingGear, true));
}

// ---------------------------------------------------------------------------
// JoystickDevices
// ---------------------------------------------------------------------------

TEST_CASE("JoystickDevices: resolve by GUID, and any-device means the first one", "[joystick_devices]") {
    MockJoystick joy;
    JoystickDevices devices;

    devices.update(joy);
    CHECK(devices.resolve(DeviceRef{}) == JoystickDevices::kAbsent);
    CHECK(devices.resolve(makeDeviceRef(kStickA)) == JoystickDevices::kAbsent);

    joy.addDevice(kStickA, "Stick A");
    joy.addDevice(kStickB, "Quadrant B");
    devices.update(joy);
    CHECK(devices.resolve(makeDeviceRef(kStickA)) == 0);
    CHECK(devices.resolve(makeDeviceRef(kStickB)) == 1);
    CHECK(devices.resolve(DeviceRef{}) == 0);
    CHECK(devices.isPresent(makeDeviceRef(kStickB)));
    CHECK_FALSE(devices.isPresent(makeDeviceRef("0300000000000000000000000000cccc")));
}

TEST_CASE("JoystickDevices: a GUID binding survives the index shift a replug causes", "[joystick_devices]") {
    // This is why bindings are keyed by GUID and not by index. SDL renumbers on hot-plug: unplug the
    // first stick and the second one becomes device 0, so an index-keyed binding would silently start
    // driving a different piece of hardware.
    MockJoystick joy;
    joy.addDevice(kStickA, "Stick A");
    joy.addDevice(kStickB, "Quadrant B");
    JoystickDevices devices;
    devices.update(joy);
    REQUIRE(devices.resolve(makeDeviceRef(kStickB)) == 1);

    joy.devices.erase(joy.devices.begin()); // stick A unplugged; B slides down to index 0
    devices.update(joy);
    CHECK(devices.resolve(makeDeviceRef(kStickB)) == 0);
    CHECK(devices.resolve(makeDeviceRef(kStickA)) == JoystickDevices::kAbsent);
}

TEST_CASE("JoystickDevices: changes() reports arrivals and departures", "[joystick_devices]") {
    MockJoystick joy;
    JoystickDevices devices;
    devices.update(joy);
    CHECK(devices.changes().empty());

    joy.addDevice(kStickA, "Stick A");
    devices.update(joy);
    REQUIRE(devices.changes().size() == 1);
    CHECK(devices.changes()[0].added);
    CHECK(devices.changes()[0].name == "Stick A");
    CHECK(devices.changes()[0].guid == kStickA);

    devices.update(joy);
    CHECK(devices.changes().empty()); // steady state is silent

    joy.devices.clear();
    devices.update(joy);
    REQUIRE(devices.changes().size() == 1);
    CHECK_FALSE(devices.changes()[0].added);
    CHECK(devices.changes()[0].guid == kStickA);
}

TEST_CASE("JoystickDevices: hat edges come from the previous frame's position", "[joystick_devices][hat]") {
    MockJoystick joy;
    auto& dev = joy.addDevice(kStickA);
    dev.hats.assign(1, HatPosition::Centered);
    JoystickDevices devices;
    devices.update(joy);
    CHECK_FALSE(devices.hatJustPressed(0, 0, HatPosition::Up));

    joy.devices[0].hats[0] = HatPosition::Up;
    devices.update(joy);
    CHECK(devices.hatJustPressed(0, 0, HatPosition::Up));

    devices.update(joy);
    CHECK_FALSE(devices.hatJustPressed(0, 0, HatPosition::Up)); // held is not a new press

    // Rolling from Up to UpRight must not re-fire a control bound to Up: the player never released it.
    joy.devices[0].hats[0] = HatPosition::UpRight;
    devices.update(joy);
    CHECK_FALSE(devices.hatJustPressed(0, 0, HatPosition::Up));
    // The Right binding, however, has genuinely just been pressed.
    CHECK(devices.hatJustPressed(0, 0, HatPosition::Right));

    joy.devices[0].hats[0] = HatPosition::Centered;
    devices.update(joy);
    CHECK_FALSE(devices.hatJustPressed(0, 0, HatPosition::Up));
    joy.devices[0].hats[0] = HatPosition::Up;
    devices.update(joy);
    CHECK(devices.hatJustPressed(0, 0, HatPosition::Up));

    // Out-of-range hat / device indices are safe.
    CHECK_FALSE(devices.hatJustPressed(0, 9, HatPosition::Up));
    CHECK_FALSE(devices.hatJustPressed(7, 0, HatPosition::Up));
}

TEST_CASE("JoystickDevices: hat history is keyed by GUID, so a replug cannot fake an edge", "[joystick_devices][hat]") {
    MockJoystick joy;
    joy.addDevice(kStickA).hats.assign(1, HatPosition::Centered);
    auto& b = joy.addDevice(kStickB);
    b.hats.assign(1, HatPosition::Up); // stick B is holding its hat up the whole time
    JoystickDevices devices;
    devices.update(joy);
    devices.update(joy);
    REQUIRE_FALSE(devices.hatJustPressed(1, 0, HatPosition::Up)); // held, no edge

    joy.devices.erase(joy.devices.begin()); // stick A leaves; B becomes index 0
    devices.update(joy);
    // If history were kept by index, B's held hat would inherit A's centered history and look like a
    // fresh press.
    CHECK_FALSE(devices.hatJustPressed(0, 0, HatPosition::Up));
}

TEST_CASE("bindingJustPressed: a hat edge needs the device table", "[bindings][hat]") {
    MockInput in;
    MockJoystick joy;
    joy.addDevice(kStickA).hats.assign(1, HatPosition::Centered);
    JoystickDevices devices;
    devices.update(joy);
    joy.devices[0].hats[0] = HatPosition::Down;
    devices.update(joy);

    const Binding hat = jsHat(0, HatPosition::Down, kStickA);
    CHECK(bindingJustPressed(InputSources{&in, &joy, &devices, 0}, hat));
    // The HAL has no just-pressed for hats, so with no table there is no edge to report.
    CHECK_FALSE(bindingJustPressed(InputSources{&in, &joy, nullptr, 0}, hat));
}

// ---------------------------------------------------------------------------
// Legacy HOTAS migration (#1061)
// ---------------------------------------------------------------------------

TEST_CASE("migrateLegacyHotas: a customised [controls] mapping lands in the table", "[bindings][migration]") {
    InputBindings b;
    AxisConfigTable axes;
    LegacyHotasAxes legacy;
    legacy.present = true;
    legacy.aileronAxis = 4;
    legacy.elevatorAxis = 5;
    legacy.throttleAxis = 6;
    legacy.rudderAxis = 7;
    legacy.deadzone = 0.12f;
    legacy.invertPitch = true;
    legacy.invertThrottle = true;

    migrateLegacyHotas(legacy, b, axes);

    CHECK(b.first(InputAction::RollAxis).source == BindingSource::JoystickAxis);
    CHECK(b.first(InputAction::RollAxis).id == 4u);
    CHECK(b.first(InputAction::PitchAxis).id == 5u);
    CHECK(b.first(InputAction::ThrottleAxis).id == 6u);
    CHECK(b.first(InputAction::YawAxis).id == 7u);
    // The gamepad bindings that were already on those actions are kept, just behind the stick.
    CHECK(hasBinding(b, InputAction::PitchAxis, padAxis(GamepadAxis::RightY)));

    const AxisConfig pitch = axes.effective(AxisKey{BindingSource::JoystickAxis, 5, DeviceRef{}});
    CHECK(pitch.deadzone == Catch::Approx(0.12f));
    CHECK(pitch.invert);
    CHECK(pitch.mode == AxisMode::Centered);
    // The throttle was the only absolute one: the old code remapped its full travel and never applied
    // the deadzone to it.
    const AxisConfig thr = axes.effective(AxisKey{BindingSource::JoystickAxis, 6, DeviceRef{}});
    CHECK(thr.mode == AxisMode::Absolute);
    CHECK(thr.invert);
}

TEST_CASE("migrateLegacyHotas: -1 kept an axis switched off", "[bindings][migration]") {
    InputBindings b;
    AxisConfigTable axes;
    LegacyHotasAxes legacy;
    legacy.present = true;
    legacy.aileronAxis = -1;
    legacy.elevatorAxis = -1;
    legacy.throttleAxis = -1;
    legacy.rudderAxis = -1;

    migrateLegacyHotas(legacy, b, axes);
    // The player disabled these; the migration must not helpfully bind them back.
    for (InputAction a :
         {InputAction::RollAxis, InputAction::PitchAxis, InputAction::ThrottleAxis, InputAction::YawAxis}) {
        for (const Binding& x : b.get(a))
            CHECK(x.source != BindingSource::JoystickAxis);
    }
}

TEST_CASE("migrateLegacyHotas: with nothing stored the shipped layout is applied", "[bindings][migration]") {
    // A version-2 bindings.toml describes NO joystick axes — they were in user.toml. Loading one and
    // stopping there would leave a working HOTAS dead, so the axes go in either way.
    InputBindings b;
    for (InputAction a :
         {InputAction::RollAxis, InputAction::PitchAxis, InputAction::ThrottleAxis, InputAction::YawAxis})
        b.set(a, std::vector<Binding>{padAxis(GamepadAxis::RightY)});
    AxisConfigTable axes;

    migrateLegacyHotas(LegacyHotasAxes{}, b, axes);
    CHECK(b.first(InputAction::RollAxis).id == kHotasAxisRoll);
    CHECK(b.first(InputAction::PitchAxis).id == kHotasAxisPitch);
    CHECK(b.first(InputAction::ThrottleAxis).id == kHotasAxisThrottle);
    CHECK(b.first(InputAction::YawAxis).id == kHotasAxisYaw);
    CHECK(axes.effective(AxisKey{BindingSource::JoystickAxis, kHotasAxisThrottle, DeviceRef{}}).mode ==
          AxisMode::Absolute);
}

TEST_CASE("migrateLegacyHotas is idempotent", "[bindings][migration]") {
    InputBindings b;
    AxisConfigTable axes;
    LegacyHotasAxes legacy;
    legacy.present = true;
    legacy.elevatorAxis = 5;

    migrateLegacyHotas(legacy, b, axes);
    const int after = b.count(InputAction::PitchAxis);
    migrateLegacyHotas(legacy, b, axes);
    CHECK(b.count(InputAction::PitchAxis) == after);
    CHECK(b.findConflicts().empty());
}
