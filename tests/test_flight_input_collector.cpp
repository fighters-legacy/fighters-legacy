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

using namespace fl;

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

TEST_CASE("FlightInputCollector master arm gates the fire triggers (#641)", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    inp.held.insert(Key::Space); // gun trigger
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    // Default is ARM: the trigger fires.
    auto r1 = fic.poll(bridge, cam, console, inp, nullptr, {});
    REQUIRE(r1.has_value());
    CHECK(fic.masterArm());
    CHECK((r1->buttons & 1u) != 0u);
    CHECK(fic.wasWeaponFired());

    // Press the master-arm key to go SAFE (edge-detected). It is Num4, NOT V: V is the radio
    // push-to-talk, and while master arm shared it, keying the mic silently safed the guns (#1050).
    inp.held.insert(Key::Num4);
    t.advance(std::chrono::milliseconds(17));
    auto r2 = fic.poll(bridge, cam, console, inp, nullptr, {});
    REQUIRE(r2.has_value());
    CHECK_FALSE(fic.masterArm());
    // SAFE suppresses the gun trigger (bit 0) and the fire-store (bit 2), and clears wasWeaponFired.
    CHECK((r2->buttons & 1u) == 0u);
    CHECK((r2->buttons & 0x04u) == 0u);
    CHECK_FALSE(fic.wasWeaponFired());

    // Release it, press it again -> back to ARM.
    inp.held.erase(Key::Num4);
    t.advance(std::chrono::milliseconds(17));
    fic.poll(bridge, cam, console, inp, nullptr, {});
    inp.held.insert(Key::Num4);
    t.advance(std::chrono::milliseconds(17));
    auto r4 = fic.poll(bridge, cam, console, inp, nullptr, {});
    REQUIRE(r4.has_value());
    CHECK(fic.masterArm());
    CHECK((r4->buttons & 1u) != 0u);

    // The regression this issue was filed for: holding the radio push-to-talk key must leave the
    // master arm exactly where it was.
    inp.held.erase(Key::Num4);
    t.advance(std::chrono::milliseconds(17));
    fic.poll(bridge, cam, console, inp, nullptr, {});
    inp.held.insert(Key::V);
    t.advance(std::chrono::milliseconds(17));
    auto r5 = fic.poll(bridge, cam, console, inp, nullptr, {});
    REQUIRE(r5.has_value());
    CHECK(fic.masterArm());
    CHECK((r5->buttons & 1u) != 0u);
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
// Snapshot ack (tickIndex + ackMask) is NOT stamped here (#566)
// ---------------------------------------------------------------------------

TEST_CASE("FlightInputCollector leaves the snapshot ack unset", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    CameraInput cam;
    // A populated bridge must NOT be read for the ack: ClientNetEventHandler::stampAck() is now the
    // single ack authority and fills tickIndex + ackMask at the send site (FlightScreen).
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
    CHECK(r->tickIndex == 0u); // left default — stamped later by stampAck()
    CHECK(r->ackMask == 0u);
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
    inp.gpDown.insert({0, GamepadButton::RightShoulder});
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
    inp.gpDown.insert({0, GamepadButton::RightShoulder});
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
    inp.gpDown.insert({0, GamepadButton::LeftShoulder});
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
    inp.gpDown.insert({0, GamepadButton::LeftShoulder});
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
    inp.gpDown.insert({0, GamepadButton::RightShoulder});
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
          {fl::BindingSource::GamepadAxis, static_cast<uint32_t>(GamepadAxis::LeftY), false}, fl::BindingSlot::Gamepad);
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
    b.clear(fl::InputAction::PitchAxis, fl::BindingSlot::Gamepad); // source = None
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

TEST_CASE("FlightInputCollector readButton GamepadAxis positive threshold sets fire bit and wasWeaponFired",
          "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    inp.gamepadCount = 1;
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    fl::InputBindings b;
    b.set(fl::InputAction::FireWeapon,
          {fl::BindingSource::GamepadAxis, static_cast<uint32_t>(GamepadAxis::TriggerRight), false},
          fl::BindingSlot::Gamepad);
    fic.setBindings(b);

    inp.axisValues[{0, GamepadAxis::TriggerRight}] = 0.6f;
    auto r = fic.poll(bridge, cam, console, inp, nullptr, {});
    REQUIRE(r.has_value());
    CHECK((r->buttons & 1u) != 0u);
    CHECK(fic.wasWeaponFired());

    inp.axisValues[{0, GamepadAxis::TriggerRight}] = 0.4f;
    FlightInputCollector fic2;
    fic2.setClock(t);
    fic2.setBindings(b);
    auto r2 = fic2.poll(bridge, cam, console, inp, nullptr, {});
    REQUIRE(r2.has_value());
    CHECK((r2->buttons & 1u) == 0u);
    CHECK_FALSE(fic2.wasWeaponFired());
}

TEST_CASE("FlightInputCollector readButton GamepadAxis negative threshold sets afterburner bit", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    inp.gamepadCount = 1;
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    fl::InputBindings b;
    b.set(fl::InputAction::Afterburner,
          {fl::BindingSource::GamepadAxis, static_cast<uint32_t>(GamepadAxis::LeftY), true}, fl::BindingSlot::Gamepad);
    fic.setBindings(b);

    inp.axisValues[{0, GamepadAxis::LeftY}] = -0.6f;
    auto r = fic.poll(bridge, cam, console, inp, nullptr, {});
    REQUIRE(r.has_value());
    CHECK((r->buttons & 2u) != 0u);

    inp.axisValues[{0, GamepadAxis::LeftY}] = -0.4f;
    FlightInputCollector fic2;
    fic2.setClock(t);
    fic2.setBindings(b);
    auto r2 = fic2.poll(bridge, cam, console, inp, nullptr, {});
    REQUIRE(r2.has_value());
    CHECK((r2->buttons & 2u) == 0u);
}

TEST_CASE("FlightInputCollector readButton None alt binding does not set fire bit", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    inp.gamepadCount = 1;
    inp.gpDown.insert({0, GamepadButton::RightShoulder});
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    fl::InputBindings b;
    b.clear(fl::InputAction::FireWeapon, fl::BindingSlot::Gamepad);
    fic.setBindings(b);

    auto r = fic.poll(bridge, cam, console, inp, nullptr, {});
    REQUIRE(r.has_value());
    CHECK((r->buttons & 1u) == 0u);
    CHECK_FALSE(fic.wasWeaponFired());
}

// ---------------------------------------------------------------------------
// Fire-store bit + weapon-station selection (#625)
// ---------------------------------------------------------------------------

TEST_CASE("FlightInputCollector Enter and MouseRight set the fire-store bit", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    inp.held.insert(Key::Enter);
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    auto r = fic.poll(bridge, cam, console, inp, nullptr, {});
    REQUIRE(r.has_value());
    CHECK((r->buttons & 0x04u) != 0u);
    CHECK(r->selectedStation == 255u); // no station count set: selection stays "keep"

    inp.held.clear();
    inp.mouseDown.insert(MouseButton::Right);
    t.advance(std::chrono::milliseconds(17));
    auto r2 = fic.poll(bridge, cam, console, inp, nullptr, {});
    REQUIRE(r2.has_value());
    CHECK((r2->buttons & 0x04u) != 0u);
}

TEST_CASE("FlightInputCollector station cycling is edge-triggered, wraps, and is absolute on the wire",
          "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);
    fic.setStationCount(3);

    // First Next press: 255 ("keep the server default") lands on station 0 — a deliberate cycle is
    // the ONLY thing that replaces the sentinel.
    inp.held.insert(Key::Num1);
    auto r = fic.poll(bridge, cam, console, inp, nullptr, {});
    REQUIRE(r.has_value());
    CHECK(r->selectedStation == 0u);

    // Held across the next poll: no new edge, no further cycling.
    t.advance(std::chrono::milliseconds(17));
    r = fic.poll(bridge, cam, console, inp, nullptr, {});
    REQUIRE(r.has_value());
    CHECK(r->selectedStation == 0u);

    // Release + press again: 0 -> 1, then 1 -> 2.
    inp.held.clear();
    t.advance(std::chrono::milliseconds(17));
    fic.poll(bridge, cam, console, inp, nullptr, {});
    inp.held.insert(Key::Num1);
    t.advance(std::chrono::milliseconds(17));
    r = fic.poll(bridge, cam, console, inp, nullptr, {});
    CHECK(r->selectedStation == 1u);
    inp.held.clear();
    t.advance(std::chrono::milliseconds(17));
    fic.poll(bridge, cam, console, inp, nullptr, {});
    inp.held.insert(Key::Num1);
    t.advance(std::chrono::milliseconds(17));
    r = fic.poll(bridge, cam, console, inp, nullptr, {});
    CHECK(r->selectedStation == 2u);

    // Prev wraps: 2 -> 1 -> 0 -> 2 would take three presses; go straight around from 2 via Next.
    inp.held.clear();
    t.advance(std::chrono::milliseconds(17));
    fic.poll(bridge, cam, console, inp, nullptr, {});
    inp.held.insert(Key::Num1);
    t.advance(std::chrono::milliseconds(17));
    r = fic.poll(bridge, cam, console, inp, nullptr, {});
    CHECK(r->selectedStation == 0u); // Next wrapped 2 -> 0

    // And Prev wraps the other way: 0 -> 2.
    inp.held.clear();
    t.advance(std::chrono::milliseconds(17));
    fic.poll(bridge, cam, console, inp, nullptr, {});
    inp.held.insert(Key::Num2);
    t.advance(std::chrono::milliseconds(17));
    r = fic.poll(bridge, cam, console, inp, nullptr, {});
    CHECK(r->selectedStation == 2u);
}

TEST_CASE("FlightInputCollector uiFocused gates the discrete weapon keys but leaves flight axes live",
          "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    inp.held.insert(Key::Enter);
    inp.held.insert(Key::Num1);
    inp.held.insert(Key::ArrowUp);
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);
    fic.setStationCount(3);

    // The radio menu is open (#610): its picks are the digit keys, and a confirm-style key must not
    // release a store. The aircraft still flies.
    auto r = fic.poll(bridge, cam, console, inp, nullptr, {}, /*uiFocused=*/true);
    REQUIRE(r.has_value());
    CHECK((r->buttons & 0x04u) == 0u);
    CHECK(r->selectedStation == 255u);
    CHECK(r->elevator == -1.f);
}

