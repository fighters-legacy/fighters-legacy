// SPDX-License-Identifier: GPL-3.0-or-later
#include "IClock.h"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "CameraInput.h"
#include "FlightInputCollector.h"
#include "config/ControlsSettings.h"
#include "console/CommandRegistry.h"
#include "console/GameConsole.h"
#include "input/AxisConfig.h"
#include "input/InputBindings.h"
#include "mock_hal.h"
#include "render/RenderSnapshot.h"
#include "render/SimRenderBridge.h"

#include <chrono>

// ---------------------------------------------------------------------------
// Rate limiter + seqNum
// ---------------------------------------------------------------------------

TEST_CASE("FlightInputCollector first poll always returns value", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    auto r = fic.poll(bridge, cam, console, inp, nullptr, {});
    REQUIRE(r.has_value());
}

TEST_CASE("FlightInputCollector second poll at same clock time returns nullopt", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    fic.poll(bridge, cam, console, inp, nullptr, {});
    auto r2 = fic.poll(bridge, cam, console, inp, nullptr, {});
    CHECK_FALSE(r2.has_value());
}

TEST_CASE("FlightInputCollector wasWeaponFired resets on rate-limited poll", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    inp.held.insert(Key::Space);
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    fic.poll(bridge, cam, console, inp, nullptr, {});
    CHECK(fic.wasWeaponFired());

    // Same tick — nullopt returned, but weaponFired still resets.
    fic.poll(bridge, cam, console, inp, nullptr, {});
    CHECK_FALSE(fic.wasWeaponFired());
}

TEST_CASE("FlightInputCollector advancing clock past gate returns value", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    fic.poll(bridge, cam, console, inp, nullptr, {});
    t.advance(std::chrono::milliseconds(17));
    auto r = fic.poll(bridge, cam, console, inp, nullptr, {});
    REQUIRE(r.has_value());
}

TEST_CASE("FlightInputCollector seqNum increments across polls", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    auto r0 = fic.poll(bridge, cam, console, inp, nullptr, {});
    REQUIRE(r0.has_value());
    CHECK(r0->seqNum == 0u);

    t.advance(std::chrono::milliseconds(17));
    auto r1 = fic.poll(bridge, cam, console, inp, nullptr, {});
    REQUIRE(r1.has_value());
    CHECK(r1->seqNum == 1u);

    t.advance(std::chrono::milliseconds(17));
    auto r2 = fic.poll(bridge, cam, console, inp, nullptr, {});
    REQUIRE(r2.has_value());
    CHECK(r2->seqNum == 2u);
}

// ---------------------------------------------------------------------------
// tickIndex branch
// ---------------------------------------------------------------------------

TEST_CASE("FlightInputCollector tickIndex is 0 when bridge has no snapshot", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    auto r = fic.poll(bridge, cam, console, inp, nullptr, {});
    REQUIRE(r.has_value());
    CHECK(r->tickIndex == 0u);
}

TEST_CASE("FlightInputCollector tickIndex taken from bridge snapshot", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    CameraInput cam;
    fl::SimRenderBridge bridge;
    fl::RenderSnapshot snap;
    snap.tickIndex = 42u;
    bridge.publishExternal(std::move(snap));
    bridge.tryAdvance();

    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    auto r = fic.poll(bridge, cam, console, inp, nullptr, {});
    REQUIRE(r.has_value());
    CHECK(r->tickIndex == 42u);
}

// ---------------------------------------------------------------------------
// Keyboard path
// ---------------------------------------------------------------------------

TEST_CASE("FlightInputCollector Space sets fire bit and wasWeaponFired", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    inp.held.insert(Key::Space);
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    auto r = fic.poll(bridge, cam, console, inp, nullptr, {});
    REQUIRE(r.has_value());
    CHECK((r->buttons & 1u) != 0u);
    CHECK(fic.wasWeaponFired());
}

