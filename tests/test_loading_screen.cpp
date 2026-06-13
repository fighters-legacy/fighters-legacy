// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "LoadingScreen.h"
#include "mock_hal.h"

#include <atomic>

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

TEST_CASE("LoadingScreen: multiplayer mode shows remote connect message") {
    std::atomic<bool> ready{false};
    LoadingScreen s(ready, [] { return false; }, [] {}, /*isSinglePlayer=*/false);
    s.update(g_inp, g_win);
    auto elems = s.buildElements();
    bool found = false;
    for (const auto& el : elems) {
        if (el.type == HudElement::Type::Text && el.text.find("remote server") != std::string_view::npos) {
            found = true;
            break;
        }
    }
    CHECK(found);
}

TEST_CASE("LoadingScreen: connect timeout trips Failed phase then returns MainMenu") {
    using clk = std::chrono::steady_clock;
    clk::time_point fakeNow = clk::now();

    std::atomic<bool> ready{true};
    LoadingScreen s(ready, [] { return false; }, [] {});
    s.setClockOverride([&] { return fakeNow; });

    s.update(g_inp, g_win); // StartingServer → Connecting; deadline = fakeNow + 10s

    CHECK(s.update(g_inp, g_win) == Screen::Loading); // within timeout

    fakeNow += std::chrono::seconds(11);              // past connect deadline
    CHECK(s.update(g_inp, g_win) == Screen::Loading); // → Failed; within kFailDisplaySeconds

    bool foundMsg = false;
    for (const auto& el : s.buildElements())
        if (el.type == HudElement::Type::Text && el.text.find("timed out") != std::string::npos)
            foundMsg = true;
    CHECK(foundMsg);

    fakeNow += std::chrono::seconds(4); // past kFailDisplaySeconds (3s)
    CHECK(s.update(g_inp, g_win) == Screen::MainMenu);
}

TEST_CASE("LoadingScreen: startup timeout trips Failed phase then returns MainMenu") {
    using clk = std::chrono::steady_clock;
    clk::time_point fakeNow = clk::now();

    std::atomic<bool> ready{false}; // server never starts
    bool onReadyCalled = false;
    LoadingScreen s(ready, [] { return false; }, [&] { onReadyCalled = true; });
    s.setClockOverride([&] { return fakeNow; });

    s.update(g_inp, g_win); // StartingServer; deadline = fakeNow + 10s

    CHECK(s.update(g_inp, g_win) == Screen::Loading); // within timeout
    CHECK(!onReadyCalled);

    fakeNow += std::chrono::seconds(11);              // past startup deadline
    CHECK(s.update(g_inp, g_win) == Screen::Loading); // → Failed; within kFailDisplaySeconds
    CHECK(!onReadyCalled);

    bool foundMsg = false;
    for (const auto& el : s.buildElements())
        if (el.type == HudElement::Type::Text && el.text.find("failed to start") != std::string::npos)
            foundMsg = true;
    CHECK(foundMsg);

    fakeNow += std::chrono::seconds(4); // past kFailDisplaySeconds (3s)
    CHECK(s.update(g_inp, g_win) == Screen::MainMenu);
}

TEST_CASE("LoadingScreen: reset after startup timeout allows successful second session") {
    using clk = std::chrono::steady_clock;
    clk::time_point fakeNow = clk::now();

    std::atomic<bool> ready{false};
    int onReadyCount = 0;
    bool connected = false;
    LoadingScreen s(ready, [&] { return connected; }, [&] { ++onReadyCount; });
    s.setClockOverride([&] { return fakeNow; });

    // First session: startup timeout fires.
    s.update(g_inp, g_win);
    fakeNow += std::chrono::seconds(11);
    s.update(g_inp, g_win); // → Failed
    CHECK(onReadyCount == 0);

    // Reset for second session.
    ready.store(false);
    s.reset();

    // Second session: server starts and connects successfully.
    s.update(g_inp, g_win); // StartingServer; fresh deadline
    ready.store(true);
    s.update(g_inp, g_win); // → Connecting; onServerReady fires
    CHECK(onReadyCount == 1);
    connected = true;
    s.update(g_inp, g_win); // → Ready
    CHECK(s.update(g_inp, g_win) == Screen::Flight);
}

TEST_CASE("LoadingScreen: reset clears startup deadline so new session gets fresh timeout") {
    using clk = std::chrono::steady_clock;
    clk::time_point fakeNow = clk::now();

    std::atomic<bool> ready{false};
    LoadingScreen s(ready, [] { return false; }, [] {});
    s.setClockOverride([&] { return fakeNow; });

    s.update(g_inp, g_win); // sets start deadline at fakeNow

    fakeNow += std::chrono::seconds(5); // halfway through first session
    s.reset();                          // clears both deadlines

    // After reset the next update sets a fresh deadline from current fakeNow.
    s.update(g_inp, g_win);

    fakeNow += std::chrono::seconds(6);               // only 6s into NEW deadline (< 10s)
    CHECK(s.update(g_inp, g_win) == Screen::Loading); // must NOT have timed out
}

