// SPDX-License-Identifier: GPL-3.0-or-later
#include "SDL3Cursor.h"
#include "SDL3Window.h"
#include "mock_hal.h"
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>

// ---------------------------------------------------------------------------
// MockCursor -- pure interface contract tests (no SDL3 required)
// ---------------------------------------------------------------------------

TEST_CASE("MockCursor setCursor records last shape", "[cursor]") {
    MockCursor c;
    CHECK(c.lastShape == CursorShape::Arrow);
    c.setCursor(CursorShape::Hand);
    CHECK(c.lastShape == CursorShape::Hand);
    c.setCursor(CursorShape::None);
    CHECK(c.lastShape == CursorShape::None);
}

TEST_CASE("MockCursor getLastError returns null initially", "[cursor]") {
    MockCursor c;
    CHECK(c.getLastError() == nullptr);
}

TEST_CASE("MockCursor setCustomCursor records call", "[cursor]") {
    MockCursor c;
    CHECK(!c.customCursorSet);
    uint8_t pixels[4] = {255, 0, 0, 255};
    c.setCustomCursor(pixels, 1, 1, 0, 0);
    CHECK(c.customCursorSet);
}

// ---------------------------------------------------------------------------
// SDL3Cursor / SDL3Window -- headless integration tests
// ---------------------------------------------------------------------------

static void useHeadlessDriver() {
#ifdef _WIN32
    _putenv_s("SDL_VIDEO_DRIVER", "dummy");
#else
    setenv("SDL_VIDEO_DRIVER", "dummy", 1);
#endif
}

TEST_CASE("SDL3Cursor setCursor Arrow does not crash (headless)", "[cursor][sdl3]") {
    useHeadlessDriver();
    SDL3Window window;
    if (!window.init("cursor-test", 64, 64))
        SKIP("SDL3 headless init failed");
    SDL3Cursor cursor;
    cursor.setCursor(CursorShape::Arrow);
    window.shutdown();
}

TEST_CASE("SDL3Cursor setCursor None does not crash (headless)", "[cursor][sdl3]") {
    useHeadlessDriver();
    SDL3Window window;
    if (!window.init("cursor-test", 64, 64))
        SKIP("SDL3 headless init failed");
    SDL3Cursor cursor;
    cursor.setCursor(CursorShape::None);
    window.shutdown();
}

TEST_CASE("SDL3Cursor setCursor cycles all shapes without crash (headless)", "[cursor][sdl3]") {
    useHeadlessDriver();
    SDL3Window window;
    if (!window.init("cursor-test", 64, 64))
        SKIP("SDL3 headless init failed");
    SDL3Cursor cursor;
    cursor.setCursor(CursorShape::Arrow);
    cursor.setCursor(CursorShape::Hand);
    cursor.setCursor(CursorShape::Crosshair);
    cursor.setCursor(CursorShape::ResizeNS);
    cursor.setCursor(CursorShape::ResizeEW);
    cursor.setCursor(CursorShape::ResizeAll);
    cursor.setCursor(CursorShape::Text);
    cursor.setCursor(CursorShape::None);
    cursor.setCursor(CursorShape::Arrow);
    window.shutdown();
}

TEST_CASE("SDL3Cursor setCustomCursor with valid RGBA data does not crash (headless)", "[cursor][sdl3]") {
    useHeadlessDriver();
    SDL3Window window;
    if (!window.init("cursor-test", 64, 64))
        SKIP("SDL3 headless init failed");
    SDL3Cursor cursor;
    uint8_t pixels[4 * 4 * 4] = {};
    pixels[3] = 255; // make first pixel opaque red
    pixels[0] = 255;
    cursor.setCustomCursor(pixels, 4, 4, 0, 0);
    window.shutdown();
}

TEST_CASE("SDL3Cursor setCustomCursor with null pixels sets error (headless)", "[cursor][sdl3]") {
    useHeadlessDriver();
    SDL3Window window;
    if (!window.init("cursor-test", 64, 64))
        SKIP("SDL3 headless init failed");
    SDL3Cursor cursor;
    cursor.setCustomCursor(nullptr, 4, 4, 0, 0);
    CHECK(cursor.getLastError() != nullptr);
    window.shutdown();
}

TEST_CASE("SDL3Cursor destructor does not crash after setCursor (headless)", "[cursor][sdl3]") {
    useHeadlessDriver();
    SDL3Window window;
    if (!window.init("cursor-test", 64, 64))
        SKIP("SDL3 headless init failed");
    {
        SDL3Cursor cursor;
        cursor.setCursor(CursorShape::Hand);
    } // cursor destroyed here — SDL video still active
    window.shutdown();
}
