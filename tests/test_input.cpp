// SPDX-License-Identifier: GPL-3.0-or-later
#include "input/AxisConfig.h"
#include "input/BindingQuery.h"
#include "input/InputBindings.h"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "mock_hal.h" // MockInput (engine-input links platform-hal, so the platform headers resolve)

using namespace fl;

// ---------------------------------------------------------------------------
// AxisConfig::apply
// ---------------------------------------------------------------------------

TEST_CASE("AxisConfig dead zone clamps to zero", "[axis_config]") {
    AxisConfig cfg;
    cfg.deadzone = 0.1f;

    CHECK(cfg.apply(0.0f) == Catch::Approx(0.0f));
    CHECK(cfg.apply(0.05f) == Catch::Approx(0.0f));
    CHECK(cfg.apply(-0.09f) == Catch::Approx(0.0f));
    // Exactly at the boundary is still clamped
    CHECK(cfg.apply(0.1f) == Catch::Approx(0.0f));
}

TEST_CASE("AxisConfig linear rescaling is correct", "[axis_config]") {
    AxisConfig cfg;
    cfg.deadzone = 0.0f;
    cfg.curve = AxisCurve::Linear;
    cfg.invert = false;
    cfg.scale = 1.0f;

    CHECK(cfg.apply(1.0f) == Catch::Approx(1.0f));
    CHECK(cfg.apply(-1.0f) == Catch::Approx(-1.0f));
    CHECK(cfg.apply(0.5f) == Catch::Approx(0.5f));
}

TEST_CASE("AxisConfig rescales from [deadzone,1] to [0,1]", "[axis_config]") {
    AxisConfig cfg;
    cfg.deadzone = 0.5f;
    cfg.curve = AxisCurve::Linear;
    cfg.scale = 1.0f;

    // Just past deadzone → small positive value
    CHECK(cfg.apply(0.5f + 1e-4f) > 0.0f);
    // Full deflection → 1.0
    CHECK(cfg.apply(1.0f) == Catch::Approx(1.0f));
}

TEST_CASE("AxisConfig cubic curve produces expected shape", "[axis_config]") {
    AxisConfig cfg;
    cfg.deadzone = 0.0f;
    cfg.curve = AxisCurve::Cubic;
    cfg.scale = 1.0f;

    // At t=0.5, cubic gives 0.5^3 = 0.125
    CHECK(cfg.apply(0.5f) == Catch::Approx(0.125f).margin(1e-5f));
    // Cubic is always less than linear for t in (0,1)
    CHECK(cfg.apply(0.8f) < 0.8f);
    // Endpoints unchanged
    CHECK(cfg.apply(0.0f) == Catch::Approx(0.0f));
    CHECK(cfg.apply(1.0f) == Catch::Approx(1.0f));
}

TEST_CASE("AxisConfig invert flips sign", "[axis_config]") {
    AxisConfig cfg;
    cfg.deadzone = 0.0f;
    cfg.invert = true;
    cfg.scale = 1.0f;

    CHECK(cfg.apply(1.0f) == Catch::Approx(-1.0f));
    CHECK(cfg.apply(-1.0f) == Catch::Approx(1.0f));
    CHECK(cfg.apply(0.5f) == Catch::Approx(-0.5f));
}

TEST_CASE("AxisConfig scale multiplies output", "[axis_config]") {
    AxisConfig cfg;
    cfg.deadzone = 0.0f;
    cfg.scale = 2.0f;

    CHECK(cfg.apply(0.5f) == Catch::Approx(1.0f));
}

TEST_CASE("AxisConfig negative input mirrors positive", "[axis_config]") {
    AxisConfig cfg;
    cfg.deadzone = 0.1f;
    cfg.curve = AxisCurve::Cubic;
    cfg.scale = 1.0f;

    float pos = cfg.apply(0.7f);
    float neg = cfg.apply(-0.7f);
    CHECK(pos == Catch::Approx(-neg).margin(1e-5f));
}

// ---------------------------------------------------------------------------
// AxisConfigTable serialization
// ---------------------------------------------------------------------------

TEST_CASE("AxisConfigTable TOML roundtrip", "[axis_config]") {
    AxisConfigTable t;
    t.get(GamepadAxis::LeftY).deadzone = 0.15f;
    t.get(GamepadAxis::LeftY).curve = AxisCurve::Cubic;
    t.get(GamepadAxis::LeftY).invert = true;
    t.get(GamepadAxis::LeftY).scale = 0.9f;

    std::string toml = t.serialize();

    AxisConfigTable t2;
    REQUIRE(t2.deserialize(toml));
    CHECK(t2.get(GamepadAxis::LeftY).deadzone == Catch::Approx(0.15f));
    CHECK(t2.get(GamepadAxis::LeftY).curve == AxisCurve::Cubic);
    CHECK(t2.get(GamepadAxis::LeftY).invert == true);
    CHECK(t2.get(GamepadAxis::LeftY).scale == Catch::Approx(0.9f));
}

// ---------------------------------------------------------------------------
// InputBindings defaults
// ---------------------------------------------------------------------------

