// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "SettingsScreen.h"
#include "mock_hal.h"

#include "config/AudioSettings.h"
#include "config/GraphicsSettings.h"
#include "config/UserConfig.h"

// Minimal fixture: UserConfig backed by in-memory filesystem.
struct Fixture {
    MockFilesystem fs;
    MockLogger log;
    MockRenderer renderer;
    MockWindow window;
    MockDisplay display;
    UserConfig cfg{fs, log};
};

static MockInput g_inp;

TEST_CASE("SettingsScreen: constructs without crash") {
    Fixture f;
    SettingsScreen s(f.cfg, f.renderer, f.window, f.display);
    Screen next = s.update(g_inp, f.window);
    CHECK(next == Screen::Settings);
}

TEST_CASE("SettingsScreen: Escape applies settings and returns MainMenu") {
    Fixture f;
    SettingsScreen s(f.cfg, f.renderer, f.window, f.display);
    MockInput inp;
    inp.justPressed.insert(Key::Escape);
    Screen next = s.update(inp, f.window);
    CHECK(next == Screen::MainMenu);
}

TEST_CASE("SettingsScreen: setReturnTarget redirects Back to Pause") {
    Fixture f;
    SettingsScreen s(f.cfg, f.renderer, f.window, f.display);
    s.setReturnTarget(Screen::Pause);
    MockInput inp;
    inp.justPressed.insert(Key::Escape);
    Screen next = s.update(inp, f.window);
    CHECK(next == Screen::Pause);
}

TEST_CASE("SettingsScreen: buildElements not empty") {
    Fixture f;
    SettingsScreen s(f.cfg, f.renderer, f.window, f.display);
    s.update(g_inp, f.window);
    CHECK(!s.buildElements().empty());
}

TEST_CASE("SettingsScreen: Right arrow cycles vsync Off to On") {
    Fixture f;
    // Navigate to vsync row (row 2), press Right
    SettingsScreen s(f.cfg, f.renderer, f.window, f.display);

    // Down twice reaches Vsync row (row 0=Resolution, 1=Display, 2=Vsync)
    for (int i = 0; i < 2; ++i) {
        MockInput inp;
        inp.justPressed.insert(Key::ArrowDown);
        s.update(inp, f.window);
    }
    // Right = cycle vsync
    {
        MockInput inp;
        inp.justPressed.insert(Key::ArrowRight);
        s.update(inp, f.window);
    }
    // Escape to apply + return
    {
        MockInput inp;
        inp.justPressed.insert(Key::Escape);
        s.update(inp, f.window);
    }
    // Default vsync is On; one Right should cycle to Adaptive
    GraphicsSettings gs = f.cfg.graphics();
    CHECK(gs.vsync == VsyncMode::Adaptive);
}

TEST_CASE("SettingsScreen: master volume clamps at 0 when decremented from 0") {
    Fixture f;
    // Set initial volume to 0
    AudioSettings as = f.cfg.audio();
    as.masterVolume = 0.0f;
    f.cfg.setAudio(as);

    SettingsScreen s(f.cfg, f.renderer, f.window, f.display);
    // Navigate to master volume row (row 5: 0=Res,1=Display,2=Vsync,3=AA,4=DrawDist,5=MasterVol)
    for (int i = 0; i < 5; ++i) {
        MockInput inp;
        inp.justPressed.insert(Key::ArrowDown);
        s.update(inp, f.window);
    }
    // Left: decrement (should clamp at 0)
    {
        MockInput inp;
        inp.justPressed.insert(Key::ArrowLeft);
        s.update(inp, f.window);
    }
    {
        MockInput inp;
        inp.justPressed.insert(Key::Escape);
        s.update(inp, f.window);
    }
    CHECK(f.cfg.audio().masterVolume >= 0.0f);
    CHECK(f.cfg.audio().masterVolume <= 0.05f); // clamped near 0
}

TEST_CASE("SettingsScreen: master volume clamps at 1 when incremented from 1") {
    Fixture f;
    AudioSettings as = f.cfg.audio();
    as.masterVolume = 1.0f;
    f.cfg.setAudio(as);

    SettingsScreen s(f.cfg, f.renderer, f.window, f.display);
    for (int i = 0; i < 5; ++i) {
        MockInput inp;
        inp.justPressed.insert(Key::ArrowDown);
        s.update(inp, f.window);
    }
    {
        MockInput inp;
        inp.justPressed.insert(Key::ArrowRight);
        s.update(inp, f.window);
    }
    {
        MockInput inp;
        inp.justPressed.insert(Key::Escape);
        s.update(inp, f.window);
    }
    CHECK(f.cfg.audio().masterVolume <= 1.0f);
}
