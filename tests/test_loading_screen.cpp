// SPDX-License-Identifier: GPL-3.0-or-later
#include "IClock.h"
#include <catch2/catch_test_macros.hpp>

#include "LoadingScreen.h"
#include "SessionStatus.h"
#include "mock_hal.h"

#include <atomic>

using namespace fl;

static MockInput g_inp;
static MockWindow g_win;

TEST_CASE("SessionFailure: None maps to empty, every failure to a non-empty message") {
    CHECK(std::string(sessionFailureMessage(SessionFailure::None)).empty());
    const SessionFailure all[] = {
        SessionFailure::ServerSpawnFailed, SessionFailure::ServerBindFailed, SessionFailure::ServerStartTimeout,
        SessionFailure::ServerStartHang,   SessionFailure::VersionMismatch,  SessionFailure::Banned,
        SessionFailure::AccessDenied,      SessionFailure::RateLimited,      SessionFailure::TooManyConnections,
        SessionFailure::ConnectionRefused, SessionFailure::ConnectTimeout};
    for (SessionFailure f : all)
        CHECK(std::string(sessionFailureMessage(f)).length() > 0);
}

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
    fl::ManualClock fakeNow;

    std::atomic<bool> ready{true};
    LoadingScreen s(ready, [] { return false; }, [] {});
    s.setClock(fakeNow);

    s.update(g_inp, g_win); // StartingServer → Connecting; deadline = fakeNow + 10s

    CHECK(s.update(g_inp, g_win) == Screen::Loading); // within timeout

    fakeNow.advance(std::chrono::seconds(11));        // past connect deadline
    CHECK(s.update(g_inp, g_win) == Screen::Loading); // → Failed; within kFailDisplaySeconds

    bool foundMsg = false;
    for (const auto& el : s.buildElements())
        if (el.type == HudElement::Type::Text && el.text.find("timed out") != std::string::npos)
            foundMsg = true;
    CHECK(foundMsg);

    fakeNow.advance(std::chrono::seconds(4)); // past kFailDisplaySeconds (3s)
    CHECK(s.update(g_inp, g_win) == Screen::MainMenu);
}

TEST_CASE("LoadingScreen: startup timeout trips Failed phase then returns MainMenu") {
    fl::ManualClock fakeNow;

    std::atomic<bool> ready{false}; // server never starts
    bool onReadyCalled = false;
    LoadingScreen s(ready, [] { return false; }, [&] { onReadyCalled = true; });
    s.setClock(fakeNow);

    s.update(g_inp, g_win); // StartingServer; deadline = fakeNow + 10s

    CHECK(s.update(g_inp, g_win) == Screen::Loading); // within timeout
    CHECK(!onReadyCalled);

    fakeNow.advance(std::chrono::seconds(11));        // past startup deadline
    CHECK(s.update(g_inp, g_win) == Screen::Loading); // → Failed; within kFailDisplaySeconds
    CHECK(!onReadyCalled);

    bool foundMsg = false;
    for (const auto& el : s.buildElements())
        if (el.type == HudElement::Type::Text && el.text.find("failed to start") != std::string::npos)
            foundMsg = true;
    CHECK(foundMsg);

    fakeNow.advance(std::chrono::seconds(4)); // past kFailDisplaySeconds (3s)
    CHECK(s.update(g_inp, g_win) == Screen::MainMenu);
}

TEST_CASE("LoadingScreen: reset after startup timeout allows successful second session") {
    fl::ManualClock fakeNow;

    std::atomic<bool> ready{false};
    int onReadyCount = 0;
    bool connected = false;
    LoadingScreen s(ready, [&] { return connected; }, [&] { ++onReadyCount; });
    s.setClock(fakeNow);

    // First session: startup timeout fires.
    s.update(g_inp, g_win);
    fakeNow.advance(std::chrono::seconds(11));
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
    fl::ManualClock fakeNow;

    std::atomic<bool> ready{false};
    LoadingScreen s(ready, [] { return false; }, [] {});
    s.setClock(fakeNow);

    s.update(g_inp, g_win); // sets start deadline at fakeNow

    fakeNow.advance(std::chrono::seconds(5)); // halfway through first session
    s.reset();                                // clears both deadlines

    // After reset the next update sets a fresh deadline from current fakeNow.
    s.update(g_inp, g_win);

    fakeNow.advance(std::chrono::seconds(6));         // only 6s into NEW deadline (< 10s)
    CHECK(s.update(g_inp, g_win) == Screen::Loading); // must NOT have timed out
}

