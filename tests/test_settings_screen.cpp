// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "SettingsScreen.h"
#include "mock_hal.h"

#include "config/AudioSettings.h"
#include "config/GraphicsSettings.h"
#include "config/UserConfig.h"

using namespace fl;

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

TEST_CASE("SettingsScreen: AA mode cycles on Right") {
    Fixture f;
    SettingsScreen s(f.cfg, f.renderer, f.window, f.display);
    // Navigate to AA mode row (row 3: 0=Res,1=Display,2=Vsync,3=AAMode)
    for (int i = 0; i < 3; ++i) {
        MockInput inp;
        inp.justPressed.insert(Key::ArrowDown);
        s.update(inp, f.window);
    }
    // Default aaMode is TAA (ordinal 2); one Right wraps to Off (ordinal 0)
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
    CHECK(f.cfg.graphics().aaMode == AntiAliasingMode::Off);
    CHECK(f.renderer.lastApplied.aaMode == RendererAAMode::Off);
}

TEST_CASE("SettingsScreen: AA mode wraps after 3 cycles") {
    Fixture f;
    SettingsScreen s(f.cfg, f.renderer, f.window, f.display);
    for (int i = 0; i < 3; ++i) {
        MockInput inp;
        inp.justPressed.insert(Key::ArrowDown);
        s.update(inp, f.window);
    }
    // Default is TAA (ordinal 2); 3 Rights wraps back to TAA
    for (int i = 0; i < 3; ++i) {
        MockInput inp;
        inp.justPressed.insert(Key::ArrowRight);
        s.update(inp, f.window);
    }
    {
        MockInput inp;
        inp.justPressed.insert(Key::Escape);
        s.update(inp, f.window);
    }
    CHECK(f.cfg.graphics().aaMode == AntiAliasingMode::TAA);
}

TEST_CASE("SettingsScreen: ambient occlusion cycles on Right") {
    Fixture f;
    SettingsScreen s(f.cfg, f.renderer, f.window, f.display);
    // Navigate to AO row (row 5: ...,4=Shadow,5=AmbientOcclusion)
    for (int i = 0; i < 5; ++i) {
        MockInput inp;
        inp.justPressed.insert(Key::ArrowDown);
        s.update(inp, f.window);
    }
    // Default is High (ordinal 2); one Right wraps to Off (ordinal 0)
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
    CHECK(f.cfg.graphics().ambientOcclusion == AmbientOcclusion::Off);
    CHECK(f.renderer.lastApplied.aoMode == RendererAOMode::Off);
}

TEST_CASE("SettingsScreen: sky quality cycles on Right") {
    Fixture f;
    SettingsScreen s(f.cfg, f.renderer, f.window, f.display);
    // Navigate to sky quality row (row 6)
    for (int i = 0; i < 6; ++i) {
        MockInput inp;
        inp.justPressed.insert(Key::ArrowDown);
        s.update(inp, f.window);
    }
    // Default is LUT (ordinal 1); one Right wraps to Procedural (ordinal 0)
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
    CHECK(f.cfg.graphics().skyQuality == SkyQuality::Procedural);
    CHECK(f.renderer.lastApplied.skyQuality == RendererSkyQuality::Procedural);
}

TEST_CASE("SettingsScreen: shadow quality cycles on Right") {
    Fixture f;
    SettingsScreen s(f.cfg, f.renderer, f.window, f.display);
    // Navigate to shadow quality row (row 4)
    for (int i = 0; i < 4; ++i) {
        MockInput inp;
        inp.justPressed.insert(Key::ArrowDown);
        s.update(inp, f.window);
    }
    // Default is High (ordinal 3); one Right cycles to Ultra
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
    CHECK(f.cfg.graphics().shadowQuality == ShadowQuality::Ultra);
    CHECK(f.renderer.lastApplied.shadowQuality == RendererShadowQuality::Ultra);
}

TEST_CASE("SettingsScreen: particle density cycles on Right") {
    Fixture f;
    SettingsScreen s(f.cfg, f.renderer, f.window, f.display);
    // Navigate to particle density row (row 7)
    for (int i = 0; i < 7; ++i) {
        MockInput inp;
        inp.justPressed.insert(Key::ArrowDown);
        s.update(inp, f.window);
    }
    // Default is High (ordinal 2); one Right cycles to Ultra
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
    CHECK(f.cfg.graphics().particleDensity == ParticleDensity::Ultra);
    CHECK(f.renderer.lastApplied.particleDensity == RendererParticleDensity::Ultra);
}

TEST_CASE("SettingsScreen: master volume clamps at 0 when decremented from 0") {
    Fixture f;
    // Set initial volume to 0
    AudioSettings as = f.cfg.audio();
    as.masterVolume = 0.0f;
    f.cfg.setAudio(as);

    SettingsScreen s(f.cfg, f.renderer, f.window, f.display);
    // Navigate to master volume row (row 9:
    // 0=Res,1=Display,2=Vsync,3=AAMode,4=Shadow,5=AO,6=Sky,7=Particles,8=DrawDist,9=MasterVol)
    for (int i = 0; i < 9; ++i) {
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
    // Navigate to master volume row (row 9)
    for (int i = 0; i < 9; ++i) {
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
