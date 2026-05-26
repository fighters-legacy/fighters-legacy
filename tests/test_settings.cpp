// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "config/UserConfig.h"
#include "mock_hal.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static UserConfig makeAndSave(MockFilesystem& fs, MockLogger& logger, const GraphicsSettings& gs,
                              const AudioSettings& as) {
    UserConfig cfg(fs, logger);
    cfg.setGraphics(gs);
    cfg.setAudio(as);
    cfg.save();
    return cfg;
}

static UserConfig reload(MockFilesystem& fs) {
    MockLogger dummy;
    UserConfig cfg(fs, dummy);
    cfg.load();
    return cfg;
}

// ---------------------------------------------------------------------------
// Upgrade path: existing [first_run]/[engine]-only file → defaults, no Warn
// ---------------------------------------------------------------------------

TEST_CASE("Settings: missing [graphics]/[audio] sections load defaults with no Warn", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("config/user.toml", "[first_run]\ncompleted = true\n\n[engine]\nlog_level = \"info\"\n");
    UserConfig cfg(fs, logger);
    cfg.load();

    CHECK(logger.entries.empty());
    GraphicsSettings g = cfg.graphics();
    CHECK(g.resolutionWidth == 0);
    CHECK(g.resolutionHeight == 0);
    CHECK(g.vsync == VsyncMode::On);
    CHECK(g.frameRateCap == FrameRateCap::Off);
    CHECK(g.qualityPreset == QualityLevel::High);
    CHECK(g.drawDistance == DrawDistance::High);
    CHECK(g.antiAliasing == true);
    CHECK(g.uiScale == UiScale::Scale100);
    CHECK(g.cockpitFov == 90);

    AudioSettings a = cfg.audio();
    CHECK(a.masterVolume == Catch::Approx(0.80f));
    CHECK(a.sfxVolume == Catch::Approx(1.00f));
    CHECK(a.musicVolume == Catch::Approx(0.70f));
    CHECK(a.voiceChatVolume == Catch::Approx(1.00f));
    CHECK(a.rwrVolume == Catch::Approx(1.00f));
}

// ---------------------------------------------------------------------------
// Cross-section data preservation
// ---------------------------------------------------------------------------

TEST_CASE("Settings: save preserves [first_run] and [engine] sections", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    UserConfig cfg(fs, logger);
    cfg.setFirstRunCompleted(true);
    cfg.setLogLevel(LogLevel::Debug);
    cfg.save();

    UserConfig cfg2(fs, logger);
    cfg2.load();
    CHECK(cfg2.isFirstRunCompleted());
    CHECK(cfg2.logLevel() == LogLevel::Debug);
}

// ---------------------------------------------------------------------------
// VsyncMode round-trips
// ---------------------------------------------------------------------------

TEST_CASE("Settings: VsyncMode Off round-trip", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    GraphicsSettings gs;
    gs.vsync = VsyncMode::Off;
    makeAndSave(fs, logger, gs, {});
    CHECK(reload(fs).graphics().vsync == VsyncMode::Off);
}

TEST_CASE("Settings: VsyncMode Adaptive round-trip", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    GraphicsSettings gs;
    gs.vsync = VsyncMode::Adaptive;
    makeAndSave(fs, logger, gs, {});
    CHECK(reload(fs).graphics().vsync == VsyncMode::Adaptive);
}

TEST_CASE("Settings: unknown vsync string falls back to On and emits Warn", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("config/user.toml", "[graphics]\nvsync = \"turbo\"\n");
    UserConfig cfg(fs, logger);
    cfg.load();
    CHECK(cfg.graphics().vsync == VsyncMode::On);
    CHECK(logger.hasMessage(LogLevel::Warn, "turbo"));
}

// ---------------------------------------------------------------------------
// FrameRateCap round-trips
// ---------------------------------------------------------------------------

TEST_CASE("Settings: FrameRateCap Cap30 round-trip", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    GraphicsSettings gs;
    gs.frameRateCap = FrameRateCap::Cap30;
    makeAndSave(fs, logger, gs, {});
    CHECK(reload(fs).graphics().frameRateCap == FrameRateCap::Cap30);
}

TEST_CASE("Settings: FrameRateCap Cap60 round-trip", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    GraphicsSettings gs;
    gs.frameRateCap = FrameRateCap::Cap60;
    makeAndSave(fs, logger, gs, {});
    CHECK(reload(fs).graphics().frameRateCap == FrameRateCap::Cap60);
}

TEST_CASE("Settings: FrameRateCap Cap120 round-trip", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    GraphicsSettings gs;
    gs.frameRateCap = FrameRateCap::Cap120;
    makeAndSave(fs, logger, gs, {});
    CHECK(reload(fs).graphics().frameRateCap == FrameRateCap::Cap120);
}

TEST_CASE("Settings: FrameRateCap Cap144 round-trip", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    GraphicsSettings gs;
    gs.frameRateCap = FrameRateCap::Cap144;
    makeAndSave(fs, logger, gs, {});
    CHECK(reload(fs).graphics().frameRateCap == FrameRateCap::Cap144);
}