TEST_CASE("LoadingScreen: spawn fail message shown immediately without timeout") {
    fl::ManualClock fakeNow;

    std::atomic<bool> ready{false};
    std::atomic<SessionFailure> failure{SessionFailure::None};
    LoadingScreen s(ready, [] { return false; }, [] {}, /*isSinglePlayer=*/true, &failure);
    s.setClock(fakeNow);

    s.update(g_inp, g_win); // sets start deadline; no failure yet

    failure.store(SessionFailure::ServerSpawnFailed);
    CHECK(s.update(g_inp, g_win) == Screen::Loading); // -> Failed, within kFailDisplaySeconds

    bool found = false;
    for (const auto& el : s.buildElements())
        if (el.type == HudElement::Type::Text && el.text.find("not found") != std::string::npos)
            found = true;
    CHECK(found);

    fakeNow.advance(std::chrono::seconds(4)); // past kFailDisplaySeconds (3 s)
    CHECK(s.update(g_inp, g_win) == Screen::MainMenu);
}

TEST_CASE("LoadingScreen: bind fail message shown immediately without timeout") {
    fl::ManualClock fakeNow;

    std::atomic<bool> ready{false};
    std::atomic<SessionFailure> failure{SessionFailure::None};
    LoadingScreen s(ready, [] { return false; }, [] {}, /*isSinglePlayer=*/true, &failure);
    s.setClock(fakeNow);

    s.update(g_inp, g_win); // sets start deadline

    failure.store(SessionFailure::ServerBindFailed);
    CHECK(s.update(g_inp, g_win) == Screen::Loading); // -> Failed, within kFailDisplaySeconds

    bool found = false;
    for (const auto& el : s.buildElements())
        if (el.type == HudElement::Type::Text && el.text.find("already in use") != std::string::npos)
            found = true;
    CHECK(found);

    fakeNow.advance(std::chrono::seconds(4));
    CHECK(s.update(g_inp, g_win) == Screen::MainMenu);
}

TEST_CASE("LoadingScreen: server timeout fail message shown immediately without timeout") {
    fl::ManualClock fakeNow;

    std::atomic<bool> ready{false};
    std::atomic<SessionFailure> failure{SessionFailure::None};
    LoadingScreen s(ready, [] { return false; }, [] {}, /*isSinglePlayer=*/true, &failure);
    s.setClock(fakeNow);

    s.update(g_inp, g_win); // sets start deadline

    failure.store(SessionFailure::ServerStartTimeout);
    CHECK(s.update(g_inp, g_win) == Screen::Loading); // -> Failed, within kFailDisplaySeconds

    bool found = false;
    for (const auto& el : s.buildElements())
        if (el.type == HudElement::Type::Text && el.text.find("startup timed out") != std::string::npos)
            found = true;
    CHECK(found);

    fakeNow.advance(std::chrono::seconds(4));
    CHECK(s.update(g_inp, g_win) == Screen::MainMenu);
}

TEST_CASE("LoadingScreen: fallback generic message shown on startup deadline with no fail msg") {
    fl::ManualClock fakeNow;

    std::atomic<bool> ready{false};
    // No getStartFailMsg — simulates a hung start() that never returns.
    LoadingScreen s(ready, [] { return false; }, [] {});
    s.setClock(fakeNow);

    s.update(g_inp, g_win); // sets start deadline at fakeNow + 10 s

    fakeNow.advance(std::chrono::seconds(11));        // past startup deadline
    CHECK(s.update(g_inp, g_win) == Screen::Loading); // -> Failed, within kFailDisplaySeconds

    bool found = false;
    for (const auto& el : s.buildElements())
        if (el.type == HudElement::Type::Text && el.text.find("failed to start") != std::string::npos)
            found = true;
    CHECK(found);

    fakeNow.advance(std::chrono::seconds(4));
    CHECK(s.update(g_inp, g_win) == Screen::MainMenu);
}