TEST_CASE("FlightInputCollector Tab sets afterburner bit", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    inp.held.insert(Key::Tab);
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    auto r = fic.poll(bridge, cam, console, inp, nullptr, {});
    REQUIRE(r.has_value());
    CHECK((r->buttons & 2u) != 0u);
    CHECK_FALSE(fic.wasWeaponFired());
}

TEST_CASE("FlightInputCollector ArrowUp gives negative elevator", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    inp.held.insert(Key::ArrowUp);
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    auto r = fic.poll(bridge, cam, console, inp, nullptr, {});
    REQUIRE(r.has_value());
    CHECK(r->elevator == Catch::Approx(-1.f));
}

TEST_CASE("FlightInputCollector ArrowDown gives positive elevator", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    inp.held.insert(Key::ArrowDown);
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    auto r = fic.poll(bridge, cam, console, inp, nullptr, {});
    REQUIRE(r.has_value());
    CHECK(r->elevator == Catch::Approx(1.f));
}

TEST_CASE("FlightInputCollector ArrowLeft gives negative aileron", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    inp.held.insert(Key::ArrowLeft);
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    auto r = fic.poll(bridge, cam, console, inp, nullptr, {});
    REQUIRE(r.has_value());
    CHECK(r->aileron == Catch::Approx(-1.f));
}

TEST_CASE("FlightInputCollector ArrowRight gives positive aileron", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    inp.held.insert(Key::ArrowRight);
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    auto r = fic.poll(bridge, cam, console, inp, nullptr, {});
    REQUIRE(r.has_value());
    CHECK(r->aileron == Catch::Approx(1.f));
}

TEST_CASE("FlightInputCollector Key Z gives negative rudder", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    inp.held.insert(Key::Z);
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    auto r = fic.poll(bridge, cam, console, inp, nullptr, {});
    REQUIRE(r.has_value());
    CHECK(r->rudder == Catch::Approx(-1.f));
}

TEST_CASE("FlightInputCollector Key X gives positive rudder", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    inp.held.insert(Key::X);
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    auto r = fic.poll(bridge, cam, console, inp, nullptr, {});
    REQUIRE(r.has_value());
    CHECK(r->rudder == Catch::Approx(1.f));
}

TEST_CASE("FlightInputCollector LeftShift sets throttle to 1", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    inp.held.insert(Key::LeftShift);
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    auto r = fic.poll(bridge, cam, console, inp, nullptr, {});
    REQUIRE(r.has_value());
    CHECK(r->throttle == Catch::Approx(1.f));
}

TEST_CASE("FlightInputCollector PageUp increases throttle via camInput", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    inp.held.insert(Key::PageUp);
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    const float before = cam.throttle();
    fic.poll(bridge, cam, console, inp, nullptr, {});
    CHECK(cam.throttle() > before);
}

TEST_CASE("FlightInputCollector PageDown decreases throttle via camInput", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    inp.held.insert(Key::PageDown);
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    // Start at half-throttle so there is room to decrease.
    cam.setThrottle(0.5f);
    fic.poll(bridge, cam, console, inp, nullptr, {});
    CHECK(cam.throttle() < 0.5f);
}

TEST_CASE("FlightInputCollector opposing Up+Down cancel to zero elevator", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    inp.held.insert(Key::ArrowUp);
    inp.held.insert(Key::ArrowDown);
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    auto r = fic.poll(bridge, cam, console, inp, nullptr, {});
    REQUIRE(r.has_value());
    CHECK(r->elevator == Catch::Approx(0.f));
}

TEST_CASE("FlightInputCollector opposing Left+Right cancel to zero aileron", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    inp.held.insert(Key::ArrowLeft);
    inp.held.insert(Key::ArrowRight);
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    auto r = fic.poll(bridge, cam, console, inp, nullptr, {});
    REQUIRE(r.has_value());
    CHECK(r->aileron == Catch::Approx(0.f));
}

