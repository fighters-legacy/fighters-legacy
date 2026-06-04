// SPDX-License-Identifier: GPL-3.0-or-later
#include "IWindowEventHandler.h"
#include "SDL3Window.h"
#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>

static void useHeadlessDriver() {
#ifdef _WIN32
    _putenv_s("SDL_VIDEO_DRIVER", "dummy");
#else
    setenv("SDL_VIDEO_DRIVER", "dummy", 1);
#endif
}

TEST_CASE("SDL3Window init and shutdown (headless)", "[sdl3]") {
    useHeadlessDriver();
    SDL3Window window;
    if (!window.init("smoke", 64, 64))
        SKIP("SDL3 headless init failed");
    CHECK(window.getLastError() == nullptr);
    CHECK(window.width() == 64);
    CHECK(window.height() == 64);
    CHECK(window.logicalWidth() == 64);
    CHECK(window.logicalHeight() == 64);
    window.shutdown();
    window.shutdown(); // idempotent
}

namespace {
struct ResizeCounter : IWindowEventHandler {
    int count{0};
    int lastW{0};
    int lastH{0};
    void onResize(int w, int h) override {
        ++count;
        lastW = w;
        lastH = h;
    }
    void onClose() override {}
};

static SDL_Event makeWindowEvent(Uint32 type, SDL_WindowID id, int data1, int data2) {
    SDL_Event ev{};
    ev.type = type;
    ev.window.windowID = id;
    ev.window.data1 = data1;
    ev.window.data2 = data2;
    return ev;
}
} // namespace

TEST_CASE("SDL3Window WINDOW_RESIZED updates logical size only", "[sdl3]") {
    useHeadlessDriver();
    SDL3Window window;
    if (!window.init("resize-logical", 64, 64))
        SKIP("SDL3 headless init failed");

    auto* sdlWin = static_cast<SDL_Window*>(window.nativeHandle());
    SDL_WindowID id = SDL_GetWindowID(sdlWin);

    SDL_Event ev = makeWindowEvent(SDL_EVENT_WINDOW_RESIZED, id, 200, 150);
    SDL_PushEvent(&ev);
    window.pollEvents();

    CHECK(window.logicalWidth() == 200);
    CHECK(window.logicalHeight() == 150);
    CHECK(window.width() == 64);
    CHECK(window.height() == 64);
    window.shutdown();
}

TEST_CASE("SDL3Window PIXEL_SIZE_CHANGED updates physical size only", "[sdl3]") {
    useHeadlessDriver();
    SDL3Window window;
    if (!window.init("resize-physical", 64, 64))
        SKIP("SDL3 headless init failed");

    auto* sdlWin = static_cast<SDL_Window*>(window.nativeHandle());
    SDL_WindowID id = SDL_GetWindowID(sdlWin);

    SDL_Event ev = makeWindowEvent(SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED, id, 400, 300);
    SDL_PushEvent(&ev);
    window.pollEvents();

    CHECK(window.width() == 400);
    CHECK(window.height() == 300);
    CHECK(window.logicalWidth() == 64);
    CHECK(window.logicalHeight() == 64);
    window.shutdown();
}

TEST_CASE("SDL3Window PIXEL_SIZE_CHANGED calls onResize; WINDOW_RESIZED does not", "[sdl3]") {
    useHeadlessDriver();
    SDL3Window window;
    if (!window.init("resize-handler", 64, 64))
        SKIP("SDL3 headless init failed");

    ResizeCounter counter;
    window.setEventHandler(&counter);

    auto* sdlWin = static_cast<SDL_Window*>(window.nativeHandle());
    SDL_WindowID id = SDL_GetWindowID(sdlWin);

    SDL_Event logicalEv = makeWindowEvent(SDL_EVENT_WINDOW_RESIZED, id, 200, 150);
    SDL_PushEvent(&logicalEv);
    window.pollEvents();
    CHECK(counter.count == 0);

    SDL_Event physicalEv = makeWindowEvent(SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED, id, 400, 300);
    SDL_PushEvent(&physicalEv);
    window.pollEvents();
    CHECK(counter.count == 1);
    CHECK(counter.lastW == 400);
    CHECK(counter.lastH == 300);

    window.shutdown();
}

TEST_CASE("SDL3Window shouldClose is false after init", "[sdl3]") {
    useHeadlessDriver();
    SDL3Window window;
    if (!window.init("smoke", 64, 64))
        SKIP("SDL3 headless init failed");
    CHECK(!window.shouldClose());
    window.shutdown();
}

TEST_CASE("SDL3Window pollEvents does not crash (headless)", "[sdl3]") {
    useHeadlessDriver();
    SDL3Window window;
    if (!window.init("smoke", 64, 64))
        SKIP("SDL3 headless init failed");
    window.pollEvents();
    CHECK(window.getLastError() == nullptr);
    window.shutdown();
}
