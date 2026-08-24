// SPDX-License-Identifier: GPL-3.0-or-later
#include "IClock.h"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "CameraInput.h"
#include "FlightInputCollector.h"
#include "console/CommandRegistry.h"
#include "console/GameConsole.h"
#include "input/AxisConfig.h"
#include "input/InputBindings.h"
#include "input/InputSources.h"
#include "input/JoystickDevices.h"
#include "mock_hal.h"
#include "render/RenderSnapshot.h"
#include "render/SimRenderBridge.h"

#include <chrono>

using namespace fl;

namespace {

// Keyboard / mouse / gamepad only — no stick attached.
fl::InputSources kbOnly(MockInput& in) {
    return fl::InputSources{&in, nullptr, nullptr, 0};
}

// With a raw joystick and its device table (#1061). The table has to be update()d against the mock
// before the sources are used, or nothing resolves.
fl::InputSources withStick(MockInput& in, MockJoystick& joy, fl::JoystickDevices& devices) {
    devices.update(joy);
    return fl::InputSources{&in, &joy, &devices, 0};
}

fl::Binding keyBinding(Key k) {
    fl::Binding b{};
    b.source = fl::BindingSource::Keyboard;
    b.id = static_cast<uint32_t>(k);
    return b;
}

fl::Binding padAxisBinding(GamepadAxis a, bool negative = false) {
    fl::Binding b{};
    b.source = fl::BindingSource::GamepadAxis;
    b.id = static_cast<uint32_t>(a);
    b.axisNegative = negative;
    return b;
}

// The action's gamepad-button binding. Slots are gone (#1061), so "the pad binding" is now found by
// source rather than by pigeonhole.
fl::Binding padBindingOf(const fl::InputBindings& binds, fl::InputAction action) {
    for (const fl::Binding& b : binds.get(action))
        if (b.source == fl::BindingSource::GamepadButton)
            return b;
    return fl::Binding{};
}

// The single HOTAS device the migrated axis tests drive. Eight axes is enough for every index they use.
MockJoystick::Device& hotasDevice(MockJoystick& joy) {
    auto& dev = joy.addDevice("03000000a1b2c3d4000000000000aaaa", "MockHotas");
    dev.axes.assign(8, 0.0f);
    dev.buttons.assign(16, false);
    dev.justPressed.assign(16, false);
    return dev;
}

} // namespace

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

    auto r = fic.poll(bridge, cam, console, kbOnly(inp));
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

    fic.poll(bridge, cam, console, kbOnly(inp));
    auto r2 = fic.poll(bridge, cam, console, kbOnly(inp));
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

    fic.poll(bridge, cam, console, kbOnly(inp));
    CHECK(fic.wasWeaponFired());

    // Same tick — nullopt returned, but weaponFired still resets.
    fic.poll(bridge, cam, console, kbOnly(inp));
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
    auto r1 = fic.poll(bridge, cam, console, kbOnly(inp));
    REQUIRE(r1.has_value());
    CHECK(fic.masterArm());
    CHECK((r1->buttons & 1u) != 0u);
    CHECK(fic.wasWeaponFired());

    // Press the master-arm key to go SAFE (edge-detected). It is Num4, NOT V: V is the radio
    // push-to-talk, and while master arm shared it, keying the mic silently safed the guns (#1050).
    inp.held.insert(Key::Num4);
    t.advance(std::chrono::milliseconds(17));
    auto r2 = fic.poll(bridge, cam, console, kbOnly(inp));
    REQUIRE(r2.has_value());
    CHECK_FALSE(fic.masterArm());
    // SAFE suppresses the gun trigger (bit 0) and the fire-store (bit 2), and clears wasWeaponFired.
    CHECK((r2->buttons & 1u) == 0u);
    CHECK((r2->buttons & 0x04u) == 0u);
    CHECK_FALSE(fic.wasWeaponFired());

    // Release it, press it again -> back to ARM.
    inp.held.erase(Key::Num4);
    t.advance(std::chrono::milliseconds(17));
    fic.poll(bridge, cam, console, kbOnly(inp));
    inp.held.insert(Key::Num4);
    t.advance(std::chrono::milliseconds(17));
    auto r4 = fic.poll(bridge, cam, console, kbOnly(inp));
    REQUIRE(r4.has_value());
    CHECK(fic.masterArm());
    CHECK((r4->buttons & 1u) != 0u);

    // The regression this issue was filed for: holding the radio push-to-talk key must leave the
    // master arm exactly where it was.
    inp.held.erase(Key::Num4);
    t.advance(std::chrono::milliseconds(17));
    fic.poll(bridge, cam, console, kbOnly(inp));
    inp.held.insert(Key::V);
    t.advance(std::chrono::milliseconds(17));
    auto r5 = fic.poll(bridge, cam, console, kbOnly(inp));
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

    fic.poll(bridge, cam, console, kbOnly(inp));
    t.advance(std::chrono::milliseconds(17));
    auto r = fic.poll(bridge, cam, console, kbOnly(inp));
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

    auto r0 = fic.poll(bridge, cam, console, kbOnly(inp));
    REQUIRE(r0.has_value());
    CHECK(r0->seqNum == 0u);

    t.advance(std::chrono::milliseconds(17));
    auto r1 = fic.poll(bridge, cam, console, kbOnly(inp));
    REQUIRE(r1.has_value());
    CHECK(r1->seqNum == 1u);

    t.advance(std::chrono::milliseconds(17));
    auto r2 = fic.poll(bridge, cam, console, kbOnly(inp));
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

    auto r = fic.poll(bridge, cam, console, kbOnly(inp));
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

    auto r = fic.poll(bridge, cam, console, kbOnly(inp));
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

    auto r = fic.poll(bridge, cam, console, kbOnly(inp));
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

    auto r = fic.poll(bridge, cam, console, kbOnly(inp));
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

    auto r = fic.poll(bridge, cam, console, kbOnly(inp));
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

    auto r = fic.poll(bridge, cam, console, kbOnly(inp));
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

    auto r = fic.poll(bridge, cam, console, kbOnly(inp));
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

    auto r = fic.poll(bridge, cam, console, kbOnly(inp));
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

    auto r = fic.poll(bridge, cam, console, kbOnly(inp));
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

    auto r = fic.poll(bridge, cam, console, kbOnly(inp));
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

    // Two polls: the first only establishes the timestamp the ramp measures against, because the
    // throttle moves at a RATE now and no time has passed yet (#1241). In play that is the opening
    // frame of the session, one frame before any input can have been held for any length of time.
    fic.poll(bridge, cam, console, kbOnly(inp));
    const float before = cam.throttle();
    t.advance(std::chrono::milliseconds(20));
    fic.poll(bridge, cam, console, kbOnly(inp));
    CHECK(cam.throttle() > before);
}