TEST_CASE("FlightInputCollector opposing Z+X cancel to zero rudder", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    inp.held.insert(Key::Z);
    inp.held.insert(Key::X);
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    auto r = fic.poll(bridge, cam, console, inp, nullptr, {});
    REQUIRE(r.has_value());
    CHECK(r->rudder == Catch::Approx(0.f));
}

// ---------------------------------------------------------------------------
// Console open suppression
// ---------------------------------------------------------------------------

TEST_CASE("FlightInputCollector console open suppresses keyboard input", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    console.openHeadless();
    MockInput inp;
    inp.held.insert(Key::Space);
    inp.held.insert(Key::ArrowUp);
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    auto r = fic.poll(bridge, cam, console, inp, nullptr, {});
    REQUIRE(r.has_value());
    CHECK(r->buttons == 0u);
    CHECK(r->elevator == Catch::Approx(0.f));
    CHECK_FALSE(fic.wasWeaponFired());
}

TEST_CASE("FlightInputCollector console open suppresses gamepad input", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    console.openHeadless();
    MockInput inp;
    inp.gamepadCount = 1;
    ControlsSettings cs;
    inp.gpDown.insert({0, static_cast<GamepadButton>(cs.fireButton)});
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    auto r = fic.poll(bridge, cam, console, inp, nullptr, cs);
    REQUIRE(r.has_value());
    CHECK(r->buttons == 0u);
}

TEST_CASE("FlightInputCollector console open still reads throttle from camInput", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    console.openHeadless();
    MockInput inp;
    CameraInput cam;
    cam.setThrottle(0.7f);
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    auto r = fic.poll(bridge, cam, console, inp, nullptr, {});
    REQUIRE(r.has_value());
    CHECK(r->throttle == Catch::Approx(0.7f));
}

// ---------------------------------------------------------------------------
// Gamepad path
// ---------------------------------------------------------------------------

TEST_CASE("FlightInputCollector gamepad fireButton sets bit 0 and wasWeaponFired", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    inp.gamepadCount = 1;
    ControlsSettings cs;
    inp.gpDown.insert({0, static_cast<GamepadButton>(cs.fireButton)});
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    auto r = fic.poll(bridge, cam, console, inp, nullptr, cs);
    REQUIRE(r.has_value());
    CHECK((r->buttons & 1u) != 0u);
    CHECK(fic.wasWeaponFired());
}

TEST_CASE("FlightInputCollector gamepad afterburnerButton sets bit 1", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    inp.gamepadCount = 1;
    ControlsSettings cs;
    inp.gpDown.insert({0, static_cast<GamepadButton>(cs.afterburnerButton)});
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    auto r = fic.poll(bridge, cam, console, inp, nullptr, cs);
    REQUIRE(r.has_value());
    CHECK((r->buttons & 2u) != 0u);
    CHECK_FALSE(fic.wasWeaponFired());
}

TEST_CASE("FlightInputCollector gamepad TriggerLeft above deadzone sets throttle", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    inp.gamepadCount = 1;
    inp.axisValues[{0, GamepadAxis::TriggerLeft}] = 0.5f;
    ControlsSettings cs;
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    auto r = fic.poll(bridge, cam, console, inp, nullptr, cs);
    REQUIRE(r.has_value());
    CHECK(r->throttle > 0.f);
}

TEST_CASE("FlightInputCollector gamepad TriggerLeft at deadzone does not override throttle", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    inp.gamepadCount = 1;
    fl::AxisConfigTable axes;
    const float dz = axes.get(GamepadAxis::TriggerLeft).deadzone; // 0.1f default
    inp.axisValues[{0, GamepadAxis::TriggerLeft}] = dz;
    CameraInput cam;
    cam.setThrottle(0.3f);
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);
    fic.setAxisConfig(axes);

    auto r = fic.poll(bridge, cam, console, inp, nullptr, {});
    REQUIRE(r.has_value());
    // Trigger at deadzone → apply() returns 0.0f → keyboard throttle (camInput) holds.
    CHECK(r->throttle == Catch::Approx(0.3f));
}

