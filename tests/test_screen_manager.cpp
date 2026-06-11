// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "FlightScreen.h"
#include "ScreenManager.h"
#include "SettingsScreen.h"
#include "mock_hal.h"

#include "config/UserConfig.h"
#include "content/AssetManager.h"

#include <atomic>
#include <string>
#include <string_view>

// MockInput subclass that tracks setMouseCapture calls.
struct TrackingInput : public MockInput {
    int captureCount{0};
    bool lastCapture{false};
    void setMouseCapture(bool c) override {
        ++captureCount;
        lastCapture = c;
    }
};

// Fixture that owns all the pieces needed to call ScreenManager::init().
struct Fixture {
    TrackingInput input;
    MockLogger log;
    MockRenderer renderer;
    MockWindow window;
    MockDisplay display;
    MockFilesystem fs;
    UserConfig cfg{fs, log};
    AssetManager assets{/*packs=*/{}, log};
    ScreenManager mgr{input, log};

    void initAll() {
        mgr.init(cfg, renderer, window, display, assets);
    }
};

TEST_CASE("ScreenManager: default screen is MainMenu") {
    Fixture f;
    CHECK(f.mgr.current() == Screen::MainMenu);
}

TEST_CASE("ScreenManager: init does not crash with empty assets") {
    Fixture f;
    f.initAll();
    CHECK(f.mgr.current() == Screen::MainMenu);
}

TEST_CASE("ScreenManager: transition to MainMenu does not capture mouse") {
    Fixture f;
    f.initAll();
    f.mgr.transition(Screen::MainMenu);
    CHECK(f.input.captureCount == 0);
    CHECK(f.mgr.current() == Screen::MainMenu);
}

TEST_CASE("ScreenManager: transition to Flight captures mouse") {
    Fixture f;
    f.initAll();
    // reinitFlight with null deps — transition only fires side effects, does not call update()
    f.mgr.reinitFlight(FlightScreenDeps{});
    f.mgr.transition(Screen::Flight);
    CHECK(f.input.lastCapture == true);
    CHECK(f.input.captureCount == 1);
    CHECK(f.mgr.current() == Screen::Flight);
}

TEST_CASE("ScreenManager: transition from Flight to menu releases mouse") {
    Fixture f;
    f.initAll();
    f.mgr.reinitFlight(FlightScreenDeps{});
    f.mgr.transition(Screen::Flight);
    f.mgr.transition(Screen::MainMenu);
    CHECK(f.input.lastCapture == false);
    CHECK(f.mgr.current() == Screen::MainMenu);
}

TEST_CASE("ScreenManager: transition to Pause fires pause serverCmd (single-player)") {
    Fixture f;
    f.initAll();
    f.mgr.reinitFlight(FlightScreenDeps{});
    f.mgr.transition(Screen::Flight);

    std::string lastCmd;
    f.mgr.setServerCmd([&](std::string_view cmd) { lastCmd = cmd; });

    f.mgr.transition(Screen::Pause);
    CHECK(lastCmd == "pause");
    CHECK(f.mgr.current() == Screen::Pause);
}

TEST_CASE("ScreenManager: transition from Pause to Flight fires resume serverCmd") {
    Fixture f;
    f.initAll();
    f.mgr.reinitFlight(FlightScreenDeps{});
    f.mgr.transition(Screen::Flight);

    std::string lastCmd;
    f.mgr.setServerCmd([&](std::string_view cmd) { lastCmd = cmd; });

    f.mgr.transition(Screen::Pause);
    CHECK(lastCmd == "pause");
    f.mgr.transition(Screen::Flight);
    CHECK(lastCmd == "resume");
}

TEST_CASE("ScreenManager: transition from Pause to MainMenu fires resume serverCmd") {
    Fixture f;
    f.initAll();
    f.mgr.reinitFlight(FlightScreenDeps{});
    f.mgr.transition(Screen::Flight);

    std::string lastCmd;
    f.mgr.setServerCmd([&](std::string_view cmd) { lastCmd = cmd; });

    f.mgr.transition(Screen::Pause);
    f.mgr.transition(Screen::MainMenu);
    CHECK(lastCmd == "resume");
}

TEST_CASE("ScreenManager: null serverCmd - no crash on Pause transition (multiplayer)") {
    Fixture f;
    f.initAll();
    f.mgr.reinitFlight(FlightScreenDeps{});
    // No setServerCmd call — m_serverCmd is null, simulating multiplayer
    f.mgr.transition(Screen::Flight);
    f.mgr.transition(Screen::Pause); // must not crash
    CHECK(f.mgr.current() == Screen::Pause);
}

TEST_CASE("ScreenManager: Settings from MainMenu sets return target to MainMenu") {
    Fixture f;
    f.initAll();
    f.mgr.transition(Screen::Settings);
    // Escape from settings should return to MainMenu (tested via SettingsScreen directly)
    CHECK(f.mgr.current() == Screen::Settings);
}

TEST_CASE("ScreenManager: Settings from Pause sets return target to Pause") {
    Fixture f;
    f.initAll();
    f.mgr.reinitFlight(FlightScreenDeps{});
    f.mgr.transition(Screen::Flight);
    f.mgr.transition(Screen::Pause);
    f.mgr.transition(Screen::Settings);
    CHECK(f.mgr.current() == Screen::Settings);
    // The return target is Pause; verify via settings screen
    MockInput inp;
    inp.justPressed.insert(Key::Escape);
    Screen ret = f.mgr.settings().update(inp, f.window);
    CHECK(ret == Screen::Pause);
}

TEST_CASE("ScreenManager: reinitLoading replaces loading screen") {
    Fixture f;
    f.initAll();
    std::atomic<bool> ready{false};
    int onReadyCount = 0;
    f.mgr.reinitLoading(ready, [] { return false; }, [&] { ++onReadyCount; });
    f.mgr.transition(Screen::Loading);
    CHECK(f.mgr.current() == Screen::Loading);
}
