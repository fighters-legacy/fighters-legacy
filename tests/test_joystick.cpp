// SPDX-License-Identifier: GPL-3.0-or-later
#include "SDL3Joystick.h"
#include "SDL3Window.h"
#include "mock_hal.h"
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>

using namespace fl;

// ---------------------------------------------------------------------------
// MockJoystick -- pure interface contract tests (no SDL3 required)
// ---------------------------------------------------------------------------

TEST_CASE("MockJoystick getJoystickCount returns configured count", "[joystick]") {
    MockJoystick j;
    j.count = 2;
    CHECK(j.getJoystickCount() == 2);
}

TEST_CASE("MockJoystick getJoystickName returns non-null for valid id", "[joystick]") {
    MockJoystick j;
    j.count = 1;
    CHECK(j.getJoystickName(0) != nullptr);
}

TEST_CASE("MockJoystick getJoystickGuid returns non-null for valid id", "[joystick]") {
    MockJoystick j;
    j.count = 1;
    CHECK(j.getJoystickGuid(0) != nullptr);
}

TEST_CASE("MockJoystick getAxisValue returns 0 by default", "[joystick]") {
    MockJoystick j;
    CHECK(j.getAxisValue(0, 0) == 0.0f);
}

TEST_CASE("MockJoystick getHatPosition returns Centered by default", "[joystick]") {
    MockJoystick j;
    CHECK(j.getHatPosition(0, 0) == HatPosition::Centered);
}

TEST_CASE("MockJoystick isButtonDown returns false by default", "[joystick]") {
    MockJoystick j;
    CHECK_FALSE(j.isButtonDown(0, 0));
}

TEST_CASE("MockJoystick isButtonJustPressed returns false by default", "[joystick]") {
    MockJoystick j;
    CHECK_FALSE(j.isButtonJustPressed(0, 0));
}

// ---------------------------------------------------------------------------
// SDL3Joystick / SDL3Window -- headless integration tests
// ---------------------------------------------------------------------------

static void useHeadlessDriver() {
#ifdef _WIN32
    _putenv_s("SDL_VIDEO_DRIVER", "dummy");
#else
    setenv("SDL_VIDEO_DRIVER", "dummy", 1);
#endif
}

TEST_CASE("SDL3Joystick getJoystickCount does not crash (headless)", "[joystick][sdl3]") {
    useHeadlessDriver();
    SDL3Window window;
    if (!window.init("joystick-test", 64, 64))
        SKIP("SDL3 headless init failed");
    SDL3Joystick joystick;
    CHECK(joystick.getJoystickCount() >= 0);
    window.shutdown();
}

TEST_CASE("SDL3Joystick getLastError returns null on clean init (headless)", "[joystick][sdl3]") {
    useHeadlessDriver();
    SDL3Window window;
    if (!window.init("joystick-test", 64, 64))
        SKIP("SDL3 headless init failed");
    SDL3Joystick joystick;
    CHECK(joystick.getLastError() == nullptr);
    window.shutdown();
}

TEST_CASE("SDL3Joystick flush does not crash with no devices (headless)", "[joystick][sdl3]") {
    useHeadlessDriver();
    SDL3Window window;
    if (!window.init("joystick-test", 64, 64))
        SKIP("SDL3 headless init failed");
    SDL3Joystick joystick;
    joystick.flush();
    CHECK(joystick.getJoystickCount() == 0);
    window.shutdown();
}