TEST_CASE("InputBindings default constructor applies defaults", "[bindings]") {
    InputBindings b;
    // Keyboard primary: the arrows fly the aircraft (#1050 — the table used to claim S/W/A/D while
    // the game read the arrows, and claim the arrows for the cockpit view pan at the same time).
    Binding pitch = b.get(InputAction::PitchUp);
    CHECK(pitch.source == BindingSource::Keyboard);
    CHECK(pitch.id == static_cast<uint32_t>(Key::ArrowDown));
    CHECK(b.get(InputAction::PitchDown).id == static_cast<uint32_t>(Key::ArrowUp));
    CHECK(b.get(InputAction::RollLeft).id == static_cast<uint32_t>(Key::ArrowLeft));
    CHECK(b.get(InputAction::RollRight).id == static_cast<uint32_t>(Key::ArrowRight));

    // Secondary slot: the gun is Space AND the left mouse button. Two slots could not hold both
    // plus the pad, which is how Space ended up hardcoded in FlightInputCollector.
    CHECK(b.get(InputAction::FireWeapon).id == static_cast<uint32_t>(Key::Space));
    CHECK(b.get(InputAction::FireWeapon, BindingSlot::Secondary).source == BindingSource::MouseButton);
    CHECK(b.get(InputAction::FireWeapon, BindingSlot::Secondary).id == static_cast<uint32_t>(MouseButton::Left));

    // Gamepad slot: PitchAxis bound to RightY
    Binding pitchAxis = b.get(InputAction::PitchAxis, BindingSlot::Gamepad);
    CHECK(pitchAxis.source == BindingSource::GamepadAxis);
    CHECK(pitchAxis.id == static_cast<uint32_t>(GamepadAxis::RightY));
}

// ---------------------------------------------------------------------------
// THE GATE #1050 EXISTS FOR: the shipped defaults are held to the same conflict rule as a user
// rebind. Nothing applied conflictsWith() to applyDefaults(), so V shipped bound to BOTH the radio
// push-to-talk and master arm — keying the mic safed the guns, and rebinding either did not help.
// ---------------------------------------------------------------------------

TEST_CASE("InputBindings: the shipped defaults contain no conflicts", "[bindings]") {
    const InputBindings b;
    const auto conflicts = b.findConflicts();
    for (const auto& c : conflicts) {
        UNSCOPED_INFO("default conflict: " << InputBindings::actionName(c.a) << " (" << InputBindings::slotName(c.slotA)
                                           << ") vs " << InputBindings::actionName(c.b) << " ("
                                           << InputBindings::slotName(c.slotB) << ")");
    }
    CHECK(conflicts.empty());
}

TEST_CASE("InputBindings: every action a default binds resolves back through its own name", "[bindings]") {
    // A default on a key with no name would serialize as "Unknown" and be silently dropped on the
    // next load — the defaults would then differ between a fresh install and a second launch.
    const InputBindings b;
    const std::string toml = b.serialize();
    CHECK(toml.find("\"Unknown\"") == std::string::npos);
}

TEST_CASE("InputBindings: V is push-to-talk and nothing else live at the same time (#1050)", "[bindings]") {
    const InputBindings b;
    CHECK(b.get(InputAction::PushToTalkPrimary).id == static_cast<uint32_t>(Key::V));
    // Master arm moved off V and now has a binding of its own.
    CHECK(b.get(InputAction::MasterArm).id != static_cast<uint32_t>(Key::V));
    CHECK_FALSE(b.get(InputAction::MasterArm).isNone());
    CHECK_FALSE(b.conflictsWith(InputAction::MasterArm, b.get(InputAction::MasterArm)).has_value());
    CHECK_FALSE(b.conflictsWith(InputAction::PushToTalkPrimary, b.get(InputAction::PushToTalkPrimary)).has_value());
}

TEST_CASE("InputBindings: sharing a key across disjoint contexts is not a conflict", "[bindings]") {
    const InputBindings b;
    // Space is the gun while flying and the replay pause key while watching a recording. There is
    // no session in which both are read, so the checker must NOT flag it — a checker that flagged
    // every reuse would have been turned off, which is the failure mode after "check nothing".
    CHECK(b.get(InputAction::FireWeapon).id == b.get(InputAction::ReplayPauseToggle).id);
    CHECK_FALSE(contextsOverlap(InputBindings::contexts(InputAction::FireWeapon),
                                InputBindings::contexts(InputAction::ReplayPauseToggle)));
    CHECK_FALSE(b.conflictsWith(InputAction::ReplayPauseToggle, b.get(InputAction::FireWeapon)).has_value());

    // ...but sharing a key inside ONE context still is. This is the V case.
    InputBindings clash;
    clash.set(InputAction::MasterArm, clash.get(InputAction::PushToTalkPrimary));
    auto c = clash.conflictsWith(InputAction::MasterArm, clash.get(InputAction::MasterArm));
    REQUIRE(c.has_value());
    CHECK(*c == InputAction::PushToTalkPrimary);
    CHECK_FALSE(clash.findConflicts().empty());
}

TEST_CASE("InputBindings: no live control shares a key with the free camera", "[bindings]") {
    // The free camera is reachable in every session mode, including while flying, so its movement
    // keys overlap everything. Before #1050 they were raw SDL scancodes and the checker could not
    // see that E both panned the camera up and dispensed countermeasures.
    const InputBindings b;
    const InputAction cam[] = {InputAction::FreeCamForward, InputAction::FreeCamBack,   InputAction::FreeCamLeft,
                               InputAction::FreeCamRight,   InputAction::FreeCamUp,     InputAction::FreeCamDown,
                               InputAction::FreeCamFaster,  InputAction::FreeCamSlower, InputAction::FreeCamReset};
    for (InputAction a : cam) {
        INFO("free-camera action " << InputBindings::actionName(a));
        CHECK_FALSE(b.get(a).isNone());
        CHECK_FALSE(b.conflictsWith(a, b.get(a)).has_value());
    }
}