TEST_CASE("FlightInputCollector gamepad FireStore and D-pad cycling reach the wire", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    inp.gamepadCount = 1;
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);
    fic.setStationCount(2);

    // Gamepad defaults (#625 fixed the collision): FireStore, NextWeapon=DpadRight, PrevWeapon=DpadLeft.
    fl::InputBindings b;
    const fl::Binding fireB = b.get(fl::InputAction::FireStore, fl::BindingSlot::Gamepad);
    const fl::Binding nextB = b.get(fl::InputAction::NextWeapon, fl::BindingSlot::Gamepad);
    REQUIRE(fireB.source == fl::BindingSource::GamepadButton);
    REQUIRE(nextB.source == fl::BindingSource::GamepadButton);
    CHECK(static_cast<GamepadButton>(nextB.id) == GamepadButton::DpadRight);

    inp.gpDown.insert({0, static_cast<GamepadButton>(fireB.id)});
    inp.gpDown.insert({0, static_cast<GamepadButton>(nextB.id)});
    auto r = fic.poll(bridge, cam, console, inp, nullptr, {});
    REQUIRE(r.has_value());
    CHECK((r->buttons & 0x04u) != 0u);
    CHECK(r->selectedStation == 0u); // first deliberate cycle replaces the "keep" sentinel with 0
}