// #1241: the throttle ramp is a RATE, so the same wall-clock interval must move it the same
// distance however often poll() is called during it. It used to add a flat 1/60 per accepted poll,
// which is one full travel per second only while polls land at 60 Hz — on a 30 fps machine the
// throttle ramped at half speed, a control that responded differently depending on the GPU.
//
// 50 and 25 Hz, not 60 and 30: a 60 Hz step lands exactly ON the send period and whether it passes
// the >= test is then a float-rounding coin toss. And 0.4 s, not a full second: adjustThrottle
// clamps at 1.0, so a longer run has both arms saturate and agree for the wrong reason. The first
// version of this test did exactly that and passed against the unfixed code.
TEST_CASE("FlightInputCollector throttle ramps at the same rate at 25 and 50 fps (#1241)", "[flight_input]") {
    constexpr float kRunSeconds = 0.4f;
    auto rampFor = [](int fps) {
        MockLogger log;
        CommandRegistry reg;
        GameConsole console(log, reg);
        MockInput inp;
        CameraInput cam;
        fl::SimRenderBridge bridge;
        FlightInputCollector fic;
        fl::ManualClock t;
        fic.setClock(t);
        cam.setThrottle(0.f);
        inp.held.insert(Key::PageUp); // ThrottleUp

        const auto step = std::chrono::nanoseconds(1'000'000'000LL / fps);
        for (int i = 0; i < static_cast<int>(kRunSeconds * static_cast<float>(fps)); ++i) {
            t.advance(step);
            fic.poll(bridge, cam, console, kbOnly(inp));
        }
        return cam.throttle();
    };

    const float at50 = rampFor(50);
    const float at25 = rampFor(25);
    INFO("50 fps -> " << at50 << ", 25 fps -> " << at25);
    // One frame of tolerance: a 25 Hz arm can be up to one 40 ms step behind a 50 Hz one.
    CHECK(at50 == Catch::Approx(at25).margin(0.05f));
    // Neither arm may be saturated or the comparison is two ceilings agreeing, and it must actually
    // have moved roughly the elapsed time at 1.0/s.
    CHECK(at50 < 0.95f);
    CHECK(at50 == Catch::Approx(kRunSeconds).margin(0.05f)); // 1.0/s over kRunSeconds
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

    // Start at half-throttle so there is room to decrease. Two polls: the first establishes the
    // timestamp the rate-based ramp measures against (#1241).
    fic.poll(bridge, cam, console, kbOnly(inp));
    cam.setThrottle(0.5f);
    t.advance(std::chrono::milliseconds(20));
    fic.poll(bridge, cam, console, kbOnly(inp));
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

    auto r = fic.poll(bridge, cam, console, kbOnly(inp));
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

    auto r = fic.poll(bridge, cam, console, kbOnly(inp));
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

    auto r = fic.poll(bridge, cam, console, kbOnly(inp));
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

    auto r = fic.poll(bridge, cam, console, kbOnly(inp));
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
    inp.gpDown.insert({0, GamepadButton::RightShoulder});
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    auto r = fic.poll(bridge, cam, console, kbOnly(inp));
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

    auto r = fic.poll(bridge, cam, console, kbOnly(inp));
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
    inp.gpDown.insert({0, GamepadButton::RightShoulder});
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    auto r = fic.poll(bridge, cam, console, kbOnly(inp));
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
    inp.gpDown.insert({0, GamepadButton::LeftShoulder});
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    auto r = fic.poll(bridge, cam, console, kbOnly(inp));
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
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    auto r = fic.poll(bridge, cam, console, kbOnly(inp));
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
    const float dz =
        axes.effective(fl::AxisKey{fl::BindingSource::GamepadAxis, static_cast<uint32_t>(GamepadAxis::TriggerLeft), {}})
            .deadzone; // 0.1f default
    inp.axisValues[{0, GamepadAxis::TriggerLeft}] = dz;
    CameraInput cam;
    cam.setThrottle(0.3f);
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);
    fic.setAxisConfig(axes);

    auto r = fic.poll(bridge, cam, console, kbOnly(inp));
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
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    auto r = fic.poll(bridge, cam, console, kbOnly(inp));
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
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    auto r = fic.poll(bridge, cam, console, kbOnly(inp));
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
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    auto r = fic.poll(bridge, cam, console, kbOnly(inp));
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
    // Below deadzone — should not override.
    inp.axisValues[{0, GamepadAxis::RightY}] = 0.01f;
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    auto r = fic.poll(bridge, cam, console, kbOnly(inp));
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
    auto r_normal = fic_normal.poll(bridge, cam, console, kbOnly(inp));
    REQUIRE(r_normal.has_value());

    fl::AxisConfigTable axisInv;
    {
        const fl::AxisKey k{fl::BindingSource::GamepadAxis, static_cast<uint32_t>(GamepadAxis::RightY), {}};
        fl::AxisConfig c = axisInv.effective(k);
        c.invert = true;
        axisInv.set(k, c);
    }
    FlightInputCollector fic_inv;
    fic_inv.setClock(t);
    fic_inv.setAxisConfig(axisInv);
    auto r_inv = fic_inv.poll(bridge, cam, console, kbOnly(inp));
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
    inp.gpDown.insert({0, GamepadButton::LeftShoulder});
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    auto r = fic.poll(bridge, cam, console, kbOnly(inp));
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
    inp.gpDown.insert({0, GamepadButton::RightShoulder});
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    auto r = fic.poll(bridge, cam, console, kbOnly(inp));
    REQUIRE(r.has_value());
    CHECK((r->buttons & 3u) == 3u);
}