TEST_CASE("InputBindings: three slots round-trip independently", "[bindings]") {
    InputBindings b;
    b.set(InputAction::Airbrake, {BindingSource::Keyboard, static_cast<uint32_t>(Key::Backslash), false});
    b.set(InputAction::Airbrake, {BindingSource::MouseButton, static_cast<uint32_t>(MouseButton::Middle), false},
          BindingSlot::Secondary);
    b.set(InputAction::Airbrake, {BindingSource::GamepadButton, static_cast<uint32_t>(GamepadButton::Y), false},
          BindingSlot::Gamepad);

    InputBindings b2;
    REQUIRE(b2.deserialize(b.serialize()));
    CHECK(b2.get(InputAction::Airbrake).id == static_cast<uint32_t>(Key::Backslash));
    CHECK(b2.get(InputAction::Airbrake, BindingSlot::Secondary).source == BindingSource::MouseButton);
    CHECK(b2.get(InputAction::Airbrake, BindingSlot::Gamepad).id == static_cast<uint32_t>(GamepadButton::Y));
}

TEST_CASE("InputBindings: the serialized table carries its format version", "[bindings]") {
    // A stored bindings.toml is a full table, not a patch, so an unversioned file from an older
    // build would load the OLD defaults back over the new ones — silently restoring the very
    // collisions #1050 removed, for every player who never customised anything.
    const InputBindings b;
    CHECK(InputBindings::fileFormatVersion(b.serialize()) == InputBindings::kFormatVersion);
    CHECK(InputBindings::fileFormatVersion("[primary]\nPitchUp = { source = \"Keyboard\", id = \"S\" }\n") == 0);
    CHECK(InputBindings::fileFormatVersion("not valid toml }{") == 0);
}

TEST_CASE("InputBindings: a legacy [alt] section still loads as the gamepad slot", "[bindings]") {
    // [alt] was the gamepad section before the third slot existed. A bindings.toml written by an
    // older build must not silently lose its pad bindings.
    InputBindings b;
    REQUIRE(b.deserialize("[alt]\nAirbrake = { source = \"GamepadButton\", id = \"X\" }\n"));
    CHECK(b.get(InputAction::Airbrake, BindingSlot::Gamepad).source == BindingSource::GamepadButton);
    CHECK(b.get(InputAction::Airbrake, BindingSlot::Gamepad).id == static_cast<uint32_t>(GamepadButton::X));
}

// ---------------------------------------------------------------------------
// InputBindings set / get / clear
// ---------------------------------------------------------------------------

TEST_CASE("InputBindings set and get roundtrip", "[bindings]") {
    InputBindings b;
    Binding fire{BindingSource::Keyboard, static_cast<uint32_t>(Key::Space), false};
    b.set(InputAction::FireWeapon, fire);
    Binding got = b.get(InputAction::FireWeapon);
    CHECK(got.source == fire.source);
    CHECK(got.id == fire.id);
}

TEST_CASE("InputBindings clear sets source to None", "[bindings]") {
    InputBindings b;
    b.clear(InputAction::PitchUp);
    CHECK(b.get(InputAction::PitchUp).isNone());
}

// ---------------------------------------------------------------------------
// InputBindings conflict detection
// ---------------------------------------------------------------------------

TEST_CASE("InputBindings detects conflict on duplicate binding", "[bindings]") {
    InputBindings b;
    b.applyDefaults();

    // Assign the key PitchUp already has (ArrowDown) to RollLeft — both are Flight-context.
    Binding s{BindingSource::Keyboard, static_cast<uint32_t>(Key::ArrowDown), false};
    auto conflict = b.conflictsWith(InputAction::RollLeft, s);
    REQUIRE(conflict.has_value());
    CHECK(*conflict == InputAction::PitchUp);
}

TEST_CASE("InputBindings no conflict after clearing the conflicting action", "[bindings]") {
    InputBindings b;
    b.applyDefaults();
    b.clear(InputAction::PitchUp);

    Binding s{BindingSource::Keyboard, static_cast<uint32_t>(Key::ArrowDown), false};
    CHECK_FALSE(b.conflictsWith(InputAction::RollLeft, s).has_value());
}

TEST_CASE("InputBindings no conflict for None binding", "[bindings]") {
    InputBindings b;
    CHECK_FALSE(b.conflictsWith(InputAction::RollLeft, Binding{}).has_value());
}

TEST_CASE("InputBindings does not conflict with its own action", "[bindings]") {
    InputBindings b;
    // PitchUp is bound to ArrowDown; asking conflictsWith(PitchUp, ArrowDown) returns nullopt
    Binding s{BindingSource::Keyboard, static_cast<uint32_t>(Key::ArrowDown), false};
    CHECK_FALSE(b.conflictsWith(InputAction::PitchUp, s).has_value());
}

// ---------------------------------------------------------------------------
// InputBindings TOML roundtrip
// ---------------------------------------------------------------------------