// ---------------------------------------------------------------------------
// Articulation switches (#639)
//
// The InputAction::LandingGear / Flaps bindings existed and were never read: a human pilot could not
// raise the gear at all. These are LATCHED client-side and sent as absolute state, so a dropped
// packet costs a tick of lag rather than a sortie in the wrong configuration.
// ---------------------------------------------------------------------------

namespace {

// Advances the rate limiter and returns the packet the collector produced.
fl::MsgClientInput pollNext(FlightInputCollector& fic, fl::ManualClock& t, fl::SimRenderBridge& bridge,
                            CameraInput& cam, GameConsole& console, MockInput& inp) {
    t.advance(std::chrono::milliseconds(20));
    auto r = fic.poll(bridge, cam, console, inp, nullptr, {});
    REQUIRE(r.has_value());
    return *r;
}

} // namespace

TEST_CASE("FlightInputCollector: gear starts DOWN and G toggles it (#639)", "[flight_input][articulation]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    // An aircraft is parked on its wheels, so the switch starts DOWN — matching the server's
    // parked-spawn configuration, which is what stops a fresh sortie from belly-scraping.
    auto first = fic.poll(bridge, cam, console, inp, nullptr, {});
    REQUIRE(first.has_value());
    CHECK((first->artButtons & fl::kArtButtonGearDown) != 0);
    CHECK(fic.gearDown());

    // Hold G: the toggle is edge-detected, so holding it flips exactly once.
    inp.held.insert(Key::G);
    CHECK((pollNext(fic, t, bridge, cam, console, inp).artButtons & fl::kArtButtonGearDown) == 0);
    CHECK((pollNext(fic, t, bridge, cam, console, inp).artButtons & fl::kArtButtonGearDown) == 0);
    CHECK_FALSE(fic.gearDown());

    // Release and press again: back down.
    inp.held.erase(Key::G);
    (void)pollNext(fic, t, bridge, cam, console, inp);
    inp.held.insert(Key::G);
    CHECK((pollNext(fic, t, bridge, cam, console, inp).artButtons & fl::kArtButtonGearDown) != 0);
}