// ---------------------------------------------------------------------------
// Raw joystick / HOTAS path (#1061)
//
// The HOTAS axes are ordinary bindings now — `JoystickAxis` entries in the shipped defaults with their
// tuning in the axis-config table — so these tests drive them through the same actionAxis() path the
// gamepad uses. There is no separate HOTAS block in the collector left to test.
// ---------------------------------------------------------------------------

TEST_CASE("FlightInputCollector HOTAS throttle axis sets absolute throttle", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    MockJoystick joy;
    fl::JoystickDevices devices;
    // Default throttle axis = 2, mode Absolute: raw 0.5 -> (0.5 + 1) / 2 = 0.75.
    hotasDevice(joy).axes[fl::kHotasAxisThrottle] = 0.5f;
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    auto r = fic.poll(bridge, cam, console, withStick(inp, joy, devices));
    REQUIRE(r.has_value());
    CHECK(r->throttle == Catch::Approx(0.75f));
}

TEST_CASE("FlightInputCollector HOTAS throttle at idle still overrides the keyboard", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    MockJoystick joy;
    fl::JoystickDevices devices;
    hotasDevice(joy).axes[fl::kHotasAxisThrottle] = -1.0f; // lever closed
    CameraInput cam;
    cam.setThrottle(0.6f); // whatever the keyboard had built up
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    auto r = fic.poll(bridge, cam, console, withStick(inp, joy, devices));
    REQUIRE(r.has_value());
    // An absolute lever is in command even when it reads zero. If it were treated as "inactive because
    // the value is 0" the keyboard throttle would win and the aircraft would fly at 60% with the
    // throttle shut.
    CHECK(r->throttle == Catch::Approx(0.0f));
}