TEST_CASE("InputBindings TOML serialize then deserialize reproduces all bindings", "[bindings]") {
    InputBindings original;
    original.applyDefaults();

    std::string toml = original.serialize();

    InputBindings loaded;
    // Clear loaded first so we can confirm deserialize fills it in
    for (int i = 0; i < InputBindings::kActionCount; ++i) {
        loaded.clear(static_cast<InputAction>(i));
        loaded.clear(static_cast<InputAction>(i), BindingSlot::Gamepad);
    }
    REQUIRE(loaded.deserialize(toml));

    for (int i = 0; i < InputBindings::kActionCount; ++i) {
        auto action = static_cast<InputAction>(i);
        Binding a = original.get(action);
        Binding b = loaded.get(action);
        INFO("Primary binding mismatch for action " << i);
        CHECK(a.source == b.source);
        CHECK(a.id == b.id);
        CHECK(a.axisNegative == b.axisNegative);

        Binding aAlt = original.get(action, BindingSlot::Gamepad);
        Binding bAlt = loaded.get(action, BindingSlot::Gamepad);
        INFO("Alt binding mismatch for action " << i);
        CHECK(aAlt.source == bAlt.source);
        CHECK(aAlt.id == bAlt.id);
        CHECK(aAlt.axisNegative == bAlt.axisNegative);
    }
}

TEST_CASE("InputBindings deserialize returns false on invalid TOML", "[bindings]") {
    InputBindings b;
    CHECK_FALSE(b.deserialize("this is not valid toml }{"));
}

TEST_CASE("InputBindings deserialize returns false on unrecognised key name", "[bindings]") {
    InputBindings b;
    CHECK_FALSE(b.deserialize("[primary]\nPitchUp = { source = \"Keyboard\", id = \"NotAKey\" }\n"));
}

TEST_CASE("InputBindings deserialize ignores unknown action names", "[bindings]") {
    InputBindings b;
    CHECK(b.deserialize("[primary]\nUnknownAction = { source = \"Keyboard\", id = \"A\" }\n"));
}

// ---------------------------------------------------------------------------
// InputBindings — serialization coverage for binding source types not in defaults
// ---------------------------------------------------------------------------

TEST_CASE("InputBindings roundtrips GamepadButton values not in defaults", "[bindings]") {
    InputBindings b;
    for (int i = 0; i < InputBindings::kActionCount; ++i) {
        b.clear(static_cast<InputAction>(i));
        b.clear(static_cast<InputAction>(i), BindingSlot::Gamepad);
    }
    b.set(InputAction::RollLeft, {BindingSource::GamepadButton, static_cast<uint32_t>(GamepadButton::X), false});
    b.set(InputAction::RollRight, {BindingSource::GamepadButton, static_cast<uint32_t>(GamepadButton::Y), false});
    b.set(InputAction::YawLeft, {BindingSource::GamepadButton, static_cast<uint32_t>(GamepadButton::LeftStick), false});
    b.set(InputAction::YawRight,
          {BindingSource::GamepadButton, static_cast<uint32_t>(GamepadButton::RightStick), false});
    b.set(InputAction::ViewUp, {BindingSource::GamepadButton, static_cast<uint32_t>(GamepadButton::DpadUp), false});
    b.set(InputAction::ViewLeft, {BindingSource::GamepadButton, static_cast<uint32_t>(GamepadButton::DpadLeft), false});
    b.set(InputAction::ViewRight,
          {BindingSource::GamepadButton, static_cast<uint32_t>(GamepadButton::DpadRight), false});

    InputBindings b2;
    for (int i = 0; i < InputBindings::kActionCount; ++i) {
        b2.clear(static_cast<InputAction>(i));
        b2.clear(static_cast<InputAction>(i), BindingSlot::Gamepad);
    }
    REQUIRE(b2.deserialize(b.serialize()));
    CHECK(b2.get(InputAction::RollLeft).id == static_cast<uint32_t>(GamepadButton::X));
    CHECK(b2.get(InputAction::RollRight).id == static_cast<uint32_t>(GamepadButton::Y));
    CHECK(b2.get(InputAction::YawLeft).id == static_cast<uint32_t>(GamepadButton::LeftStick));
    CHECK(b2.get(InputAction::YawRight).id == static_cast<uint32_t>(GamepadButton::RightStick));
    CHECK(b2.get(InputAction::ViewUp).id == static_cast<uint32_t>(GamepadButton::DpadUp));
    CHECK(b2.get(InputAction::ViewLeft).id == static_cast<uint32_t>(GamepadButton::DpadLeft));
    CHECK(b2.get(InputAction::ViewRight).id == static_cast<uint32_t>(GamepadButton::DpadRight));
}

TEST_CASE("InputBindings roundtrips GamepadAxis RightY and TriggerRight with axisNegative", "[bindings]") {
    InputBindings b;
    b.set(InputAction::PitchAxis, {BindingSource::GamepadAxis, static_cast<uint32_t>(GamepadAxis::RightY), false});
    b.set(InputAction::RollAxis, {BindingSource::GamepadAxis, static_cast<uint32_t>(GamepadAxis::TriggerRight), true});

    InputBindings b2;
    REQUIRE(b2.deserialize(b.serialize()));
    CHECK(b2.get(InputAction::PitchAxis).id == static_cast<uint32_t>(GamepadAxis::RightY));
    CHECK(b2.get(InputAction::RollAxis).id == static_cast<uint32_t>(GamepadAxis::TriggerRight));
    CHECK(b2.get(InputAction::RollAxis).axisNegative == true);
}

TEST_CASE("InputBindings roundtrips MouseButton Middle", "[bindings]") {
    InputBindings b;
    b.set(InputAction::FireStore, {BindingSource::MouseButton, static_cast<uint32_t>(MouseButton::Middle), false});

    InputBindings b2;
    REQUIRE(b2.deserialize(b.serialize()));
    CHECK(b2.get(InputAction::FireStore).source == BindingSource::MouseButton);
    CHECK(b2.get(InputAction::FireStore).id == static_cast<uint32_t>(MouseButton::Middle));
}