TEST_CASE("FlightInputCollector gamepad RightY above deadzone overrides elevator", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    inp.gamepadCount = 1;
    inp.axisValues[{0, GamepadAxis::RightY}] = 0.5f;
    ControlsSettings cs;
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    auto r = fic.poll(bridge, cam, console, inp, nullptr, cs);
    REQUIRE(r.has_value());
    CHECK(r->elevator != Catch::Approx(0.f));
}

TEST_CASE("FlightInputCollector gamepad RightX above deadzone overrides aileron", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    inp.gamepadCount = 1;
    inp.axisValues[{0, GamepadAxis::RightX}] = 0.5f;
    ControlsSettings cs;
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    auto r = fic.poll(bridge, cam, console, inp, nullptr, cs);
    REQUIRE(r.has_value());
    CHECK(r->aileron != Catch::Approx(0.f));
}

TEST_CASE("FlightInputCollector gamepad LeftX above deadzone overrides rudder", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    inp.gamepadCount = 1;
    inp.axisValues[{0, GamepadAxis::LeftX}] = 0.5f;
    ControlsSettings cs;
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    auto r = fic.poll(bridge, cam, console, inp, nullptr, cs);
    REQUIRE(r.has_value());
    CHECK(r->rudder != Catch::Approx(0.f));
}

TEST_CASE("FlightInputCollector gamepad axis below deadzone leaves keyboard value", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    inp.gamepadCount = 1;
    inp.held.insert(Key::ArrowUp);
    ControlsSettings cs;
    // Below deadzone — should not override.
    inp.axisValues[{0, GamepadAxis::RightY}] = 0.01f;
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    auto r = fic.poll(bridge, cam, console, inp, nullptr, cs);
    REQUIRE(r.has_value());
    CHECK(r->elevator == Catch::Approx(-1.f));
}

TEST_CASE("FlightInputCollector gamepad RightY invert flips elevator sign", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    inp.gamepadCount = 1;
    inp.axisValues[{0, GamepadAxis::RightY}] = 0.5f;
    CameraInput cam;
    fl::SimRenderBridge bridge;

    FlightInputCollector fic_normal;
    fl::ManualClock t;
    fic_normal.setClock(t);
    auto r_normal = fic_normal.poll(bridge, cam, console, inp, nullptr, {});
    REQUIRE(r_normal.has_value());

    fl::AxisConfigTable axisInv;
    axisInv.get(GamepadAxis::RightY).invert = true;
    FlightInputCollector fic_inv;
    fic_inv.setClock(t);
    fic_inv.setAxisConfig(axisInv);
    auto r_inv = fic_inv.poll(bridge, cam, console, inp, nullptr, {});
    REQUIRE(r_inv.has_value());

    CHECK(r_normal->elevator * r_inv->elevator < 0.f);
}

// ---------------------------------------------------------------------------
// Combined keyboard + gamepad OR paths
// ---------------------------------------------------------------------------

TEST_CASE("FlightInputCollector keyboard Space and gamepad afterburner set both bits", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    inp.gamepadCount = 1;
    inp.held.insert(Key::Space);
    ControlsSettings cs;
    inp.gpDown.insert({0, static_cast<GamepadButton>(cs.afterburnerButton)});
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    auto r = fic.poll(bridge, cam, console, inp, nullptr, cs);
    REQUIRE(r.has_value());
    CHECK((r->buttons & 3u) == 3u);
}

TEST_CASE("FlightInputCollector keyboard Tab and gamepad fireButton set both bits", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    inp.gamepadCount = 1;
    inp.held.insert(Key::Tab);
    ControlsSettings cs;
    inp.gpDown.insert({0, static_cast<GamepadButton>(cs.fireButton)});
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    auto r = fic.poll(bridge, cam, console, inp, nullptr, cs);
    REQUIRE(r.has_value());
    CHECK((r->buttons & 3u) == 3u);
}

