// SPDX-License-Identifier: GPL-3.0-or-later
#include "input/AxisConfig.h"
#include "input/InputBindings.h"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

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
    // Keyboard primary: PitchUp bound to S
    Binding pitch = b.get(InputAction::PitchUp);
    CHECK(pitch.source == BindingSource::Keyboard);
    CHECK(pitch.id == static_cast<uint32_t>(Key::S));

    // Gamepad alt: PitchAxis bound to RightY
    Binding pitchAxis = b.get(InputAction::PitchAxis, true);
    CHECK(pitchAxis.source == BindingSource::GamepadAxis);
    CHECK(pitchAxis.id == static_cast<uint32_t>(GamepadAxis::RightY));
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

    // Assign the same key that PitchUp already has (S) to RollLeft
    Binding s{BindingSource::Keyboard, static_cast<uint32_t>(Key::S), false};
    auto conflict = b.conflictsWith(InputAction::RollLeft, s);
    REQUIRE(conflict.has_value());
    CHECK(*conflict == InputAction::PitchUp);
}

TEST_CASE("InputBindings no conflict after clearing the conflicting action", "[bindings]") {
    InputBindings b;
    b.applyDefaults();
    b.clear(InputAction::PitchUp);

    Binding s{BindingSource::Keyboard, static_cast<uint32_t>(Key::S), false};
    CHECK_FALSE(b.conflictsWith(InputAction::RollLeft, s).has_value());
}

TEST_CASE("InputBindings no conflict for None binding", "[bindings]") {
    InputBindings b;
    CHECK_FALSE(b.conflictsWith(InputAction::RollLeft, Binding{}).has_value());
}

TEST_CASE("InputBindings does not conflict with its own action", "[bindings]") {
    InputBindings b;
    // PitchUp is bound to S; asking conflictsWith(PitchUp, S) should return nullopt
    Binding s{BindingSource::Keyboard, static_cast<uint32_t>(Key::S), false};
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
        loaded.clear(static_cast<InputAction>(i), true);
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

        Binding aAlt = original.get(action, true);
        Binding bAlt = loaded.get(action, true);
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
        b.clear(static_cast<InputAction>(i), true);
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
        b2.clear(static_cast<InputAction>(i), true);
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
    b.set(InputAction::FireMissile, {BindingSource::MouseButton, static_cast<uint32_t>(MouseButton::Middle), false});

    InputBindings b2;
    REQUIRE(b2.deserialize(b.serialize()));
    CHECK(b2.get(InputAction::FireMissile).source == BindingSource::MouseButton);
    CHECK(b2.get(InputAction::FireMissile).id == static_cast<uint32_t>(MouseButton::Middle));
}