TEST_CASE("InputBindings roundtrips keyboard keys not in default bindings", "[bindings]") {
    InputBindings b;
    for (int i = 0; i < InputBindings::kActionCount; ++i) {
        b.clear(static_cast<InputAction>(i));
        b.clear(static_cast<InputAction>(i), BindingSlot::Gamepad);
    }
    b.set(InputAction::ViewUp, {BindingSource::Keyboard, static_cast<uint32_t>(Key::F1), false});
    b.set(InputAction::ViewDown, {BindingSource::Keyboard, static_cast<uint32_t>(Key::F12), false});
    b.set(InputAction::ViewLeft, {BindingSource::Keyboard, static_cast<uint32_t>(Key::LeftCtrl), false});
    b.set(InputAction::ViewRight, {BindingSource::Keyboard, static_cast<uint32_t>(Key::RightAlt), false});
    b.set(InputAction::LandingGear, {BindingSource::Keyboard, static_cast<uint32_t>(Key::Enter), false});
    b.set(InputAction::Flaps, {BindingSource::Keyboard, static_cast<uint32_t>(Key::Backspace), false});
    b.set(InputAction::Pause, {BindingSource::Keyboard, static_cast<uint32_t>(Key::Insert), false});
    b.set(InputAction::CommsMenu, {BindingSource::Keyboard, static_cast<uint32_t>(Key::PageUp), false});

    InputBindings b2;
    for (int i = 0; i < InputBindings::kActionCount; ++i) {
        b2.clear(static_cast<InputAction>(i));
        b2.clear(static_cast<InputAction>(i), BindingSlot::Gamepad);
    }
    REQUIRE(b2.deserialize(b.serialize()));
    CHECK(b2.get(InputAction::ViewUp).id == static_cast<uint32_t>(Key::F1));
    CHECK(b2.get(InputAction::ViewDown).id == static_cast<uint32_t>(Key::F12));
    CHECK(b2.get(InputAction::ViewLeft).id == static_cast<uint32_t>(Key::LeftCtrl));
    CHECK(b2.get(InputAction::ViewRight).id == static_cast<uint32_t>(Key::RightAlt));
    CHECK(b2.get(InputAction::LandingGear).id == static_cast<uint32_t>(Key::Enter));
    CHECK(b2.get(InputAction::Flaps).id == static_cast<uint32_t>(Key::Backspace));
    CHECK(b2.get(InputAction::Pause).id == static_cast<uint32_t>(Key::Insert));
    CHECK(b2.get(InputAction::CommsMenu).id == static_cast<uint32_t>(Key::PageUp));
}

// ---------------------------------------------------------------------------
// AxisConfig — overrange clamping
// ---------------------------------------------------------------------------

TEST_CASE("AxisConfig clamps input beyond 1.0 to 1.0", "[axis_config]") {
    AxisConfig cfg;
    cfg.deadzone = 0.0f;
    cfg.scale = 1.0f;
    CHECK(cfg.apply(1.5f) == Catch::Approx(1.0f));
    CHECK(cfg.apply(-1.5f) == Catch::Approx(-1.0f));
}

// ---------------------------------------------------------------------------
// AxisConfigTable — absent section keeps defaults
// ---------------------------------------------------------------------------

TEST_CASE("AxisConfigTable deserialize with no axis_config section keeps defaults", "[axis_config]") {
    AxisConfigTable t;
    t.get(GamepadAxis::LeftY).deadzone = 0.2f;
    REQUIRE(t.deserialize("[other_section]\nfoo = 1\n"));
    CHECK(t.get(GamepadAxis::LeftY).deadzone == Catch::Approx(0.2f));
}

// ---------------------------------------------------------------------------
// InputBindings — comprehensive key name coverage
// Exercises keyFromName / keyName branches for all keys not covered by
// the default bindings or the earlier per-type roundtrip tests.
// ---------------------------------------------------------------------------