TEST_CASE("FlightInputCollector HOTAS elevator axis overrides keyboard", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    inp.held.insert(Key::ArrowUp);
    MockJoystick joy;
    fl::JoystickDevices devices;
    hotasDevice(joy).axes[fl::kHotasAxisPitch] = 0.6f;
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    auto r = fic.poll(bridge, cam, console, withStick(inp, joy, devices));
    REQUIRE(r.has_value());
    // 0.6 is past the 0.05 HOTAS deadzone, so it overrides ArrowUp (-1).
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
    fl::JoystickDevices devices;
    hotasDevice(joy).axes[fl::kHotasAxisRoll] = 0.6f;
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    auto r = fic.poll(bridge, cam, console, withStick(inp, joy, devices));
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
    fl::JoystickDevices devices;
    hotasDevice(joy).axes[fl::kHotasAxisYaw] = 0.6f;
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    auto r = fic.poll(bridge, cam, console, withStick(inp, joy, devices));
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
    fl::JoystickDevices devices;
    hotasDevice(joy).axes[fl::kHotasAxisPitch] = fl::kHotasDefaultDeadzone;
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    auto r = fic.poll(bridge, cam, console, withStick(inp, joy, devices));
    REQUIRE(r.has_value());
    CHECK(r->elevator == Catch::Approx(-1.f));
}

TEST_CASE("FlightInputCollector HOTAS axis inversion flips elevator sign", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    MockJoystick joy;
    fl::JoystickDevices devices;
    hotasDevice(joy).axes[fl::kHotasAxisPitch] = 0.6f;
    CameraInput cam;
    fl::SimRenderBridge bridge;

    FlightInputCollector fic_n;
    fl::ManualClock t;
    fic_n.setClock(t);
    auto r_n = fic_n.poll(bridge, cam, console, withStick(inp, joy, devices));
    REQUIRE(r_n.has_value());

    // Inversion is per-axis config now, not a hotas_invert_pitch flag in a second config file.
    fl::AxisConfigTable inverted;
    fl::AxisConfig cfg = inverted.effective(fl::AxisKey{fl::BindingSource::JoystickAxis, fl::kHotasAxisPitch, {}});
    cfg.invert = true;
    inverted.set(fl::AxisKey{fl::BindingSource::JoystickAxis, fl::kHotasAxisPitch, {}}, cfg);
    FlightInputCollector fic_i;
    fic_i.setClock(t);
    fic_i.setAxisConfig(inverted);
    auto r_i = fic_i.poll(bridge, cam, console, withStick(inp, joy, devices));
    REQUIRE(r_i.has_value());

    CHECK(r_n->elevator * r_i->elevator < 0.f);
}

