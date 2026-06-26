// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "config/UserConfig.h"
#include "mock_hal.h"

using namespace fl;

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
    CHECK(g.aaMode == AntiAliasingMode::TAA);
    CHECK(g.shadowQuality == ShadowQuality::High);
    CHECK(g.particleDensity == ParticleDensity::High);
    CHECK(g.ambientOcclusion == AmbientOcclusion::High);
    CHECK(g.skyQuality == SkyQuality::LUT);
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
// AntiAliasingMode round-trips
// ---------------------------------------------------------------------------

TEST_CASE("Settings: aaMode Off round-trip", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    GraphicsSettings gs;
    gs.aaMode = AntiAliasingMode::Off;
    makeAndSave(fs, logger, gs, {});
    CHECK(reload(fs).graphics().aaMode == AntiAliasingMode::Off);
}

TEST_CASE("Settings: aaMode FXAA round-trip", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    GraphicsSettings gs;
    gs.aaMode = AntiAliasingMode::FXAA;
    makeAndSave(fs, logger, gs, {});
    CHECK(reload(fs).graphics().aaMode == AntiAliasingMode::FXAA);
}

TEST_CASE("Settings: aaMode TAA round-trip", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    GraphicsSettings gs;
    gs.aaMode = AntiAliasingMode::TAA;
    makeAndSave(fs, logger, gs, {});
    CHECK(reload(fs).graphics().aaMode == AntiAliasingMode::TAA);
}

TEST_CASE("Settings: legacy msaa aa_mode migrates to TAA", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("config/user.toml", "[graphics]\naa_mode = \"msaa4x\"\n");
    UserConfig cfg(fs, logger);
    cfg.load();
    CHECK(cfg.graphics().aaMode == AntiAliasingMode::TAA);
}

TEST_CASE("Settings: ambientOcclusion Off round-trip", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    GraphicsSettings gs;
    gs.ambientOcclusion = AmbientOcclusion::Off;
    makeAndSave(fs, logger, gs, {});
    CHECK(reload(fs).graphics().ambientOcclusion == AmbientOcclusion::Off);
}

TEST_CASE("Settings: ambientOcclusion Low round-trip", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    GraphicsSettings gs;
    gs.ambientOcclusion = AmbientOcclusion::Low;
    makeAndSave(fs, logger, gs, {});
    CHECK(reload(fs).graphics().ambientOcclusion == AmbientOcclusion::Low);
}

TEST_CASE("Settings: unknown ao_mode falls back to High and emits Warn", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("config/user.toml", "[graphics]\nao_mode = \"insane\"\n");
    UserConfig cfg(fs, logger);
    cfg.load();
    CHECK(cfg.graphics().ambientOcclusion == AmbientOcclusion::High);
    CHECK_FALSE(logger.entries.empty());
}

TEST_CASE("Settings: skyQuality Procedural round-trip", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    GraphicsSettings gs;
    gs.skyQuality = SkyQuality::Procedural;
    makeAndSave(fs, logger, gs, {});
    CHECK(reload(fs).graphics().skyQuality == SkyQuality::Procedural);
}

TEST_CASE("Settings: skyQuality LUT round-trip", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    GraphicsSettings gs;
    gs.skyQuality = SkyQuality::LUT;
    makeAndSave(fs, logger, gs, {});
    CHECK(reload(fs).graphics().skyQuality == SkyQuality::LUT);
}

TEST_CASE("Settings: unknown sky_quality falls back to LUT and emits Warn", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("config/user.toml", "[graphics]\nsky_quality = \"raytraced\"\n");
    UserConfig cfg(fs, logger);
    cfg.load();
    CHECK(cfg.graphics().skyQuality == SkyQuality::LUT);
    CHECK_FALSE(logger.entries.empty());
}

TEST_CASE("Settings: anti_aliasing bool migration true -> FXAA", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("config/user.toml", "[graphics]\nanti_aliasing = true\n");
    UserConfig cfg(fs, logger);
    cfg.load();
    CHECK(cfg.graphics().aaMode == AntiAliasingMode::FXAA);
}

