// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "LoadingScreen.h"
#include "mock_hal.h"

#include <atomic>
#include <chrono>

static MockInput g_inp;
static MockWindow g_win;

TEST_CASE("LoadingScreen: stays on Loading while server not ready") {
    std::atomic<bool> ready{false};
    bool connected = false;
    bool onReadyCalled = false;
    LoadingScreen s(ready, [&] { return connected; }, [&] { onReadyCalled = true; });

    Screen next = s.update(g_inp, g_win);
    CHECK(next == Screen::Loading);
    CHECK(!onReadyCalled);
}

TEST_CASE("LoadingScreen: calls onServerReady once when serverReady fires") {
    std::atomic<bool> ready{false};
    bool connected = false;
    int onReadyCount = 0;
    LoadingScreen s(ready, [&] { return connected; }, [&] { ++onReadyCount; });

    s.update(g_inp, g_win); // StartingServer
    ready.store(true);
    s.update(g_inp, g_win); // triggers onServerReady → Connecting
    s.update(g_inp, g_win); // Connecting (still not connected)
    CHECK(onReadyCount == 1);
}

TEST_CASE("LoadingScreen: stays Connecting while isConnected returns false") {
    std::atomic<bool> ready{true};
    bool connected = false;
    LoadingScreen s(ready, [&] { return connected; }, [] {});

    s.update(g_inp, g_win); // → Connecting (serverReady already true)
    Screen next = s.update(g_inp, g_win);
    CHECK(next == Screen::Loading);
}

TEST_CASE("LoadingScreen: transitions to Flight after isConnected returns true") {
    std::atomic<bool> ready{true};
    bool connected = false;
    LoadingScreen s(ready, [&] { return connected; }, [] {});

    s.update(g_inp, g_win); // → Connecting
    connected = true;
    s.update(g_inp, g_win);               // → Ready
    Screen next = s.update(g_inp, g_win); // emits Screen::Flight
    CHECK(next == Screen::Flight);
}

TEST_CASE("LoadingScreen: buildElements not empty while in progress") {
    std::atomic<bool> ready{false};
    LoadingScreen s(ready, [] { return false; }, [] {});
    s.update(g_inp, g_win);
    CHECK(!s.buildElements().empty());
}

TEST_CASE("LoadingScreen: reset allows reuse for a second session") {
    std::atomic<bool> ready{true};
    bool connected = true;
    int onReadyCount = 0;
    LoadingScreen s(ready, [&] { return connected; }, [&] { ++onReadyCount; });

    // First session: run to Flight
    s.update(g_inp, g_win);
    s.update(g_inp, g_win);
    s.update(g_inp, g_win);
    CHECK(onReadyCount == 1);

    // Reset for second session
    ready.store(false);
    connected = false;
    s.reset();
    s.update(g_inp, g_win); // StartingServer
    ready.store(true);
    s.update(g_inp, g_win); // fires onServerReady again
    CHECK(onReadyCount == 2);
}