TEST_CASE("Settings: FrameRateCap Cap240 round-trip", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    GraphicsSettings gs;
    gs.frameRateCap = FrameRateCap::Cap240;
    makeAndSave(fs, logger, gs, {});
    CHECK(reload(fs).graphics().frameRateCap == FrameRateCap::Cap240);
}

TEST_CASE("Settings: unknown frame_rate_cap falls back to Off and emits Warn", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("config/user.toml", "[graphics]\nframe_rate_cap = \"999\"\n");
    UserConfig cfg(fs, logger);
    cfg.load();
    CHECK(cfg.graphics().frameRateCap == FrameRateCap::Off);
    CHECK(logger.hasMessage(LogLevel::Warn, "999"));
}

// ---------------------------------------------------------------------------
// QualityLevel round-trips
// ---------------------------------------------------------------------------

TEST_CASE("Settings: QualityLevel Low round-trip", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    GraphicsSettings gs;
    gs.qualityPreset = QualityLevel::Low;
    makeAndSave(fs, logger, gs, {});
    CHECK(reload(fs).graphics().qualityPreset == QualityLevel::Low);
}

TEST_CASE("Settings: QualityLevel Medium round-trip", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    GraphicsSettings gs;
    gs.qualityPreset = QualityLevel::Medium;
    makeAndSave(fs, logger, gs, {});
    CHECK(reload(fs).graphics().qualityPreset == QualityLevel::Medium);
}

TEST_CASE("Settings: QualityLevel Ultra round-trip", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    GraphicsSettings gs;
    gs.qualityPreset = QualityLevel::Ultra;
    makeAndSave(fs, logger, gs, {});
    CHECK(reload(fs).graphics().qualityPreset == QualityLevel::Ultra);
}

TEST_CASE("Settings: unknown quality_preset falls back to High and emits Warn", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("config/user.toml", "[graphics]\nquality_preset = \"extreme\"\n");
    UserConfig cfg(fs, logger);
    cfg.load();
    CHECK(cfg.graphics().qualityPreset == QualityLevel::High);
    CHECK(logger.hasMessage(LogLevel::Warn, "extreme"));
}

// ---------------------------------------------------------------------------
// DrawDistance round-trips
// ---------------------------------------------------------------------------

TEST_CASE("Settings: DrawDistance Low round-trip", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    GraphicsSettings gs;
    gs.drawDistance = DrawDistance::Low;
    makeAndSave(fs, logger, gs, {});
    CHECK(reload(fs).graphics().drawDistance == DrawDistance::Low);
}

TEST_CASE("Settings: DrawDistance Ultra round-trip", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    GraphicsSettings gs;
    gs.drawDistance = DrawDistance::Ultra;
    makeAndSave(fs, logger, gs, {});
    CHECK(reload(fs).graphics().drawDistance == DrawDistance::Ultra);
}

TEST_CASE("Settings: unknown draw_distance falls back to High and emits Warn", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("config/user.toml", "[graphics]\ndraw_distance = \"infinite\"\n");
    UserConfig cfg(fs, logger);
    cfg.load();
    CHECK(cfg.graphics().drawDistance == DrawDistance::High);
    CHECK(logger.hasMessage(LogLevel::Warn, "infinite"));
}

// ---------------------------------------------------------------------------
// AntiAliasing boolean round-trips
// ---------------------------------------------------------------------------

TEST_CASE("Settings: antiAliasing false round-trip", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    GraphicsSettings gs;
    gs.antiAliasing = false;
    makeAndSave(fs, logger, gs, {});
    CHECK(reload(fs).graphics().antiAliasing == false);
}

TEST_CASE("Settings: antiAliasing true round-trip", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    GraphicsSettings gs;
    gs.antiAliasing = true;
    makeAndSave(fs, logger, gs, {});
    CHECK(reload(fs).graphics().antiAliasing == true);
}

// ---------------------------------------------------------------------------
// UiScale round-trips
// ---------------------------------------------------------------------------

TEST_CASE("Settings: UiScale Scale75 round-trip", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    GraphicsSettings gs;
    gs.uiScale = UiScale::Scale75;
    makeAndSave(fs, logger, gs, {});
    CHECK(reload(fs).graphics().uiScale == UiScale::Scale75);
}

TEST_CASE("Settings: UiScale Scale125 round-trip", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    GraphicsSettings gs;
    gs.uiScale = UiScale::Scale125;
    makeAndSave(fs, logger, gs, {});
    CHECK(reload(fs).graphics().uiScale == UiScale::Scale125);
}

TEST_CASE("Settings: UiScale Scale150 round-trip", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    GraphicsSettings gs;
    gs.uiScale = UiScale::Scale150;
    makeAndSave(fs, logger, gs, {});
    CHECK(reload(fs).graphics().uiScale == UiScale::Scale150);
}

TEST_CASE("Settings: unknown ui_scale falls back to Scale100 and emits Warn", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("config/user.toml", "[graphics]\nui_scale = 999\n");
    UserConfig cfg(fs, logger);
    cfg.load();
    CHECK(cfg.graphics().uiScale == UiScale::Scale100);
    CHECK(logger.hasMessage(LogLevel::Warn, "999"));
}