TEST_CASE("LoadingScreen: version mismatch shown immediately in Connecting phase") {
    fl::ManualClock fakeNow;

    std::atomic<bool> ready{true}; // multiplayer fast-path: skip StartingServer
    std::atomic<SessionFailure> failure{SessionFailure::None};
    LoadingScreen s(ready, [] { return false; }, [] {}, /*isSinglePlayer=*/false, &failure);
    s.setClock(fakeNow);

    s.update(g_inp, g_win); // StartingServer -> Connecting (serverReady already true)

    failure.store(SessionFailure::VersionMismatch);
    CHECK(s.update(g_inp, g_win) == Screen::Loading); // -> Failed, within kFailDisplaySeconds

    bool found = false;
    for (const auto& el : s.buildElements())
        if (el.type == HudElement::Type::Text && el.text.find("mismatch") != std::string::npos)
            found = true;
    CHECK(found);

    fakeNow.advance(std::chrono::seconds(4)); // past kFailDisplaySeconds (3 s)
    CHECK(s.update(g_inp, g_win) == Screen::MainMenu);
}

TEST_CASE("LoadingScreen: connection refused shown immediately in Connecting phase") {
    fl::ManualClock fakeNow;

    std::atomic<bool> ready{true};
    std::atomic<SessionFailure> failure{SessionFailure::None};
    LoadingScreen s(ready, [] { return false; }, [] {}, /*isSinglePlayer=*/false, &failure);
    s.setClock(fakeNow);

    s.update(g_inp, g_win); // -> Connecting

    failure.store(SessionFailure::ConnectionRefused);
    CHECK(s.update(g_inp, g_win) == Screen::Loading); // -> Failed

    bool found = false;
    for (const auto& el : s.buildElements())
        if (el.type == HudElement::Type::Text && el.text.find("refused") != std::string::npos)
            found = true;
    CHECK(found);

    fakeNow.advance(std::chrono::seconds(4));
    CHECK(s.update(g_inp, g_win) == Screen::MainMenu);
}

TEST_CASE("LoadingScreen: getConnectFailMsg null does not break timeout path") {
    fl::ManualClock fakeNow;

    std::atomic<bool> ready{true};
    // callback wired but always returns null
    LoadingScreen s(ready, [] { return false; }, [] {}, /*isSinglePlayer=*/false);
    s.setClock(fakeNow);

    s.update(g_inp, g_win); // -> Connecting

    fakeNow.advance(std::chrono::seconds(11));        // past connect deadline
    CHECK(s.update(g_inp, g_win) == Screen::Loading); // -> Failed via timeout

    bool found = false;
    for (const auto& el : s.buildElements())
        if (el.type == HudElement::Type::Text && el.text.find("timed out") != std::string::npos)
            found = true;
    CHECK(found);

    fakeNow.advance(std::chrono::seconds(4));
    CHECK(s.update(g_inp, g_win) == Screen::MainMenu);
}

TEST_CASE("LoadingScreen: getConnectFailMsg null does not break success path") {
    std::atomic<bool> ready{true};
    bool connected = false;
    LoadingScreen s(ready, [&] { return connected; }, [] {}, /*isSinglePlayer=*/false);

    s.update(g_inp, g_win); // -> Connecting
    connected = true;
    s.update(g_inp, g_win); // -> Ready
    CHECK(s.update(g_inp, g_win) == Screen::Flight);
}

TEST_CASE("LoadingScreen: version mismatch in single-player flow") {
    fl::ManualClock fakeNow;

    std::atomic<bool> ready{false};
    std::atomic<SessionFailure> failure{SessionFailure::None};
    LoadingScreen s(ready, [] { return false; }, [] {}, /*isSinglePlayer=*/true, &failure);
    s.setClock(fakeNow);

    s.update(g_inp, g_win); // StartingServer; deadline set
    ready.store(true);
    s.update(g_inp, g_win); // -> Connecting; onServerReady fires

    failure.store(SessionFailure::VersionMismatch);
    CHECK(s.update(g_inp, g_win) == Screen::Loading); // -> Failed immediately

    bool found = false;
    for (const auto& el : s.buildElements())
        if (el.type == HudElement::Type::Text && el.text.find("mismatch") != std::string::npos)
            found = true;
    CHECK(found);

    fakeNow.advance(std::chrono::seconds(4));
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