TEST_CASE("LoadingScreen: spawn fail message shown immediately without timeout") {
    using clk = std::chrono::steady_clock;
    clk::time_point fakeNow = clk::now();

    std::atomic<bool> ready{false};
    const char* failMsg = nullptr;
    LoadingScreen s(ready, [] { return false; }, [] {}, /*isSinglePlayer=*/true, [&] { return failMsg; });
    s.setClockOverride([&] { return fakeNow; });

    s.update(g_inp, g_win); // sets start deadline; failMsg still null

    failMsg = "Server binary not found.";
    CHECK(s.update(g_inp, g_win) == Screen::Loading); // -> Failed, within kFailDisplaySeconds

    bool found = false;
    for (const auto& el : s.buildElements())
        if (el.type == HudElement::Type::Text && el.text.find("not found") != std::string::npos)
            found = true;
    CHECK(found);

    fakeNow += std::chrono::seconds(4); // past kFailDisplaySeconds (3 s)
    CHECK(s.update(g_inp, g_win) == Screen::MainMenu);
}

TEST_CASE("LoadingScreen: bind fail message shown immediately without timeout") {
    using clk = std::chrono::steady_clock;
    clk::time_point fakeNow = clk::now();

    std::atomic<bool> ready{false};
    const char* failMsg = nullptr;
    LoadingScreen s(ready, [] { return false; }, [] {}, /*isSinglePlayer=*/true, [&] { return failMsg; });
    s.setClockOverride([&] { return fakeNow; });

    s.update(g_inp, g_win); // sets start deadline

    failMsg = "Port already in use.";
    CHECK(s.update(g_inp, g_win) == Screen::Loading); // -> Failed, within kFailDisplaySeconds

    bool found = false;
    for (const auto& el : s.buildElements())
        if (el.type == HudElement::Type::Text && el.text.find("already in use") != std::string::npos)
            found = true;
    CHECK(found);

    fakeNow += std::chrono::seconds(4);
    CHECK(s.update(g_inp, g_win) == Screen::MainMenu);
}

TEST_CASE("LoadingScreen: server timeout fail message shown immediately without timeout") {
    using clk = std::chrono::steady_clock;
    clk::time_point fakeNow = clk::now();

    std::atomic<bool> ready{false};
    const char* failMsg = nullptr;
    LoadingScreen s(ready, [] { return false; }, [] {}, /*isSinglePlayer=*/true, [&] { return failMsg; });
    s.setClockOverride([&] { return fakeNow; });

    s.update(g_inp, g_win); // sets start deadline

    failMsg = "Server startup timed out.";
    CHECK(s.update(g_inp, g_win) == Screen::Loading); // -> Failed, within kFailDisplaySeconds

    bool found = false;
    for (const auto& el : s.buildElements())
        if (el.type == HudElement::Type::Text && el.text.find("startup timed out") != std::string::npos)
            found = true;
    CHECK(found);

    fakeNow += std::chrono::seconds(4);
    CHECK(s.update(g_inp, g_win) == Screen::MainMenu);
}

TEST_CASE("LoadingScreen: fallback generic message shown on startup deadline with no fail msg") {
    using clk = std::chrono::steady_clock;
    clk::time_point fakeNow = clk::now();

    std::atomic<bool> ready{false};
    // No getStartFailMsg — simulates a hung start() that never returns.
    LoadingScreen s(ready, [] { return false; }, [] {});
    s.setClockOverride([&] { return fakeNow; });

    s.update(g_inp, g_win); // sets start deadline at fakeNow + 10 s

    fakeNow += std::chrono::seconds(11);              // past startup deadline
    CHECK(s.update(g_inp, g_win) == Screen::Loading); // -> Failed, within kFailDisplaySeconds

    bool found = false;
    for (const auto& el : s.buildElements())
        if (el.type == HudElement::Type::Text && el.text.find("failed to start") != std::string::npos)
            found = true;
    CHECK(found);

    fakeNow += std::chrono::seconds(4);
    CHECK(s.update(g_inp, g_win) == Screen::MainMenu);
}

TEST_CASE("LoadingScreen: reset preserves multiplayer messages") {
    std::atomic<bool> ready{true};
    bool connected = false;
    LoadingScreen s(ready, [&] { return connected; }, [] {}, /*isSinglePlayer=*/false);
    // Run one session to completion.
    s.update(g_inp, g_win);
    s.update(g_inp, g_win);
    ready.store(false);
    connected = false;
    s.reset();
    // After reset, initial elements should still reflect remote-connect text.
    auto elems = s.buildElements();
    bool found = false;
    for (const auto& el : elems)
        if (el.type == HudElement::Type::Text && el.text.find("remote server") != std::string_view::npos)
            found = true;
    CHECK(found);
}