// ---------------------------------------------------------------------------
// HOTAS path
// ---------------------------------------------------------------------------

TEST_CASE("FlightInputCollector HOTAS throttle axis sets absolute throttle", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    MockJoystick joy;
    joy.count = 1;
    joy.axisCount = 4;
    ControlsSettings cs;
    // hotasThrottleAxis default = 2; raw 0.5 → (0.5+1)/2 = 0.75.
    joy.axisValues[{0, cs.hotasThrottleAxis}] = 0.5f;
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    auto r = fic.poll(bridge, cam, console, inp, &joy, cs);
    REQUIRE(r.has_value());
    CHECK(r->throttle == Catch::Approx(0.75f));
}

TEST_CASE("FlightInputCollector HOTAS elevator axis overrides keyboard", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    inp.held.insert(Key::ArrowUp);
    MockJoystick joy;
    joy.count = 1;
    joy.axisCount = 4;
    ControlsSettings cs;
    joy.axisValues[{0, cs.hotasElevatorAxis}] = 0.6f;
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    auto r = fic.poll(bridge, cam, console, inp, &joy, cs);
    REQUIRE(r.has_value());
    // HOTAS axis 0.6 above deadzone 0.05: overrides ArrowUp (-1).
    CHECK(r->elevator != Catch::Approx(-1.f));
    CHECK(r->elevator > 0.f);
}

TEST_CASE("FlightInputCollector HOTAS aileron axis overrides keyboard", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    inp.held.insert(Key::ArrowLeft);
    MockJoystick joy;
    joy.count = 1;
    joy.axisCount = 4;
    ControlsSettings cs;
    joy.axisValues[{0, cs.hotasAileronAxis}] = 0.6f;
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    auto r = fic.poll(bridge, cam, console, inp, &joy, cs);
    REQUIRE(r.has_value());
    CHECK(r->aileron != Catch::Approx(-1.f));
    CHECK(r->aileron > 0.f);
}

TEST_CASE("FlightInputCollector HOTAS rudder axis overrides keyboard", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    inp.held.insert(Key::Z);
    MockJoystick joy;
    joy.count = 1;
    joy.axisCount = 4;
    ControlsSettings cs;
    joy.axisValues[{0, cs.hotasRudderAxis}] = 0.6f;
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    auto r = fic.poll(bridge, cam, console, inp, &joy, cs);
    REQUIRE(r.has_value());
    CHECK(r->rudder != Catch::Approx(-1.f));
    CHECK(r->rudder > 0.f);
}

TEST_CASE("FlightInputCollector HOTAS axis at deadzone does not override keyboard", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    inp.held.insert(Key::ArrowUp);
    MockJoystick joy;
    joy.count = 1;
    joy.axisCount = 4;
    ControlsSettings cs;
    // Exactly at deadzone — applyHotas returns 0, so keyboard wins.
    joy.axisValues[{0, cs.hotasElevatorAxis}] = cs.hotasDeadzone;
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    auto r = fic.poll(bridge, cam, console, inp, &joy, cs);
    REQUIRE(r.has_value());
    CHECK(r->elevator == Catch::Approx(-1.f));
}

TEST_CASE("FlightInputCollector HOTAS hotasInvertPitch flips elevator sign", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    MockJoystick joy;
    joy.count = 1;
    joy.axisCount = 4;
    ControlsSettings cs_normal;
    cs_normal.hotasElevatorAxis = 1;
    joy.axisValues[{0, 1}] = 0.6f;
    CameraInput cam;
    fl::SimRenderBridge bridge;

    FlightInputCollector fic_n;
    fl::ManualClock t;
    fic_n.setClock(t);
    auto r_n = fic_n.poll(bridge, cam, console, inp, &joy, cs_normal);
    REQUIRE(r_n.has_value());

    ControlsSettings cs_inv = cs_normal;
    cs_inv.hotasInvertPitch = true;
    FlightInputCollector fic_i;
    fic_i.setClock(t);
    auto r_i = fic_i.poll(bridge, cam, console, inp, &joy, cs_inv);
    REQUIRE(r_i.has_value());

    CHECK(r_n->elevator * r_i->elevator < 0.f);
}

