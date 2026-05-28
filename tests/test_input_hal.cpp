// SPDX-License-Identifier: GPL-3.0-or-later
#include "SDL3Input.h"
#include "SDL3Window.h"
#include "mock_hal.h"
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>

// ---------------------------------------------------------------------------
// MockInput -- pure interface contract tests (no SDL3 required)
// ---------------------------------------------------------------------------

TEST_CASE("MockInput supportsRumble returns false by default", "[input_hal]") {
    MockInput input;
    CHECK_FALSE(input.supportsRumble(0));
}

TEST_CASE("MockInput supportsRumble returns false for negative id", "[input_hal]") {
    MockInput input;
    CHECK_FALSE(input.supportsRumble(-1));
}

TEST_CASE("MockInput supportsTriggerRumble returns false by default", "[input_hal]") {
    MockInput input;
    CHECK_FALSE(input.supportsTriggerRumble(0));
}

TEST_CASE("MockInput supportsTriggerRumble returns false for negative id", "[input_hal]") {
    MockInput input;
    CHECK_FALSE(input.supportsTriggerRumble(-1));
}

TEST_CASE("MockInput stopRumble does not crash", "[input_hal]") {
    MockInput input;
    input.stopRumble(0);
    input.stopRumble(-1);
    SUCCEED();
}

// ---------------------------------------------------------------------------
// SDL3Input / SDL3Window -- headless integration tests
// ---------------------------------------------------------------------------

static void useHeadlessDriver() {
#ifdef _WIN32
    _putenv_s("SDL_VIDEO_DRIVER", "dummy");
#else
    setenv("SDL_VIDEO_DRIVER", "dummy", 1);
#endif
}

TEST_CASE("SDL3Input supportsRumble returns false for out-of-range id (headless)", "[input_hal][sdl3]") {
    useHeadlessDriver();
    SDL3Window window;
    if (!window.init("input-hal-test", 64, 64))
        SKIP("SDL3 headless init failed");
    SDL3Input input;
    window.setInputSink(&input);
    CHECK_FALSE(input.supportsRumble(-1));
    CHECK_FALSE(input.supportsRumble(99));
    window.shutdown();
}

TEST_CASE("SDL3Input supportsRumble returns false when no gamepad connected (headless)", "[input_hal][sdl3]") {
    useHeadlessDriver();
    SDL3Window window;
    if (!window.init("input-hal-test", 64, 64))
        SKIP("SDL3 headless init failed");
    SDL3Input input;
    window.setInputSink(&input);
    CHECK(input.getGamepadCount() == 0);
    CHECK_FALSE(input.supportsRumble(0));
    window.shutdown();
}

TEST_CASE("SDL3Input supportsTriggerRumble returns false for out-of-range id (headless)", "[input_hal][sdl3]") {
    useHeadlessDriver();
    SDL3Window window;
    if (!window.init("input-hal-test", 64, 64))
        SKIP("SDL3 headless init failed");
    SDL3Input input;
    window.setInputSink(&input);
    CHECK_FALSE(input.supportsTriggerRumble(-1));
    CHECK_FALSE(input.supportsTriggerRumble(99));
    window.shutdown();
}

TEST_CASE("SDL3Input stopRumble does not crash for out-of-range id (headless)", "[input_hal][sdl3]") {
    useHeadlessDriver();
    SDL3Window window;
    if (!window.init("input-hal-test", 64, 64))
        SKIP("SDL3 headless init failed");
    SDL3Input input;
    window.setInputSink(&input);
    input.stopRumble(-1);
    input.stopRumble(99);
    SUCCEED();
    window.shutdown();
}

TEST_CASE("SDL3Input stopRumble does not crash when no gamepad connected (headless)", "[input_hal][sdl3]") {
    useHeadlessDriver();
    SDL3Window window;
    if (!window.init("input-hal-test", 64, 64))
        SKIP("SDL3 headless init failed");
    SDL3Input input;
    window.setInputSink(&input);
    CHECK(input.getGamepadCount() == 0);
    input.stopRumble(0);
    SUCCEED();
    window.shutdown();
}