// ---------------------------------------------------------------------------
// CockpitFov clamping
// ---------------------------------------------------------------------------

TEST_CASE("Settings: cockpitFov below 60 clamps to 60 and emits Warn", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("config/user.toml", "[graphics]\ncockpit_fov = 30\n");
    UserConfig cfg(fs, logger);
    cfg.load();
    CHECK(cfg.graphics().cockpitFov == 60);
    CHECK(logger.hasMessage(LogLevel::Warn, "cockpit_fov"));
}

TEST_CASE("Settings: cockpitFov above 120 clamps to 120 and emits Warn", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("config/user.toml", "[graphics]\ncockpit_fov = 999\n");
    UserConfig cfg(fs, logger);
    cfg.load();
    CHECK(cfg.graphics().cockpitFov == 120);
    CHECK(logger.hasMessage(LogLevel::Warn, "cockpit_fov"));
}

TEST_CASE("Settings: cockpitFov in-range round-trip", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    GraphicsSettings gs;
    gs.cockpitFov = 75;
    makeAndSave(fs, logger, gs, {});
    CHECK(reload(fs).graphics().cockpitFov == 75);
}

// ---------------------------------------------------------------------------
// Resolution
// ---------------------------------------------------------------------------

TEST_CASE("Settings: resolution 0,0 round-trips as native", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    GraphicsSettings gs;
    gs.resolutionWidth = 0;
    gs.resolutionHeight = 0;
    makeAndSave(fs, logger, gs, {});
    auto g = reload(fs).graphics();
    CHECK(g.resolutionWidth == 0);
    CHECK(g.resolutionHeight == 0);
}

TEST_CASE("Settings: explicit resolution round-trips", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    GraphicsSettings gs;
    gs.resolutionWidth = 1920;
    gs.resolutionHeight = 1080;
    makeAndSave(fs, logger, gs, {});
    auto g = reload(fs).graphics();
    CHECK(g.resolutionWidth == 1920);
    CHECK(g.resolutionHeight == 1080);
}

TEST_CASE("Settings: negative resolution clamped to 0 (native)", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("config/user.toml", "[graphics]\nresolution_width = -1920\nresolution_height = -1080\n");
    UserConfig cfg(fs, logger);
    cfg.load();
    CHECK(cfg.graphics().resolutionWidth == 0);
    CHECK(cfg.graphics().resolutionHeight == 0);
}

TEST_CASE("Settings: mixed resolution (one zero) treated as native with Warn", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("config/user.toml", "[graphics]\nresolution_width = 1920\nresolution_height = 0\n");
    UserConfig cfg(fs, logger);
    cfg.load();
    CHECK(cfg.graphics().resolutionWidth == 0);
    CHECK(cfg.graphics().resolutionHeight == 0);
    CHECK(logger.hasMessage(LogLevel::Warn, "resolution"));
}

// ---------------------------------------------------------------------------
// Audio volumes
// ---------------------------------------------------------------------------

TEST_CASE("Settings: audio volume round-trip for 0.50f (exact)", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    AudioSettings as;
    as.masterVolume = 0.50f;
    makeAndSave(fs, logger, {}, as);
    CHECK(reload(fs).audio().masterVolume == Catch::Approx(0.50f));
}

TEST_CASE("Settings: audio all volumes round-trip", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    AudioSettings as;
    as.masterVolume = 0.60f;
    as.sfxVolume = 0.80f;
    as.musicVolume = 0.40f;
    as.voiceChatVolume = 0.70f;
    as.rwrVolume = 0.90f;
    makeAndSave(fs, logger, {}, as);
    auto a = reload(fs).audio();
    CHECK(a.masterVolume == Catch::Approx(0.60f));
    CHECK(a.sfxVolume == Catch::Approx(0.80f));
    CHECK(a.musicVolume == Catch::Approx(0.40f));
    CHECK(a.voiceChatVolume == Catch::Approx(0.70f));
    CHECK(a.rwrVolume == Catch::Approx(0.90f));
}

TEST_CASE("Settings: audio volume below 0 clamps to 0.0f", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("config/user.toml", "[audio]\nmaster_volume = -10\n");
    UserConfig cfg(fs, logger);
    cfg.load();
    CHECK(cfg.audio().masterVolume == Catch::Approx(0.0f));
}

TEST_CASE("Settings: audio volume above 100 clamps to 1.0f", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("config/user.toml", "[audio]\nmaster_volume = 150\n");
    UserConfig cfg(fs, logger);
    cfg.load();
    CHECK(cfg.audio().masterVolume == Catch::Approx(1.0f));
}

TEST_CASE("Settings: audio defaults match spec (80/100/70/100/100)", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    UserConfig cfg(fs, logger);
    auto a = cfg.audio();
    CHECK(a.masterVolume == Catch::Approx(0.80f));
    CHECK(a.sfxVolume == Catch::Approx(1.00f));
    CHECK(a.musicVolume == Catch::Approx(0.70f));
    CHECK(a.voiceChatVolume == Catch::Approx(1.00f));
    CHECK(a.rwrVolume == Catch::Approx(1.00f));
}