TEST_CASE("FlightInputCollector a HOTAS BUTTON can fire the gun (#1061)", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    MockJoystick joy;
    fl::JoystickDevices devices;
    hotasDevice(joy);
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    // The headline of #1061: before it, BindingSource had no joystick entry at all, so a HOTAS trigger
    // could not be bound to ANY action and nothing in game/ or engine/ ever called IJoystick's button
    // methods.
    fl::InputBindings b;
    fl::Binding trigger{};
    trigger.source = fl::BindingSource::JoystickButton;
    trigger.id = 5;
    trigger.device = fl::makeDeviceRef("03000000a1b2c3d4000000000000aaaa");
    REQUIRE(b.add(fl::InputAction::FireWeapon, trigger));
    fic.setBindings(b);

    auto r = fic.poll(bridge, cam, console, withStick(inp, joy, devices));
    REQUIRE(r.has_value());
    CHECK((r->buttons & fl::kInputButtonGun) == 0u);

    joy.devices[0].buttons[5] = true;
    t.advance(std::chrono::milliseconds(20));
    auto r2 = fic.poll(bridge, cam, console, withStick(inp, joy, devices));
    REQUIRE(r2.has_value());
    CHECK((r2->buttons & fl::kInputButtonGun) != 0u);
    CHECK(fic.wasWeaponFired());
}

TEST_CASE("FlightInputCollector a HOTAS HAT can latch the landing gear (#1061)", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    MockJoystick joy;
    fl::JoystickDevices devices;
    auto& dev = hotasDevice(joy);
    dev.hats.assign(1, HatPosition::Centered);
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    fl::InputBindings b;
    fl::Binding hat{};
    hat.source = fl::BindingSource::JoystickHat;
    hat.id = 0;
    hat.hat = HatPosition::Down;
    hat.device = fl::makeDeviceRef("03000000a1b2c3d4000000000000aaaa");
    b.set(fl::InputAction::LandingGear, std::vector<fl::Binding>{hat});
    fic.setBindings(b);

    REQUIRE(fic.gearDown()); // parked configuration
    joy.devices[0].hats[0] = HatPosition::Down;
    t.advance(std::chrono::milliseconds(20));
    REQUIRE(fic.poll(bridge, cam, console, withStick(inp, joy, devices)).has_value());
    CHECK_FALSE(fic.gearDown());

    // A HELD hat is one toggle, not one per frame — the latch rides the collector's own edge tracker,
    // which is what makes an axis- or hat-driven switch behave like a button.
    t.advance(std::chrono::milliseconds(20));
    REQUIRE(fic.poll(bridge, cam, console, withStick(inp, joy, devices)).has_value());
    CHECK_FALSE(fic.gearDown());

    joy.devices[0].hats[0] = HatPosition::Centered;
    t.advance(std::chrono::milliseconds(20));
    REQUIRE(fic.poll(bridge, cam, console, withStick(inp, joy, devices)).has_value());
    joy.devices[0].hats[0] = HatPosition::Down;
    t.advance(std::chrono::milliseconds(20));
    REQUIRE(fic.poll(bridge, cam, console, withStick(inp, joy, devices)).has_value());
    CHECK(fic.gearDown());
}