TEST_CASE("InputBindings roundtrips all uncovered keyboard letter and digit keys", "[bindings]") {
    InputBindings b;
    for (int i = 0; i < InputBindings::kActionCount; ++i) {
        b.clear(static_cast<InputAction>(i));
        b.clear(static_cast<InputAction>(i), BindingSlot::Gamepad);
    }

    // Primary: uncovered letter keys B C H I J K L M N O P R T U V X Y Z
    b.set(InputAction::PitchAxis, {BindingSource::Keyboard, static_cast<uint32_t>(Key::B), false});
    b.set(InputAction::RollAxis, {BindingSource::Keyboard, static_cast<uint32_t>(Key::C), false});
    b.set(InputAction::YawAxis, {BindingSource::Keyboard, static_cast<uint32_t>(Key::H), false});
    b.set(InputAction::ThrottleAxis, {BindingSource::Keyboard, static_cast<uint32_t>(Key::I), false});
    b.set(InputAction::PitchUp, {BindingSource::Keyboard, static_cast<uint32_t>(Key::J), false});
    b.set(InputAction::PitchDown, {BindingSource::Keyboard, static_cast<uint32_t>(Key::K), false});
    b.set(InputAction::RollLeft, {BindingSource::Keyboard, static_cast<uint32_t>(Key::L), false});
    b.set(InputAction::RollRight, {BindingSource::Keyboard, static_cast<uint32_t>(Key::M), false});
    b.set(InputAction::YawLeft, {BindingSource::Keyboard, static_cast<uint32_t>(Key::N), false});
    b.set(InputAction::YawRight, {BindingSource::Keyboard, static_cast<uint32_t>(Key::O), false});
    b.set(InputAction::ThrottleUp, {BindingSource::Keyboard, static_cast<uint32_t>(Key::P), false});
    b.set(InputAction::ThrottleDown, {BindingSource::Keyboard, static_cast<uint32_t>(Key::R), false});
    b.set(InputAction::Airbrake, {BindingSource::Keyboard, static_cast<uint32_t>(Key::T), false});
    b.set(InputAction::Afterburner, {BindingSource::Keyboard, static_cast<uint32_t>(Key::U), false});
    b.set(InputAction::FireWeapon, {BindingSource::Keyboard, static_cast<uint32_t>(Key::V), false});
    b.set(InputAction::FireStore, {BindingSource::Keyboard, static_cast<uint32_t>(Key::X), false});
    b.set(InputAction::NextWeapon, {BindingSource::Keyboard, static_cast<uint32_t>(Key::Y), false});
    b.set(InputAction::PrevWeapon, {BindingSource::Keyboard, static_cast<uint32_t>(Key::Z), false});
    // Digit keys 0, 3-9 (1 and 2 are in defaults)
    b.set(InputAction::ViewUp, {BindingSource::Keyboard, static_cast<uint32_t>(Key::Num0), false});
    b.set(InputAction::ViewDown, {BindingSource::Keyboard, static_cast<uint32_t>(Key::Num3), false});
    b.set(InputAction::ViewLeft, {BindingSource::Keyboard, static_cast<uint32_t>(Key::Num4), false});
    b.set(InputAction::ViewRight, {BindingSource::Keyboard, static_cast<uint32_t>(Key::Num5), false});
    b.set(InputAction::LandingGear, {BindingSource::Keyboard, static_cast<uint32_t>(Key::Num6), false});
    b.set(InputAction::Flaps, {BindingSource::Keyboard, static_cast<uint32_t>(Key::Num7), false});
    b.set(InputAction::Pause, {BindingSource::Keyboard, static_cast<uint32_t>(Key::Num8), false});
    b.set(InputAction::CommsMenu, {BindingSource::Keyboard, static_cast<uint32_t>(Key::Num9), false});

    // Alt: uncovered navigation / F-key / modifier keys
    b.set(InputAction::PitchAxis, {BindingSource::Keyboard, static_cast<uint32_t>(Key::Delete), false},
          BindingSlot::Gamepad);
    b.set(InputAction::RollAxis, {BindingSource::Keyboard, static_cast<uint32_t>(Key::ArrowLeft), false},
          BindingSlot::Gamepad);
    b.set(InputAction::YawAxis, {BindingSource::Keyboard, static_cast<uint32_t>(Key::ArrowRight), false},
          BindingSlot::Gamepad);
    b.set(InputAction::ThrottleAxis, {BindingSource::Keyboard, static_cast<uint32_t>(Key::Home), false},
          BindingSlot::Gamepad);
    b.set(InputAction::PitchUp, {BindingSource::Keyboard, static_cast<uint32_t>(Key::End), false},
          BindingSlot::Gamepad);
    b.set(InputAction::PitchDown, {BindingSource::Keyboard, static_cast<uint32_t>(Key::PageDown), false},
          BindingSlot::Gamepad);
    b.set(InputAction::RollLeft, {BindingSource::Keyboard, static_cast<uint32_t>(Key::F2), false},
          BindingSlot::Gamepad);
    b.set(InputAction::RollRight, {BindingSource::Keyboard, static_cast<uint32_t>(Key::F3), false},
          BindingSlot::Gamepad);
    b.set(InputAction::YawLeft, {BindingSource::Keyboard, static_cast<uint32_t>(Key::F4), false}, BindingSlot::Gamepad);
    b.set(InputAction::YawRight, {BindingSource::Keyboard, static_cast<uint32_t>(Key::F5), false},
          BindingSlot::Gamepad);
    b.set(InputAction::ThrottleUp, {BindingSource::Keyboard, static_cast<uint32_t>(Key::F6), false},
          BindingSlot::Gamepad);
    b.set(InputAction::ThrottleDown, {BindingSource::Keyboard, static_cast<uint32_t>(Key::F7), false},
          BindingSlot::Gamepad);
    b.set(InputAction::Airbrake, {BindingSource::Keyboard, static_cast<uint32_t>(Key::F8), false},
          BindingSlot::Gamepad);
    b.set(InputAction::Afterburner, {BindingSource::Keyboard, static_cast<uint32_t>(Key::F9), false},
          BindingSlot::Gamepad);
    b.set(InputAction::FireWeapon, {BindingSource::Keyboard, static_cast<uint32_t>(Key::F10), false},
          BindingSlot::Gamepad);
    b.set(InputAction::FireStore, {BindingSource::Keyboard, static_cast<uint32_t>(Key::F11), false},
          BindingSlot::Gamepad);
    b.set(InputAction::NextWeapon, {BindingSource::Keyboard, static_cast<uint32_t>(Key::RightShift), false},
          BindingSlot::Gamepad);
    b.set(InputAction::PrevWeapon, {BindingSource::Keyboard, static_cast<uint32_t>(Key::RightCtrl), false},
          BindingSlot::Gamepad);
    b.set(InputAction::ViewUp, {BindingSource::Keyboard, static_cast<uint32_t>(Key::LeftAlt), false},
          BindingSlot::Gamepad);
    // MouseButton Left and Right (also exercises mousButtonName / mousButtonFromName)
    b.set(InputAction::ViewLeft, {BindingSource::MouseButton, static_cast<uint32_t>(MouseButton::Left), false},
          BindingSlot::Gamepad);
    b.set(InputAction::ViewRight, {BindingSource::MouseButton, static_cast<uint32_t>(MouseButton::Right), false},
          BindingSlot::Gamepad);

    InputBindings b2;
    for (int i = 0; i < InputBindings::kActionCount; ++i) {
        b2.clear(static_cast<InputAction>(i));
        b2.clear(static_cast<InputAction>(i), BindingSlot::Gamepad);
    }
    REQUIRE(b2.deserialize(b.serialize()));

    // Spot-check a sample of the round-tripped bindings
    CHECK(b2.get(InputAction::PitchAxis).id == static_cast<uint32_t>(Key::B));
    CHECK(b2.get(InputAction::ThrottleUp).id == static_cast<uint32_t>(Key::P));
    CHECK(b2.get(InputAction::CommsMenu).id == static_cast<uint32_t>(Key::Num9));
    CHECK(b2.get(InputAction::PitchAxis, BindingSlot::Gamepad).id == static_cast<uint32_t>(Key::Delete));
    CHECK(b2.get(InputAction::FireStore, BindingSlot::Gamepad).id == static_cast<uint32_t>(Key::F11));
    CHECK(b2.get(InputAction::ViewLeft, BindingSlot::Gamepad).source == BindingSource::MouseButton);
    CHECK(b2.get(InputAction::ViewLeft, BindingSlot::Gamepad).id == static_cast<uint32_t>(MouseButton::Left));
    CHECK(b2.get(InputAction::ViewRight, BindingSlot::Gamepad).id == static_cast<uint32_t>(MouseButton::Right));
}