TEST_CASE("Settings: anti_aliasing bool migration false -> Off", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("config/user.toml", "[graphics]\nanti_aliasing = false\n");
    UserConfig cfg(fs, logger);
    cfg.load();
    CHECK(cfg.graphics().aaMode == AntiAliasingMode::Off);
}

// ---------------------------------------------------------------------------
// ShadowQuality round-trips
// ---------------------------------------------------------------------------

TEST_CASE("Settings: shadowQuality Off round-trip", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    GraphicsSettings gs;
    gs.shadowQuality = ShadowQuality::Off;
    makeAndSave(fs, logger, gs, {});
    CHECK(reload(fs).graphics().shadowQuality == ShadowQuality::Off);
}

TEST_CASE("Settings: shadowQuality Low round-trip", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    GraphicsSettings gs;
    gs.shadowQuality = ShadowQuality::Low;
    makeAndSave(fs, logger, gs, {});
    CHECK(reload(fs).graphics().shadowQuality == ShadowQuality::Low);
}

TEST_CASE("Settings: shadowQuality Medium round-trip", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    GraphicsSettings gs;
    gs.shadowQuality = ShadowQuality::Medium;
    makeAndSave(fs, logger, gs, {});
    CHECK(reload(fs).graphics().shadowQuality == ShadowQuality::Medium);
}

TEST_CASE("Settings: shadowQuality High round-trip", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    GraphicsSettings gs;
    gs.shadowQuality = ShadowQuality::High;
    makeAndSave(fs, logger, gs, {});
    CHECK(reload(fs).graphics().shadowQuality == ShadowQuality::High);
}

TEST_CASE("Settings: shadowQuality Ultra round-trip", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    GraphicsSettings gs;
    gs.shadowQuality = ShadowQuality::Ultra;
    makeAndSave(fs, logger, gs, {});
    CHECK(reload(fs).graphics().shadowQuality == ShadowQuality::Ultra);
}

// ---------------------------------------------------------------------------
// ParticleDensity round-trips
// ---------------------------------------------------------------------------

TEST_CASE("Settings: particleDensity Low round-trip", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    GraphicsSettings gs;
    gs.particleDensity = ParticleDensity::Low;
    makeAndSave(fs, logger, gs, {});
    CHECK(reload(fs).graphics().particleDensity == ParticleDensity::Low);
}

TEST_CASE("Settings: particleDensity Medium round-trip", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    GraphicsSettings gs;
    gs.particleDensity = ParticleDensity::Medium;
    makeAndSave(fs, logger, gs, {});
    CHECK(reload(fs).graphics().particleDensity == ParticleDensity::Medium);
}

TEST_CASE("Settings: particleDensity High round-trip", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    GraphicsSettings gs;
    gs.particleDensity = ParticleDensity::High;
    makeAndSave(fs, logger, gs, {});
    CHECK(reload(fs).graphics().particleDensity == ParticleDensity::High);
}

