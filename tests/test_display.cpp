// SPDX-License-Identifier: GPL-3.0-or-later
#include "SDL3Display.h"
#include "SDL3Window.h"
#include "mock_hal.h"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>

using namespace fl;

// ---------------------------------------------------------------------------
// MockDisplay -- pure interface contract tests (no SDL3 required)
// ---------------------------------------------------------------------------

TEST_CASE("MockDisplay getMonitorCount returns configured count", "[display]") {
    MockDisplay d;
    d.monitorCount = 3;
    CHECK(d.getMonitorCount() == 3);
}

TEST_CASE("MockDisplay getMonitorName returns non-null for valid id", "[display]") {
    MockDisplay d;
    d.monitorCount = 2;
    CHECK(d.getMonitorName(0) != nullptr);
    CHECK(d.getMonitorName(1) != nullptr);
}

TEST_CASE("MockDisplay getMonitorName returns null for out-of-range id", "[display]") {
    MockDisplay d;
    d.monitorCount = 1;
    CHECK(d.getMonitorName(1) == nullptr);
    CHECK(d.getMonitorName(-1) == nullptr);
}

TEST_CASE("MockDisplay listModes returns configured modes", "[display]") {
    MockDisplay d;
    d.modes = {{0, 1920, 1080, 60.0f}, {0, 2560, 1440, 144.0f}};
    auto modes = d.listModes(0);
    REQUIRE(modes.size() == 2);
    CHECK(modes[0].width == 1920);
    CHECK(modes[0].height == 1080);
    CHECK(modes[1].width == 2560);
    CHECK(modes[1].refreshRate == Catch::Approx(144.0f));
}

TEST_CASE("MockDisplay getRefreshRate returns configured rate", "[display]") {
    MockDisplay d;
    d.monitorCount = 2;
    d.mockRefreshRate = 120.0f;
    CHECK(d.getRefreshRate(0) == Catch::Approx(120.0f));
    CHECK(d.getRefreshRate(1) == Catch::Approx(120.0f));
}

TEST_CASE("MockDisplay getRefreshRate returns 0 for out-of-range id", "[display]") {
    MockDisplay d;
    d.monitorCount = 1;
    CHECK(d.getRefreshRate(1) == Catch::Approx(0.0f));
    CHECK(d.getRefreshRate(-1) == Catch::Approx(0.0f));
}

TEST_CASE("MockDisplay getLastError returns null initially", "[display]") {
    MockDisplay d;
    CHECK(d.getLastError() == nullptr);
}

// ---------------------------------------------------------------------------
// SDL3Display / SDL3Window -- headless integration tests
// ---------------------------------------------------------------------------

static void useHeadlessDriver() {
#ifdef _WIN32
    _putenv_s("SDL_VIDEO_DRIVER", "dummy");
#else
    setenv("SDL_VIDEO_DRIVER", "dummy", 1);
#endif
}

TEST_CASE("SDL3Display getMonitorCount returns at least 1 (headless)", "[display][sdl3]") {
    useHeadlessDriver();
    SDL3Window window;
    if (!window.init("display-test", 64, 64))
        SKIP("SDL3 headless init failed");
    SDL3Display display;
    CHECK(display.getMonitorCount() >= 1);
    window.shutdown();
}

TEST_CASE("SDL3Display getMonitorName returns non-null for monitor 0 (headless)", "[display][sdl3]") {
    useHeadlessDriver();
    SDL3Window window;
    if (!window.init("display-test", 64, 64))
        SKIP("SDL3 headless init failed");
    SDL3Display display;
    if (display.getMonitorCount() < 1)
        SKIP("no monitors available in headless driver");
    CHECK(display.getMonitorName(0) != nullptr);
    window.shutdown();
}

TEST_CASE("SDL3Display getMonitorName returns null for out-of-range id (headless)", "[display][sdl3]") {
    useHeadlessDriver();
    SDL3Window window;
    if (!window.init("display-test", 64, 64))
        SKIP("SDL3 headless init failed");
    SDL3Display display;
    int count = display.getMonitorCount();
    CHECK(display.getMonitorName(count) == nullptr);
    CHECK(display.getMonitorName(-1) == nullptr);
    window.shutdown();
}

TEST_CASE("SDL3Display listModes does not crash (headless)", "[display][sdl3]") {
    useHeadlessDriver();
    SDL3Window window;
    if (!window.init("display-test", 64, 64))
        SKIP("SDL3 headless init failed");
    SDL3Display display;
    if (display.getMonitorCount() < 1)
        SKIP("no monitors available in headless driver");
    // Under the dummy driver listModes may return empty -- that is valid.
    auto modes = display.listModes(0);
    (void)modes;
    window.shutdown();
}

TEST_CASE("SDL3Window getCurrentMonitorId returns -1 before init", "[display][sdl3]") {
    SDL3Window window;
    CHECK(window.getCurrentMonitorId() == -1);
}

TEST_CASE("SDL3Window getCurrentMonitorId does not crash after init (headless)", "[display][sdl3]") {
    useHeadlessDriver();
    SDL3Window window;
    if (!window.init("display-test", 64, 64))
        SKIP("SDL3 headless init failed");
    // Dummy driver may return -1 or a valid index -- both are acceptable.
    int id = window.getCurrentMonitorId();
    CHECK(id >= -1);
    window.shutdown();
}

TEST_CASE("SDL3Window setTitle does not crash (headless)", "[display][sdl3]") {
    useHeadlessDriver();
    SDL3Window window;
    if (!window.init("initial title", 64, 64))
        SKIP("SDL3 headless init failed");
    window.setTitle("updated title");
    window.shutdown();
}

TEST_CASE("SDL3Window setFullscreen does not crash (headless)", "[display][sdl3]") {
    useHeadlessDriver();
    SDL3Window window;
    if (!window.init("fs-test", 64, 64))
        SKIP("SDL3 headless init failed");
    // Under the dummy driver fullscreen may fail gracefully.
    window.setFullscreen(true);
    window.setFullscreen(false);
    window.shutdown();
}