TEST_CASE("InputBindings conflictsWith detects alt-slot conflict", "[bindings]") {
    InputBindings b;
    b.applyDefaults();
    // PitchAxis alt is RightY gamepad axis — try to put same binding in RollAxis alt
    Binding altPitch = b.get(InputAction::PitchAxis, BindingSlot::Gamepad);
    auto conflict = b.conflictsWith(InputAction::RollAxis, altPitch);
    REQUIRE(conflict.has_value());
    CHECK(*conflict == InputAction::PitchAxis);
}

TEST_CASE("InputBindings parseBinding accepts empty source as None", "[bindings]") {
    // A TOML entry with source = "" should deserialize to None binding
    InputBindings b;
    b.applyDefaults();
    // Use raw TOML with missing source field to hit the source.empty() branch
    REQUIRE(b.deserialize("[primary]\nPitchUp = { source = \"\", id = \"\" }\n"));
    CHECK(b.get(InputAction::PitchUp).isNone());
}

// ---------------------------------------------------------------------------
// AxisConfigTable — additional branch coverage
// ---------------------------------------------------------------------------

TEST_CASE("AxisConfigTable deserialize with invalid TOML returns false", "[axis_config]") {
    AxisConfigTable t;
    REQUIRE_FALSE(t.deserialize("this is {{{ totally invalid"));
}

TEST_CASE("AxisConfigTable deserialize with absent axis_config section returns true (keeps defaults)",
          "[axis_config]") {
    AxisConfigTable t;
    // No [axis_config] section → sec==nullptr → if(!sec) return true branch
    REQUIRE(t.deserialize("[other]\nkey = \"value\"\n"));
    // Defaults unchanged
    CHECK(t.get(GamepadAxis::LeftX).deadzone == Catch::Approx(0.1f));
}

TEST_CASE("AxisConfigTable deserialize with partial axis entry (missing optional keys)", "[axis_config]") {
    AxisConfigTable t;
    // Only deadzone present — invert/scale/curve absent → if(auto v = ...) FALSE branches
    REQUIRE(t.deserialize("[axis_config]\nLeftX = { deadzone = 0.12 }\n"));
    CHECK(t.get(GamepadAxis::LeftX).deadzone == Catch::Approx(0.12f));
    CHECK(t.get(GamepadAxis::LeftX).invert == false); // default preserved
}

TEST_CASE("AxisConfigTable serialize and deserialize roundtrip for Linear curve", "[axis_config]") {
    AxisConfigTable t;
    t.get(GamepadAxis::RightX).curve = AxisCurve::Linear;
    t.get(GamepadAxis::RightX).invert = true;
    std::string toml = t.serialize();
    AxisConfigTable t2;
    REQUIRE(t2.deserialize(toml));
    CHECK(t2.get(GamepadAxis::RightX).curve == AxisCurve::Linear);
    CHECK(t2.get(GamepadAxis::RightX).invert == true);
}

TEST_CASE("AxisConfigTable deserialize with axis name not in section continues", "[axis_config]") {
    AxisConfigTable t;
    // Section exists but only LeftX is present; other axes not in section → if(!entry) continue
    REQUIRE(t.deserialize("[axis_config]\nLeftX = { deadzone = 0.20 }\n"));
    CHECK(t.get(GamepadAxis::LeftX).deadzone == Catch::Approx(0.20f));
    CHECK(t.get(GamepadAxis::RightX).deadzone == Catch::Approx(0.1f)); // default
}

// ---------------------------------------------------------------------------
// Camera/view-family action defaults + BindingQuery (#689)
// ---------------------------------------------------------------------------