TEST_CASE("FlightInputCollector: F steps the flap detent clean/manoeuvre/full (#639)", "[flight_input][articulation]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    auto first = fic.poll(bridge, cam, console, inp, nullptr, {});
    REQUIRE(first.has_value());
    CHECK(first->flaps == 0); // clean

    auto tap = [&]() {
        inp.held.insert(Key::F);
        const auto msg = pollNext(fic, t, bridge, cam, console, inp);
        inp.held.erase(Key::F);
        (void)pollNext(fic, t, bridge, cam, console, inp);
        return msg;
    };

    // Three detents, wrapping — the positions a real flap lever has, not a continuous axis.
    CHECK(tap().flaps == 128); // manoeuvre (0.5 -> 127.5, rounds to 128)
    CHECK(tap().flaps == 255); // full
    CHECK(tap().flaps == 0);   // back to clean
}

TEST_CASE("FlightInputCollector: hook and canopy latch; the airbrake is momentary (#639)",
          "[flight_input][articulation]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);
    (void)fic.poll(bridge, cam, console, inp, nullptr, {});

    // H latches the hook down and it STAYS down after release — a switch, not a button.
    inp.held.insert(Key::H);
    CHECK((pollNext(fic, t, bridge, cam, console, inp).artButtons & fl::kArtButtonHookDown) != 0);
    inp.held.erase(Key::H);
    CHECK((pollNext(fic, t, bridge, cam, console, inp).artButtons & fl::kArtButtonHookDown) != 0);
    CHECK(fic.hookDown());

    // The canopy has a key of its OWN. It used to be the Shift+C chord, which the binding table
    // cannot express and whose modifier is itself a bound action (LeftShift = max throttle), so
    // neither half was rebindable and neither was visible to the conflict check (#1050). C alone is
    // still the wingman radio menu and must do nothing here.
    inp.held.insert(Key::C);
    CHECK((pollNext(fic, t, bridge, cam, console, inp).artButtons & fl::kArtButtonCanopyOpen) == 0);
    inp.held.erase(Key::C);
    inp.held.insert(Key::LeftBracket);
    CHECK((pollNext(fic, t, bridge, cam, console, inp).artButtons & fl::kArtButtonCanopyOpen) != 0);
    inp.held.erase(Key::LeftBracket);

    // K is MOMENTARY: the airbrake retracts the moment it is released, unlike every switch above.
    inp.held.insert(Key::K);
    CHECK(pollNext(fic, t, bridge, cam, console, inp).speedbrake == 255);
    inp.held.erase(Key::K);
    CHECK(pollNext(fic, t, bridge, cam, console, inp).speedbrake == 0);
}

TEST_CASE("FlightInputCollector: articulation state rides EVERY packet (#639)", "[flight_input][articulation]") {
    // Absolute state, not edges: a client that stops repeating the configuration would leave the
    // server unable to recover from a single dropped packet.
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);
    (void)fic.poll(bridge, cam, console, inp, nullptr, {});

    inp.held.insert(Key::G); // gear up
    (void)pollNext(fic, t, bridge, cam, console, inp);
    inp.held.erase(Key::G);

    for (int i = 0; i < 10; ++i) {
        const auto msg = pollNext(fic, t, bridge, cam, console, inp);
        CHECK((msg.artButtons & fl::kArtButtonGearDown) == 0); // repeated, unchanged, every packet
    }
}

// ---------------------------------------------------------------------------
// #1050 — the binding table is the authority
// ---------------------------------------------------------------------------

TEST_CASE("FlightInputCollector: master arm honours its own binding (#1050)", "[flight_input][bindings]") {
    // The second half of #1050: FlightInputCollector read `Key::V` directly instead of resolving
    // InputAction::MasterArm, so rebinding master arm did nothing AND rebinding push-to-talk off V
    // did not clear the collision either — the one workaround a player would reach for.
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    fl::InputBindings b;
    b.set(fl::InputAction::MasterArm, {fl::BindingSource::Keyboard, static_cast<uint32_t>(Key::Num8), false});
    fic.setBindings(b);
    REQUIRE(fic.masterArm());

    // The DEFAULT key no longer does anything once the action has been rebound.
    inp.held.insert(Key::Num4);
    t.advance(std::chrono::milliseconds(17));
    REQUIRE(fic.poll(bridge, cam, console, inp, nullptr, {}).has_value());
    CHECK(fic.masterArm());
    inp.held.erase(Key::Num4);

    // The rebound key does.
    inp.held.insert(Key::Num8);
    t.advance(std::chrono::milliseconds(17));
    REQUIRE(fic.poll(bridge, cam, console, inp, nullptr, {}).has_value());
    CHECK_FALSE(fic.masterArm());
}

