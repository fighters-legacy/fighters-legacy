// SPDX-License-Identifier: GPL-3.0-or-later
//
// rendererSettingsFrom (#1235): the ONE GraphicsSettings -> RendererSettings mapping. Game (fresh
// launch) and SettingsScreen (Apply) carried diverged copies — Apply forced bloom on while launch
// derived it from the quality preset, so the same saved config rendered differently after Apply
// than after a restart. These pins hold the resolved rules.

#include "RendererSettingsMap.h"

#include <catch2/catch_test_macros.hpp>

using namespace fl;

TEST_CASE("rendererSettingsFrom: bloom follows the quality preset (the launch rule)") {
    GraphicsSettings g{};
    g.qualityPreset = QualityLevel::Low;
    CHECK_FALSE(rendererSettingsFrom(g).bloom);
    g.qualityPreset = QualityLevel::Medium;
    CHECK(rendererSettingsFrom(g).bloom);
    g.qualityPreset = QualityLevel::High;
    CHECK(rendererSettingsFrom(g).bloom);
    g.qualityPreset = QualityLevel::Ultra;
    CHECK(rendererSettingsFrom(g).bloom);
}

TEST_CASE("rendererSettingsFrom: draw distance and vsync map as before") {
    GraphicsSettings g{};
    g.drawDistance = DrawDistance::Low;
    CHECK(rendererSettingsFrom(g).drawDistanceKm == 20.0f);
    g.drawDistance = DrawDistance::Medium;
    CHECK(rendererSettingsFrom(g).drawDistanceKm == 50.0f);
    g.drawDistance = DrawDistance::High;
    CHECK(rendererSettingsFrom(g).drawDistanceKm == 100.0f);
    g.drawDistance = DrawDistance::Ultra;
    CHECK(rendererSettingsFrom(g).drawDistanceKm == 200.0f);

    g.vsync = VsyncMode::Off;
    CHECK(rendererSettingsFrom(g).vsync == RendererVsyncMode::Off);
    g.vsync = VsyncMode::On;
    CHECK(rendererSettingsFrom(g).vsync == RendererVsyncMode::On);
    g.vsync = VsyncMode::Adaptive;
    CHECK(rendererSettingsFrom(g).vsync == RendererVsyncMode::Adaptive);

    CHECK(rendererSettingsFrom(g).autoExposure); // baseline HDR feature, always on
}