TEST_CASE("InputBindings: camera + view-family actions have their #689 defaults", "[bindings]") {
    InputBindings b;
    CHECK(b.get(InputAction::CameraCockpit).source == BindingSource::Keyboard);
    CHECK(b.get(InputAction::CameraCockpit).id == static_cast<uint32_t>(Key::F1));
    CHECK(b.get(InputAction::CameraChase).id == static_cast<uint32_t>(Key::F2));
    CHECK(b.get(InputAction::CameraFree).id == static_cast<uint32_t>(Key::F4));
    CHECK(b.get(InputAction::PadlockToggle).id == static_cast<uint32_t>(Key::F5));
    CHECK(b.get(InputAction::TargetInsetToggle).id == static_cast<uint32_t>(Key::F6));
    CHECK(b.get(InputAction::NextTarget).id == static_cast<uint32_t>(Key::N));
    CHECK(b.get(InputAction::PrevTarget).id == static_cast<uint32_t>(Key::P));
    // Later epic-587 actions keep their reserved defaults.
    CHECK(b.get(InputAction::MasterArm).id == static_cast<uint32_t>(Key::Num4));
    CHECK(b.get(InputAction::MfdPage).id == static_cast<uint32_t>(Key::O));
    CHECK(b.get(InputAction::MfdRange).id == static_cast<uint32_t>(Key::Num3));
    CHECK(b.get(InputAction::NvgToggle).id == static_cast<uint32_t>(Key::F7));
    CHECK(b.get(InputAction::AutopilotAltHold).id == static_cast<uint32_t>(Key::F9));
    // The cockpit pan is the keypad cross (#1050): the arrows fly the aircraft, and a key that both
    // rolled and panned was two actions on one input, not a feature.
    CHECK(b.get(InputAction::ViewLeft).id == static_cast<uint32_t>(Key::Numpad4));
    CHECK(b.get(InputAction::ViewRight).id == static_cast<uint32_t>(Key::Numpad6));
    CHECK(b.get(InputAction::ViewUp).id == static_cast<uint32_t>(Key::Numpad8));
    CHECK(b.get(InputAction::ViewDown).id == static_cast<uint32_t>(Key::Numpad2));
    // Gamepad alt: padlock on the right stick click, next-target on the d-pad up.
    CHECK(b.get(InputAction::PadlockToggle, BindingSlot::Gamepad).source == BindingSource::GamepadButton);
    CHECK(b.get(InputAction::PadlockToggle, BindingSlot::Gamepad).id ==
          static_cast<uint32_t>(GamepadButton::RightStick));
    CHECK(b.get(InputAction::NextTarget, BindingSlot::Gamepad).id == static_cast<uint32_t>(GamepadButton::DpadUp));
    // The target-cycle defaults deliberately avoid T (comms) and Y (ChatAll).
    CHECK(b.get(InputAction::NextTarget).id != static_cast<uint32_t>(Key::T));
    CHECK(b.get(InputAction::PrevTarget).id != static_cast<uint32_t>(Key::Y));
}

TEST_CASE("InputBindings: Minus/Equals now serialize round-trip (#689)", "[bindings]") {
    InputBindings b;
    b.set(InputAction::ViewUp, {BindingSource::Keyboard, static_cast<uint32_t>(Key::Minus), false});
    b.set(InputAction::ViewDown, {BindingSource::Keyboard, static_cast<uint32_t>(Key::Equals), false});
    InputBindings b2;
    REQUIRE(b2.deserialize(b.serialize()));
    CHECK(b2.get(InputAction::ViewUp).id == static_cast<uint32_t>(Key::Minus));
    CHECK(b2.get(InputAction::ViewDown).id == static_cast<uint32_t>(Key::Equals));
}

TEST_CASE("bindingJustPressed/bindingDown resolve every Binding source (#689)", "[bindings]") {
    MockInput in;

    // Keyboard: F5 padlock toggle (edge) — just-pressed only, not held.
    const Binding kb{BindingSource::Keyboard, static_cast<uint32_t>(Key::F5), false};
    CHECK_FALSE(bindingJustPressed(in, kb));
    in.justPressed.insert(Key::F5);
    in.held.insert(Key::F5);
    CHECK(bindingJustPressed(in, kb));
    CHECK(bindingDown(in, kb));

    // Gamepad button.
    const Binding gb{BindingSource::GamepadButton, static_cast<uint32_t>(GamepadButton::RightStick), false};
    CHECK_FALSE(bindingDown(in, gb));
    in.gpDown.insert({0, GamepadButton::RightStick});
    in.gpJustPressed.insert({0, GamepadButton::RightStick});
    CHECK(bindingDown(in, gb));
    CHECK(bindingJustPressed(in, gb));

    // Gamepad axis: Down past the ±0.5 threshold; no rising edge.
    const Binding axPos{BindingSource::GamepadAxis, static_cast<uint32_t>(GamepadAxis::RightY), false};
    in.axisValues[{0, GamepadAxis::RightY}] = 0.7f;
    CHECK(bindingDown(in, axPos));
    CHECK_FALSE(bindingJustPressed(in, axPos)); // analog axes have no discrete edge
    in.axisValues[{0, GamepadAxis::RightY}] = 0.3f;
    CHECK_FALSE(bindingDown(in, axPos)); // below threshold
    // Negative-direction binding on the same axis.
    const Binding axNeg{BindingSource::GamepadAxis, static_cast<uint32_t>(GamepadAxis::RightY), true};
    in.axisValues[{0, GamepadAxis::RightY}] = -0.7f;
    CHECK(bindingDown(in, axNeg));

    // A None binding is always inert.
    CHECK_FALSE(bindingDown(in, Binding{}));
    CHECK_FALSE(bindingJustPressed(in, Binding{}));
}