TEST_CASE("FlightInputCollector: a secondary slot drives the same action (#1050)", "[flight_input][bindings]") {
    // The gun is Space AND the left mouse button. With two slots one of the two had to be
    // hardcoded in this file, outside both the rebind path and the conflict check.
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    inp.held.insert(Key::Space);
    auto r1 = fic.poll(bridge, cam, console, inp, nullptr, {});
    REQUIRE(r1.has_value());
    CHECK((r1->buttons & fl::kInputButtonGun) != 0u);

    inp.held.erase(Key::Space);
    inp.mouseDown.insert(MouseButton::Left);
    t.advance(std::chrono::milliseconds(17));
    auto r2 = fic.poll(bridge, cam, console, inp, nullptr, {});
    REQUIRE(r2.has_value());
    CHECK((r2->buttons & fl::kInputButtonGun) != 0u);
}

TEST_CASE("FlightInputCollector: every rebound flight control follows its action (#1050)", "[flight_input][bindings]") {
    // A sweep rather than one case per control: the defect class was "this control does not go
    // through the table", and the only way to be sure none is left is to move them all and check.
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);
    fic.setStationCount(3);

    fl::InputBindings b;
    struct Remap {
        fl::InputAction action;
        Key key;
    };
    // Deliberately onto keys no default uses, so a control still reading its old hardcoded key
    // simply would not fire.
    const Remap remaps[] = {
        {fl::InputAction::LandingGear, Key::Numpad0},      {fl::InputAction::Flaps, Key::Numpad1},
        {fl::InputAction::ArrestorHook, Key::Numpad5},     {fl::InputAction::CanopyToggle, Key::Numpad7},
        {fl::InputAction::Airbrake, Key::NumpadDivide},    {fl::InputAction::WheelBrake, Key::RightShift},
        {fl::InputAction::Eject, Key::RightCtrl},          {fl::InputAction::Respawn, Key::RightAlt},
        {fl::InputAction::CountermeasureDispense, Key::L}, {fl::InputAction::EcmToggle, Key::RightBracket},
    };
    for (const auto& r : remaps)
        b.set(r.action, {fl::BindingSource::Keyboard, static_cast<uint32_t>(r.key), false});
    fic.setBindings(b);

    auto pump = [&]() {
        t.advance(std::chrono::milliseconds(17));
        auto r = fic.poll(bridge, cam, console, inp, nullptr, {});
        REQUIRE(r.has_value());
        return *r;
    };

    pump(); // settle: gear starts DOWN

    inp.held.insert(Key::Numpad0);
    CHECK((pump().artButtons & fl::kArtButtonGearDown) == 0); // gear up
    inp.held.erase(Key::Numpad0);

    inp.held.insert(Key::Numpad1);
    CHECK(pump().flaps > 0); // first detent
    inp.held.erase(Key::Numpad1);

    inp.held.insert(Key::Numpad5);
    CHECK((pump().artButtons & fl::kArtButtonHookDown) != 0);
    inp.held.erase(Key::Numpad5);

    inp.held.insert(Key::Numpad7);
    CHECK((pump().artButtons & fl::kArtButtonCanopyOpen) != 0);
    inp.held.erase(Key::Numpad7);

    inp.held.insert(Key::NumpadDivide);
    CHECK(pump().speedbrake == 255);
    inp.held.erase(Key::NumpadDivide);

    inp.held.insert(Key::RightShift);
    CHECK((pump().buttons & fl::kInputButtonWheelBrake) != 0u);
    inp.held.erase(Key::RightShift);

    inp.held.insert(Key::RightCtrl);
    CHECK((pump().buttons & fl::kInputButtonEject) != 0u);
    inp.held.erase(Key::RightCtrl);

    inp.held.insert(Key::RightAlt);
    CHECK((pump().buttons & fl::kInputButtonRespawn) != 0u);
    inp.held.erase(Key::RightAlt);

    inp.held.insert(Key::L);
    CHECK((pump().buttons & fl::kInputButtonChaffFlare) != 0u);
    inp.held.erase(Key::L);

    inp.held.insert(Key::RightBracket);
    CHECK((pump().buttons & fl::kInputButtonEcm) != 0u);
    inp.held.erase(Key::RightBracket);
    CHECK((pump().buttons & fl::kInputButtonEcm) != 0u); // the jammer is a latch, not a hold
}