TEST_CASE("Settings: particleDensity Ultra round-trip", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    GraphicsSettings gs;
    gs.particleDensity = ParticleDensity::Ultra;
    makeAndSave(fs, logger, gs, {});
    CHECK(reload(fs).graphics().particleDensity == ParticleDensity::Ultra);
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

// ---------------------------------------------------------------------------
// DrawDistance Medium and High round-trips (previously untested enum arms)
// ---------------------------------------------------------------------------

TEST_CASE("Settings: DrawDistance Medium round-trip", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    GraphicsSettings gs;
    gs.drawDistance = DrawDistance::Medium;
    makeAndSave(fs, logger, gs, {});
    CHECK(reload(fs).graphics().drawDistance == DrawDistance::Medium);
}

TEST_CASE("Settings: DrawDistance High round-trip", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    GraphicsSettings gs;
    gs.drawDistance = DrawDistance::High;
    makeAndSave(fs, logger, gs, {});
    CHECK(reload(fs).graphics().drawDistance == DrawDistance::High);
}

// ---------------------------------------------------------------------------
// Error-path branches in load() and save()
// ---------------------------------------------------------------------------

TEST_CASE("Settings: load returns false for invalid TOML", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("config/user.toml", "this is {{{ invalid toml");
    UserConfig cfg(fs, logger);
    REQUIRE_FALSE(cfg.load());
    CHECK(logger.hasMessage(LogLevel::Warn, "failed to parse"));
}

TEST_CASE("Settings: load warns and defaults on unknown log_level string", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("config/user.toml", "[engine]\nlog_level = \"bogus\"\n");
    UserConfig cfg(fs, logger);
    REQUIRE(cfg.load());
    CHECK(cfg.logLevel() == LogLevel::Info);
    CHECK(logger.hasMessage(LogLevel::Warn, "bogus"));
}

TEST_CASE("Settings: save returns false when createDirectory fails", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    fs.createDirectoryResult = false;
    UserConfig cfg(fs, logger);
    REQUIRE_FALSE(cfg.save());
    CHECK(logger.hasMessage(LogLevel::Warn, "failed to create config directory"));
}

TEST_CASE("Settings: save returns false when tmp file open fails", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    fs.failWriteOpen = true;
    UserConfig cfg(fs, logger);
    REQUIRE_FALSE(cfg.save());
    CHECK(logger.hasMessage(LogLevel::Warn, "failed to open tmp file"));
}

TEST_CASE("Settings: save returns false when rename fails", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    fs.renameResult = false;
    UserConfig cfg(fs, logger);
    REQUIRE_FALSE(cfg.save());
    CHECK(logger.hasMessage(LogLevel::Warn, "failed to rename tmp file"));
}

// ---------------------------------------------------------------------------
// [client] section — motdDisplayS
// ---------------------------------------------------------------------------

TEST_CASE("Settings: motdDisplayS missing uses default 15", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("config/user.toml", "[first_run]\ncompleted = true\n");
    UserConfig cfg(fs, logger);
    cfg.load();
    CHECK(cfg.client().motdDisplayS == 15u);
    CHECK(logger.entries.empty());
}

TEST_CASE("Settings: motdDisplayS 0 accepted as persistent", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("config/user.toml", "[client]\nmotd_display_s = 0\n");
    UserConfig cfg(fs, logger);
    cfg.load();
    CHECK(cfg.client().motdDisplayS == 0u);
    CHECK(logger.entries.empty());
}

TEST_CASE("Settings: motdDisplayS at upper boundary 3600 accepted without Warn", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("config/user.toml", "[client]\nmotd_display_s = 3600\n");
    UserConfig cfg(fs, logger);
    cfg.load();
    CHECK(cfg.client().motdDisplayS == 3600u);
    CHECK(logger.entries.empty());
}

TEST_CASE("Settings: motdDisplayS above 3600 clamps and emits Warn", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("config/user.toml", "[client]\nmotd_display_s = 9999\n");
    UserConfig cfg(fs, logger);
    cfg.load();
    CHECK(cfg.client().motdDisplayS == 3600u);
    CHECK(logger.hasMessage(LogLevel::Warn, "motd_display_s"));
}

TEST_CASE("Settings: motdDisplayS negative clamps to 0 and emits Warn", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("config/user.toml", "[client]\nmotd_display_s = -1\n");
    UserConfig cfg(fs, logger);
    cfg.load();
    CHECK(cfg.client().motdDisplayS == 0u);
    CHECK(logger.hasMessage(LogLevel::Warn, "motd_display_s"));
}

TEST_CASE("Settings: motdDisplayS in-range round-trips", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    ClientSettings cs;
    cs.motdDisplayS = 30;
    UserConfig cfg(fs, logger);
    cfg.setClient(cs);
    cfg.save();
    CHECK(reload(fs).client().motdDisplayS == 30u);
}

TEST_CASE("Settings: [client] section does not corrupt other sections", "[settings]") {
    MockFilesystem fs;
    MockLogger logger;
    ClientSettings cs;
    cs.motdDisplayS = 45;
    GraphicsSettings gs;
    gs.cockpitFov = 75;
    UserConfig cfg(fs, logger);
    cfg.setClient(cs);
    cfg.setGraphics(gs);
    cfg.save();
    auto loaded = reload(fs);
    CHECK(loaded.client().motdDisplayS == 45u);
    CHECK(loaded.graphics().cockpitFov == 75);
}