TEST_CASE("InputBindings roundtrips keyboard keys not in default bindings", "[bindings]") {
    InputBindings b;
    for (int i = 0; i < InputBindings::kActionCount; ++i) {
        b.clear(static_cast<InputAction>(i));
        b.clear(static_cast<InputAction>(i), true);
    }
    b.set(InputAction::ViewUp, {BindingSource::Keyboard, static_cast<uint32_t>(Key::F1), false});
    b.set(InputAction::ViewDown, {BindingSource::Keyboard, static_cast<uint32_t>(Key::F12), false});
    b.set(InputAction::ViewLeft, {BindingSource::Keyboard, static_cast<uint32_t>(Key::LeftCtrl), false});
    b.set(InputAction::ViewRight, {BindingSource::Keyboard, static_cast<uint32_t>(Key::RightAlt), false});
    b.set(InputAction::LandingGear, {BindingSource::Keyboard, static_cast<uint32_t>(Key::Enter), false});
    b.set(InputAction::Flaps, {BindingSource::Keyboard, static_cast<uint32_t>(Key::Backspace), false});
    b.set(InputAction::Pause, {BindingSource::Keyboard, static_cast<uint32_t>(Key::Insert), false});
    b.set(InputAction::Menu, {BindingSource::Keyboard, static_cast<uint32_t>(Key::PageUp), false});

    InputBindings b2;
    for (int i = 0; i < InputBindings::kActionCount; ++i) {
        b2.clear(static_cast<InputAction>(i));
        b2.clear(static_cast<InputAction>(i), true);
    }
    REQUIRE(b2.deserialize(b.serialize()));
    CHECK(b2.get(InputAction::ViewUp).id == static_cast<uint32_t>(Key::F1));
    CHECK(b2.get(InputAction::ViewDown).id == static_cast<uint32_t>(Key::F12));
    CHECK(b2.get(InputAction::ViewLeft).id == static_cast<uint32_t>(Key::LeftCtrl));
    CHECK(b2.get(InputAction::ViewRight).id == static_cast<uint32_t>(Key::RightAlt));
    CHECK(b2.get(InputAction::LandingGear).id == static_cast<uint32_t>(Key::Enter));
    CHECK(b2.get(InputAction::Flaps).id == static_cast<uint32_t>(Key::Backspace));
    CHECK(b2.get(InputAction::Pause).id == static_cast<uint32_t>(Key::Insert));
    CHECK(b2.get(InputAction::Menu).id == static_cast<uint32_t>(Key::PageUp));
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
        b.clear(static_cast<InputAction>(i), true);
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
    b.set(InputAction::FireMissile, {BindingSource::Keyboard, static_cast<uint32_t>(Key::X), false});
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
    b.set(InputAction::Menu, {BindingSource::Keyboard, static_cast<uint32_t>(Key::Num9), false});

    // Alt: uncovered navigation / F-key / modifier keys
    b.set(InputAction::PitchAxis, {BindingSource::Keyboard, static_cast<uint32_t>(Key::Delete), false}, true);
    b.set(InputAction::RollAxis, {BindingSource::Keyboard, static_cast<uint32_t>(Key::ArrowLeft), false}, true);
    b.set(InputAction::YawAxis, {BindingSource::Keyboard, static_cast<uint32_t>(Key::ArrowRight), false}, true);
    b.set(InputAction::ThrottleAxis, {BindingSource::Keyboard, static_cast<uint32_t>(Key::Home), false}, true);
    b.set(InputAction::PitchUp, {BindingSource::Keyboard, static_cast<uint32_t>(Key::End), false}, true);
    b.set(InputAction::PitchDown, {BindingSource::Keyboard, static_cast<uint32_t>(Key::PageDown), false}, true);
    b.set(InputAction::RollLeft, {BindingSource::Keyboard, static_cast<uint32_t>(Key::F2), false}, true);
    b.set(InputAction::RollRight, {BindingSource::Keyboard, static_cast<uint32_t>(Key::F3), false}, true);
    b.set(InputAction::YawLeft, {BindingSource::Keyboard, static_cast<uint32_t>(Key::F4), false}, true);
    b.set(InputAction::YawRight, {BindingSource::Keyboard, static_cast<uint32_t>(Key::F5), false}, true);
    b.set(InputAction::ThrottleUp, {BindingSource::Keyboard, static_cast<uint32_t>(Key::F6), false}, true);
    b.set(InputAction::ThrottleDown, {BindingSource::Keyboard, static_cast<uint32_t>(Key::F7), false}, true);
    b.set(InputAction::Airbrake, {BindingSource::Keyboard, static_cast<uint32_t>(Key::F8), false}, true);
    b.set(InputAction::Afterburner, {BindingSource::Keyboard, static_cast<uint32_t>(Key::F9), false}, true);
    b.set(InputAction::FireWeapon, {BindingSource::Keyboard, static_cast<uint32_t>(Key::F10), false}, true);
    b.set(InputAction::FireMissile, {BindingSource::Keyboard, static_cast<uint32_t>(Key::F11), false}, true);
    b.set(InputAction::NextWeapon, {BindingSource::Keyboard, static_cast<uint32_t>(Key::RightShift), false}, true);
    b.set(InputAction::PrevWeapon, {BindingSource::Keyboard, static_cast<uint32_t>(Key::RightCtrl), false}, true);
    b.set(InputAction::ViewUp, {BindingSource::Keyboard, static_cast<uint32_t>(Key::LeftAlt), false}, true);
    // MouseButton Left and Right (also exercises mousButtonName / mousButtonFromName)
    b.set(InputAction::ViewLeft, {BindingSource::MouseButton, static_cast<uint32_t>(MouseButton::Left), false}, true);
    b.set(InputAction::ViewRight, {BindingSource::MouseButton, static_cast<uint32_t>(MouseButton::Right), false}, true);

    InputBindings b2;
    for (int i = 0; i < InputBindings::kActionCount; ++i) {
        b2.clear(static_cast<InputAction>(i));
        b2.clear(static_cast<InputAction>(i), true);
    }
    REQUIRE(b2.deserialize(b.serialize()));

    // Spot-check a sample of the round-tripped bindings
    CHECK(b2.get(InputAction::PitchAxis).id == static_cast<uint32_t>(Key::B));
    CHECK(b2.get(InputAction::ThrottleUp).id == static_cast<uint32_t>(Key::P));
    CHECK(b2.get(InputAction::Menu).id == static_cast<uint32_t>(Key::Num9));
    CHECK(b2.get(InputAction::PitchAxis, true).id == static_cast<uint32_t>(Key::Delete));
    CHECK(b2.get(InputAction::FireMissile, true).id == static_cast<uint32_t>(Key::F11));
    CHECK(b2.get(InputAction::ViewLeft, true).source == BindingSource::MouseButton);
    CHECK(b2.get(InputAction::ViewLeft, true).id == static_cast<uint32_t>(MouseButton::Left));
    CHECK(b2.get(InputAction::ViewRight, true).id == static_cast<uint32_t>(MouseButton::Right));
}

TEST_CASE("InputBindings conflictsWith detects alt-slot conflict", "[bindings]") {
    InputBindings b;
    b.applyDefaults();
    // PitchAxis alt is RightY gamepad axis — try to put same binding in RollAxis alt
    Binding altPitch = b.get(InputAction::PitchAxis, true);
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