TEST_CASE("FlightInputCollector nullptr joystick skips HOTAS path", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    inp.held.insert(Key::ArrowUp);
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    // joystick=nullptr must not crash and keyboard must win.
    auto r = fic.poll(bridge, cam, console, inp, nullptr, {});
    REQUIRE(r.has_value());
    CHECK(r->elevator == Catch::Approx(-1.f));
}

// ---------------------------------------------------------------------------
// setBindings / setAxisConfig
// ---------------------------------------------------------------------------

TEST_CASE("FlightInputCollector setBindings remaps PitchAxis to LeftY", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    inp.gamepadCount = 1;
    // Only LeftY populated; RightY = 0.
    inp.axisValues[{0, GamepadAxis::LeftY}] = 0.5f;
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    fl::InputBindings b;
    b.set(fl::InputAction::PitchAxis,
          {fl::BindingSource::GamepadAxis, static_cast<uint32_t>(GamepadAxis::LeftY), false},
          /*alt=*/true);
    fic.setBindings(b);

    auto r = fic.poll(bridge, cam, console, inp, nullptr, {});
    REQUIRE(r.has_value());
    CHECK(r->elevator != Catch::Approx(0.f)); // LeftY drove elevator

    // With only RightY set (LeftY=0), elevator must be zero.
    inp.axisValues[{0, GamepadAxis::LeftY}] = 0.f;
    inp.axisValues[{0, GamepadAxis::RightY}] = 0.5f;
    FlightInputCollector fic2;
    fic2.setClock(t);
    fic2.setBindings(b);
    auto r2 = fic2.poll(bridge, cam, console, inp, nullptr, {});
    REQUIRE(r2.has_value());
    CHECK(r2->elevator == Catch::Approx(0.f));
}

TEST_CASE("FlightInputCollector setBindings PitchAxis None leaves keyboard elevator", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    inp.gamepadCount = 1;
    inp.held.insert(Key::ArrowUp);
    inp.axisValues[{0, GamepadAxis::RightY}] = 0.5f;
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    fl::InputBindings b;
    b.clear(fl::InputAction::PitchAxis, /*alt=*/true); // source = None
    fic.setBindings(b);

    auto r = fic.poll(bridge, cam, console, inp, nullptr, {});
    REQUIRE(r.has_value());
    CHECK(r->elevator == Catch::Approx(-1.f)); // keyboard value preserved
}

TEST_CASE("FlightInputCollector setAxisConfig scale multiplies axis output", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    inp.gamepadCount = 1;
    inp.axisValues[{0, GamepadAxis::RightY}] = 0.5f;
    CameraInput cam;
    fl::SimRenderBridge bridge;
    fl::ManualClock t;

    FlightInputCollector fic_default;
    fic_default.setClock(t);
    auto r_default = fic_default.poll(bridge, cam, console, inp, nullptr, {});
    REQUIRE(r_default.has_value());

    fl::AxisConfigTable axes;
    axes.get(GamepadAxis::RightY).scale = 2.0f;
    FlightInputCollector fic_scaled;
    fic_scaled.setClock(t);
    fic_scaled.setAxisConfig(axes);
    auto r_scaled = fic_scaled.poll(bridge, cam, console, inp, nullptr, {});
    REQUIRE(r_scaled.has_value());

    CHECK(r_scaled->elevator == Catch::Approx(r_default->elevator * 2.0f));
}