TEST_CASE("FlightInputCollector an absent stick leaves the keyboard in charge", "[flight_input]") {
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    inp.held.insert(Key::ArrowUp);
    MockJoystick joy; // no devices at all
    fl::JoystickDevices devices;
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    // The shipped defaults bind a joystick axis to every flight axis. With nothing plugged in those
    // bindings are inert and the keyboard is untouched — a binding on an absent device is preserved and
    // does nothing, never pruned and never resolved against some other device.
    auto r = fic.poll(bridge, cam, console, withStick(inp, joy, devices));
    REQUIRE(r.has_value());
    CHECK(r->elevator == Catch::Approx(-1.f));
    CHECK(r->throttle == Catch::Approx(0.f));
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
    b.set(fl::InputAction::PitchAxis, std::vector<fl::Binding>{padAxisBinding(GamepadAxis::LeftY)});
    fic.setBindings(b);

    auto r = fic.poll(bridge, cam, console, kbOnly(inp));
    REQUIRE(r.has_value());
    CHECK(r->elevator != Catch::Approx(0.f)); // LeftY drove elevator

    // With only RightY set (LeftY=0), elevator must be zero.
    inp.axisValues[{0, GamepadAxis::LeftY}] = 0.f;
    inp.axisValues[{0, GamepadAxis::RightY}] = 0.5f;
    FlightInputCollector fic2;
    fic2.setClock(t);
    fic2.setBindings(b);
    auto r2 = fic2.poll(bridge, cam, console, kbOnly(inp));
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
    b.clear(fl::InputAction::PitchAxis); // no axis binding at all
    fic.setBindings(b);

    auto r = fic.poll(bridge, cam, console, kbOnly(inp));
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
    auto r_default = fic_default.poll(bridge, cam, console, kbOnly(inp));
    REQUIRE(r_default.has_value());

    fl::AxisConfigTable axes;
    {
        const fl::AxisKey k{fl::BindingSource::GamepadAxis, static_cast<uint32_t>(GamepadAxis::RightY), {}};
        fl::AxisConfig c = axes.effective(k);
        c.scale = 2.0f;
        axes.set(k, c);
    }
    FlightInputCollector fic_scaled;
    fic_scaled.setClock(t);
    fic_scaled.setAxisConfig(axes);
    auto r_scaled = fic_scaled.poll(bridge, cam, console, kbOnly(inp));
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
    b.set(fl::InputAction::FireWeapon, std::vector<fl::Binding>{padAxisBinding(GamepadAxis::TriggerRight)});
    fic.setBindings(b);

    inp.axisValues[{0, GamepadAxis::TriggerRight}] = 0.6f;
    auto r = fic.poll(bridge, cam, console, kbOnly(inp));
    REQUIRE(r.has_value());
    CHECK((r->buttons & 1u) != 0u);
    CHECK(fic.wasWeaponFired());

    inp.axisValues[{0, GamepadAxis::TriggerRight}] = 0.4f;
    FlightInputCollector fic2;
    fic2.setClock(t);
    fic2.setBindings(b);
    auto r2 = fic2.poll(bridge, cam, console, kbOnly(inp));
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
          std::vector<fl::Binding>{padAxisBinding(GamepadAxis::LeftY, /*negative=*/true)});
    fic.setBindings(b);

    inp.axisValues[{0, GamepadAxis::LeftY}] = -0.6f;
    auto r = fic.poll(bridge, cam, console, kbOnly(inp));
    REQUIRE(r.has_value());
    CHECK((r->buttons & 2u) != 0u);

    inp.axisValues[{0, GamepadAxis::LeftY}] = -0.4f;
    FlightInputCollector fic2;
    fic2.setClock(t);
    fic2.setBindings(b);
    auto r2 = fic2.poll(bridge, cam, console, kbOnly(inp));
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
    b.clear(fl::InputAction::FireWeapon);
    fic.setBindings(b);

    auto r = fic.poll(bridge, cam, console, kbOnly(inp));
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

    auto r = fic.poll(bridge, cam, console, kbOnly(inp));
    REQUIRE(r.has_value());
    CHECK((r->buttons & 0x04u) != 0u);
    CHECK(r->selectedStation == 255u); // no station count set: selection stays "keep"

    inp.held.clear();
    inp.mouseDown.insert(MouseButton::Right);
    t.advance(std::chrono::milliseconds(17));
    auto r2 = fic.poll(bridge, cam, console, kbOnly(inp));
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
    auto r = fic.poll(bridge, cam, console, kbOnly(inp));
    REQUIRE(r.has_value());
    CHECK(r->selectedStation == 0u);

    // Held across the next poll: no new edge, no further cycling.
    t.advance(std::chrono::milliseconds(17));
    r = fic.poll(bridge, cam, console, kbOnly(inp));
    REQUIRE(r.has_value());
    CHECK(r->selectedStation == 0u);

    // Release + press again: 0 -> 1, then 1 -> 2.
    inp.held.clear();
    t.advance(std::chrono::milliseconds(17));
    fic.poll(bridge, cam, console, kbOnly(inp));
    inp.held.insert(Key::Num1);
    t.advance(std::chrono::milliseconds(17));
    r = fic.poll(bridge, cam, console, kbOnly(inp));
    CHECK(r->selectedStation == 1u);
    inp.held.clear();
    t.advance(std::chrono::milliseconds(17));
    fic.poll(bridge, cam, console, kbOnly(inp));
    inp.held.insert(Key::Num1);
    t.advance(std::chrono::milliseconds(17));
    r = fic.poll(bridge, cam, console, kbOnly(inp));
    CHECK(r->selectedStation == 2u);

    // Prev wraps: 2 -> 1 -> 0 -> 2 would take three presses; go straight around from 2 via Next.
    inp.held.clear();
    t.advance(std::chrono::milliseconds(17));
    fic.poll(bridge, cam, console, kbOnly(inp));
    inp.held.insert(Key::Num1);
    t.advance(std::chrono::milliseconds(17));
    r = fic.poll(bridge, cam, console, kbOnly(inp));
    CHECK(r->selectedStation == 0u); // Next wrapped 2 -> 0

    // And Prev wraps the other way: 0 -> 2.
    inp.held.clear();
    t.advance(std::chrono::milliseconds(17));
    fic.poll(bridge, cam, console, kbOnly(inp));
    inp.held.insert(Key::Num2);
    t.advance(std::chrono::milliseconds(17));
    r = fic.poll(bridge, cam, console, kbOnly(inp));
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
    auto r = fic.poll(bridge, cam, console, kbOnly(inp), /*uiFocused=*/true);
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
    const fl::Binding fireB = padBindingOf(b, fl::InputAction::FireStore);
    const fl::Binding nextB = padBindingOf(b, fl::InputAction::NextWeapon);
    REQUIRE(fireB.source == fl::BindingSource::GamepadButton);
    REQUIRE(nextB.source == fl::BindingSource::GamepadButton);
    CHECK(static_cast<GamepadButton>(nextB.id) == GamepadButton::DpadRight);

    inp.gpDown.insert({0, static_cast<GamepadButton>(fireB.id)});
    inp.gpDown.insert({0, static_cast<GamepadButton>(nextB.id)});
    auto r = fic.poll(bridge, cam, console, kbOnly(inp));
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
    auto r = fic.poll(bridge, cam, console, kbOnly(inp));
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
    auto first = fic.poll(bridge, cam, console, kbOnly(inp));
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

    auto first = fic.poll(bridge, cam, console, kbOnly(inp));
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
    (void)fic.poll(bridge, cam, console, kbOnly(inp));

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
    (void)fic.poll(bridge, cam, console, kbOnly(inp));

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
    b.set(fl::InputAction::MasterArm, std::vector<fl::Binding>{keyBinding(Key::Num8)});
    fic.setBindings(b);
    REQUIRE(fic.masterArm());

    // The DEFAULT key no longer does anything once the action has been rebound.
    inp.held.insert(Key::Num4);
    t.advance(std::chrono::milliseconds(17));
    REQUIRE(fic.poll(bridge, cam, console, kbOnly(inp)).has_value());
    CHECK(fic.masterArm());
    inp.held.erase(Key::Num4);

    // The rebound key does.
    inp.held.insert(Key::Num8);
    t.advance(std::chrono::milliseconds(17));
    REQUIRE(fic.poll(bridge, cam, console, kbOnly(inp)).has_value());
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
    auto r1 = fic.poll(bridge, cam, console, kbOnly(inp));
    REQUIRE(r1.has_value());
    CHECK((r1->buttons & fl::kInputButtonGun) != 0u);

    inp.held.erase(Key::Space);
    inp.mouseDown.insert(MouseButton::Left);
    t.advance(std::chrono::milliseconds(17));
    auto r2 = fic.poll(bridge, cam, console, kbOnly(inp));
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
        b.set(r.action, std::vector<fl::Binding>{keyBinding(r.key)});
    fic.setBindings(b);

    auto pump = [&]() {
        t.advance(std::chrono::milliseconds(17));
        auto r = fic.poll(bridge, cam, console, kbOnly(inp));
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

TEST_CASE("FlightInputCollector: uiFocused leaves the gun and the flight controls live (#610)",
          "[flight_input][bindings]") {
    // The radio menu is non-modal on purpose. It gates the discretes it consumes; it must not safe
    // the guns or idle the engine. Previously the keyboard gun was live under uiFocused but the
    // GAMEPAD gun was gated — an asymmetry the docs did not promise and #1050's unified resolution
    // removes.
    MockLogger log;
    CommandRegistry reg;
    GameConsole console(log, reg);
    MockInput inp;
    inp.gamepadCount = 1;
    inp.held.insert(Key::Space);                          // gun, keyboard
    inp.gpDown.insert({0, GamepadButton::RightShoulder}); // gun, gamepad
    inp.held.insert(Key::Tab);                            // afterburner
    inp.held.insert(Key::B);                              // wheel brakes
    CameraInput cam;
    fl::SimRenderBridge bridge;
    FlightInputCollector fic;
    fl::ManualClock t;
    fic.setClock(t);

    auto r = fic.poll(bridge, cam, console, kbOnly(inp), /*uiFocused=*/true);
    REQUIRE(r.has_value());
    CHECK((r->buttons & fl::kInputButtonGun) != 0u);
    CHECK(fic.wasWeaponFired());
    CHECK((r->buttons & fl::kInputButtonAfterburner) != 0u);
    CHECK((r->buttons & fl::kInputButtonWheelBrake) != 0u);
}

TEST_CASE("FlightInputCollector: textEntry suppresses keyboard and mouse, never the stick (#646)",
          "[flight_input][bindings]") {
    // The chat box owns the keyboard AND the pointer that clicks its Send button; it does not own the
    // stick, so a partner keeps flying while you type. The suppression is per binding SOURCE (#1050),
    // not per input block, so an action bound to both a key and a pad button keeps working from the pad.
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
    cam.setThrottle(0.6f);

    // Space and the left mouse button are both FireWeapon; the pad shoulder is too.
    inp.held.insert(Key::Space);
    inp.mouseDown.insert(MouseButton::Left);
    inp.held.insert(Key::ArrowUp);                   // elevator
    inp.held.insert(Key::PageUp);                    // throttle up
    inp.axisValues[{0, GamepadAxis::RightY}] = 0.8f; // pad elevator

    auto r = fic.poll(bridge, cam, console, kbOnly(inp), /*uiFocused=*/false, /*textEntry=*/true);
    REQUIRE(r.has_value());
    // Typing must not fire the gun, and clicking Send must not either.
    CHECK((r->buttons & fl::kInputButtonGun) == 0u);
    CHECK_FALSE(fic.wasWeaponFired());
    // The throttle HOLDS at its last value rather than being driven or zeroed.
    CHECK(r->throttle == Catch::Approx(0.6f));
    // The pad axis is still flying the aircraft (0.8 raw through the default 0.1 deadzone rescale).
    CHECK(r->elevator == Catch::Approx((0.8f - 0.1f) / (1.0f - 0.1f)));

    // The pad's own gun button is likewise unaffected.
    inp.gpDown.insert({0, GamepadButton::RightShoulder});
    t.advance(std::chrono::milliseconds(17));
    auto r2 = fic.poll(bridge, cam, console, kbOnly(inp), /*uiFocused=*/false, /*textEntry=*/true);
    REQUIRE(r2.has_value());
    CHECK((r2->buttons & fl::kInputButtonGun) != 0u);
}
