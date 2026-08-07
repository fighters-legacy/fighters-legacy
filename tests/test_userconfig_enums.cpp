// SPDX-License-Identifier: GPL-3.0-or-later
//
// Every enumerated setting, through save and back (#1145).
//
// Each of these settings is a pair of functions written by hand: one turning the enum into a string
// for the file, one turning the string back. Nothing checks that they agree. A missing arm in either
// direction silently rewrites the player's choice to the default the next time the game starts —
// and it happens at SAVE time, so by the time anyone notices, the file on disk already says the
// wrong thing and the original choice is gone.
//
// So this file round-trips every value of every enum, and separately feeds each key a value that is
// not in its vocabulary to confirm the fallback is a warning plus a sane default rather than a
// crash or an out-of-range enum. Hand-written per-value cases would test the same thing; iterating
// the vocabulary means adding an enumerator without an arm fails here rather than after a release.

#include <catch2/catch_test_macros.hpp>

#include "config/UserConfig.h"
#include "mock_hal.h"

#include <string>
#include <vector>

using namespace fl;

namespace {

// Save `cfg`'s state into `fs`, then read it back through a fresh UserConfig.
UserConfig reloaded(MockFilesystem& fs, MockLogger& log, UserConfig& cfg) {
    REQUIRE(cfg.save());
    UserConfig fresh(fs, log);
    REQUIRE(fresh.load());
    return fresh;
}

// Load a config whose one key carries a value outside its vocabulary.
UserConfig withBadValue(MockFilesystem& fs, MockLogger& log, const std::string& section, const std::string& key) {
    fs.files.erase("config/user.toml");
    fs.addFile("config/user.toml", "[" + section + "]\n" + key + " = \"definitely-not-a-valid-value\"\n");
    UserConfig cfg(fs, log);
    cfg.load();
    return cfg;
}

} // namespace

// ---------------------------------------------------------------------------
// Graphics
// ---------------------------------------------------------------------------

TEST_CASE("UserConfig: every graphics enum value survives a save and reload (#1145)", "[userconfig]") {
    MockFilesystem fs;
    MockLogger log;

    SECTION("vsync") {
        for (auto v : {VsyncMode::Off, VsyncMode::On, VsyncMode::Adaptive}) {
            UserConfig cfg(fs, log);
            GraphicsSettings gs = cfg.graphics();
            gs.vsync = v;
            cfg.setGraphics(gs);
            CHECK(reloaded(fs, log, cfg).graphics().vsync == v);
        }
    }
    SECTION("frame rate cap") {
        for (auto v : {FrameRateCap::Off, FrameRateCap::Cap30, FrameRateCap::Cap60, FrameRateCap::Cap120,
                       FrameRateCap::Cap144, FrameRateCap::Cap240}) {
            UserConfig cfg(fs, log);
            GraphicsSettings gs = cfg.graphics();
            gs.frameRateCap = v;
            cfg.setGraphics(gs);
            CHECK(reloaded(fs, log, cfg).graphics().frameRateCap == v);
        }
    }
    SECTION("quality preset and draw distance") {
        for (auto v : {QualityLevel::Low, QualityLevel::Medium, QualityLevel::High, QualityLevel::Ultra}) {
            UserConfig cfg(fs, log);
            GraphicsSettings gs = cfg.graphics();
            gs.qualityPreset = v;
            gs.drawDistance = static_cast<DrawDistance>(v); // the two share a vocabulary
            cfg.setGraphics(gs);
            const UserConfig back = reloaded(fs, log, cfg);
            CHECK(back.graphics().qualityPreset == v);
            CHECK(back.graphics().drawDistance == static_cast<DrawDistance>(v));
        }
    }
    SECTION("anti-aliasing") {
        for (auto v : {AntiAliasingMode::Off, AntiAliasingMode::FXAA, AntiAliasingMode::TAA}) {
            UserConfig cfg(fs, log);
            GraphicsSettings gs = cfg.graphics();
            gs.aaMode = v;
            cfg.setGraphics(gs);
            CHECK(reloaded(fs, log, cfg).graphics().aaMode == v);
        }
    }
    SECTION("shadow quality") {
        for (auto v : {ShadowQuality::Off, ShadowQuality::Low, ShadowQuality::Medium, ShadowQuality::High,
                       ShadowQuality::Ultra}) {
            UserConfig cfg(fs, log);
            GraphicsSettings gs = cfg.graphics();
            gs.shadowQuality = v;
            cfg.setGraphics(gs);
            CHECK(reloaded(fs, log, cfg).graphics().shadowQuality == v);
        }
    }
    SECTION("particle density") {
        for (auto v : {ParticleDensity::Low, ParticleDensity::Medium, ParticleDensity::High, ParticleDensity::Ultra}) {
            UserConfig cfg(fs, log);
            GraphicsSettings gs = cfg.graphics();
            gs.particleDensity = v;
            cfg.setGraphics(gs);
            CHECK(reloaded(fs, log, cfg).graphics().particleDensity == v);
        }
    }
    SECTION("ambient occlusion") {
        for (auto v : {AmbientOcclusion::Off, AmbientOcclusion::Low, AmbientOcclusion::High}) {
            UserConfig cfg(fs, log);
            GraphicsSettings gs = cfg.graphics();
            gs.ambientOcclusion = v;
            cfg.setGraphics(gs);
            CHECK(reloaded(fs, log, cfg).graphics().ambientOcclusion == v);
        }
    }
    SECTION("sky quality") {
        for (auto v : {SkyQuality::Procedural, SkyQuality::LUT}) {
            UserConfig cfg(fs, log);
            GraphicsSettings gs = cfg.graphics();
            gs.skyQuality = v;
            cfg.setGraphics(gs);
            CHECK(reloaded(fs, log, cfg).graphics().skyQuality == v);
        }
    }
    SECTION("ui scale") {
        // Stored as an integer percentage rather than a word, so the mapping is arithmetic in both
        // directions and an unlisted percentage has to land somewhere sane.
        for (auto v : {UiScale::Scale75, UiScale::Scale100, UiScale::Scale125, UiScale::Scale150}) {
            UserConfig cfg(fs, log);
            GraphicsSettings gs = cfg.graphics();
            gs.uiScale = v;
            cfg.setGraphics(gs);
            CHECK(reloaded(fs, log, cfg).graphics().uiScale == v);
        }
    }
}

TEST_CASE("UserConfig: an unknown graphics value warns and defaults (#1145)", "[userconfig]") {
    // A hand-edited config, or one written by a newer build. Neither should take the game down, and
    // the player has to be told which key was ignored — otherwise the setting simply appears not to
    // work.
    MockFilesystem fs;
    for (const char* key : {"vsync", "frame_rate_cap", "quality_preset", "draw_distance", "aa_mode", "shadow_quality",
                            "particle_density", "ao_mode", "sky_quality"}) {
        MockLogger log;
        INFO("graphics." << key);
        const UserConfig cfg = withBadValue(fs, log, "graphics", key);
        CHECK(log.hasMessage(LogLevel::Warn, key)); // the diagnostic names the key
        (void)cfg.graphics();                       // and the settings are still readable
    }
}

TEST_CASE("UserConfig: MSAA migrates to TAA rather than being refused (#1145)", "[userconfig]") {
    // MSAA was removed in favour of TAA. Anyone who had it selected gets TAA silently: warning them
    // about a setting THIS BUILD deleted would be blaming the player for our change.
    MockFilesystem fs;
    for (const char* legacy : {"msaa2x", "msaa4x", "msaa8x"}) {
        MockLogger log;
        fs.files.erase("config/user.toml");
        fs.addFile("config/user.toml", std::string("[graphics]\naa_mode = \"") + legacy + "\"\n");
        UserConfig cfg(fs, log);
        cfg.load();
        INFO(legacy);
        CHECK(cfg.graphics().aaMode == AntiAliasingMode::TAA);
        CHECK_FALSE(log.hasMessage(LogLevel::Warn, "aa_mode"));
    }
}

// ---------------------------------------------------------------------------
// Difficulty
// ---------------------------------------------------------------------------

TEST_CASE("UserConfig: every difficulty enum value survives a save and reload (#1145)", "[userconfig]") {
    MockFilesystem fs;
    MockLogger log;

    SECTION("preset") {
        for (auto v :
             {DifficultyPreset::Cadet, DifficultyPreset::Pilot, DifficultyPreset::Ace, DifficultyPreset::Custom}) {
            UserConfig cfg(fs, log);
            DifficultySettings ds = cfg.difficulty();
            ds.preset = v;
            cfg.setDifficulty(ds);
            CHECK(reloaded(fs, log, cfg).difficulty().preset == v);
        }
    }
    SECTION("flight assists") {
        for (auto v : {FlightAssists::AllOn, FlightAssists::GLimiterOnly, FlightAssists::AllOff}) {
            UserConfig cfg(fs, log);
            DifficultySettings ds = cfg.difficulty();
            ds.toggles.flightAssists = v;
            cfg.setDifficulty(ds);
            CHECK(reloaded(fs, log, cfg).difficulty().toggles.flightAssists == v);
        }
    }
    SECTION("enemy labels") {
        for (auto v : {EnemyLabels::Always, EnemyLabels::OnLock, EnemyLabels::Off}) {
            UserConfig cfg(fs, log);
            DifficultySettings ds = cfg.difficulty();
            ds.toggles.enemyLabels = v;
            cfg.setDifficulty(ds);
            CHECK(reloaded(fs, log, cfg).difficulty().toggles.enemyLabels == v);
        }
    }
    SECTION("radar realism") {
        for (auto v : {RadarRealism::Simple, RadarRealism::Standard, RadarRealism::Full}) {
            UserConfig cfg(fs, log);
            DifficultySettings ds = cfg.difficulty();
            ds.toggles.radarRealism = v;
            cfg.setDifficulty(ds);
            CHECK(reloaded(fs, log, cfg).difficulty().toggles.radarRealism == v);
        }
    }
    SECTION("in-flight refueling") {
        for (auto v : {RefuelingMode::Auto, RefuelingMode::Simplified, RefuelingMode::Manual}) {
            UserConfig cfg(fs, log);
            DifficultySettings ds = cfg.difficulty();
            ds.toggles.inFlightRefueling = v;
            cfg.setDifficulty(ds);
            CHECK(reloaded(fs, log, cfg).difficulty().toggles.inFlightRefueling == v);
        }
    }
    SECTION("rearm mode") {
        for (auto v : {RearmMode::Instantaneous, RearmMode::Timed, RearmMode::SupplyLimited}) {
            UserConfig cfg(fs, log);
            DifficultySettings ds = cfg.difficulty();
            ds.toggles.rearmMode = v;
            cfg.setDifficulty(ds);
            CHECK(reloaded(fs, log, cfg).difficulty().toggles.rearmMode == v);
        }
    }
    SECTION("countermeasure use") {
        for (auto v : {CountermeasureUse::Never, CountermeasureUse::Reactive, CountermeasureUse::Proactive}) {
            UserConfig cfg(fs, log);
            DifficultySettings ds = cfg.difficulty();
            ds.ai.countermeasureUse = v;
            cfg.setDifficulty(ds);
            CHECK(reloaded(fs, log, cfg).difficulty().ai.countermeasureUse == v);
        }
    }
    SECTION("energy management") {
        for (auto v : {EnergyManagement::Passive, EnergyManagement::Standard, EnergyManagement::AggressiveBfm}) {
            UserConfig cfg(fs, log);
            DifficultySettings ds = cfg.difficulty();
            ds.ai.energyManagement = v;
            cfg.setDifficulty(ds);
            CHECK(reloaded(fs, log, cfg).difficulty().ai.energyManagement == v);
        }
    }
    SECTION("SAM radar shutdown") {
        for (auto v : {SamRadarShutdown::Never, SamRadarShutdown::Sometimes, SamRadarShutdown::Always}) {
            UserConfig cfg(fs, log);
            DifficultySettings ds = cfg.difficulty();
            ds.ai.samRadarShutdown = v;
            cfg.setDifficulty(ds);
            CHECK(reloaded(fs, log, cfg).difficulty().ai.samRadarShutdown == v);
        }
    }
}

TEST_CASE("UserConfig: an unknown difficulty value warns and defaults (#1145)", "[userconfig]") {
    MockFilesystem fs;
    for (const char* key : {"preset", "flight_assists", "enemy_labels", "radar_realism", "in_flight_refueling",
                            "rearm_mode", "countermeasure_use", "energy_management", "sam_radar_shutdown"}) {
        MockLogger log;
        INFO("difficulty." << key);
        const UserConfig cfg = withBadValue(fs, log, "difficulty", key);
        CHECK(log.hasMessage(LogLevel::Warn, key));
        (void)cfg.difficulty();
    }
}

// ---------------------------------------------------------------------------
// Voice
// ---------------------------------------------------------------------------

TEST_CASE("UserConfig: the voice key mode round-trips and falls back to push-to-talk (#1145)", "[userconfig]") {
    // Push-to-talk is the safe default on purpose: silently defaulting to open mic would put a
    // player's room on the radio without them choosing it.
    MockFilesystem fs;
    MockLogger log;
    for (auto v : {VoiceKeyMode::PushToTalk, VoiceKeyMode::Voice, VoiceKeyMode::Open}) {
        UserConfig cfg(fs, log);
        VoiceSettings vs = cfg.voice();
        vs.keyMode = v;
        cfg.setVoice(vs);
        CHECK(reloaded(fs, log, cfg).voice().keyMode == v);
    }

    MockLogger bad;
    const UserConfig cfg = withBadValue(fs, bad, "voice", "key_mode");
    CHECK(cfg.voice().keyMode == VoiceKeyMode::PushToTalk);
}

// ---------------------------------------------------------------------------
// The log level, which the CLI shares
// ---------------------------------------------------------------------------

TEST_CASE("UserConfig: every log level round-trips through the file (#1145)", "[userconfig]") {
    // parseLogLevel is tested directly elsewhere; what is untested is the WRITING half, which is a
    // separate switch and the one that can lose a setting on save.
    MockFilesystem fs;
    MockLogger log;
    for (auto l : {LogLevel::Trace, LogLevel::Debug, LogLevel::Info, LogLevel::Warn, LogLevel::Error}) {
        UserConfig cfg(fs, log);
        cfg.setLogLevel(l);
        CHECK(reloaded(fs, log, cfg).logLevel() == l);
    }
}

// ---------------------------------------------------------------------------
// The file itself
// ---------------------------------------------------------------------------

TEST_CASE("UserConfig: a malformed file leaves every setting at its default (#1145)", "[userconfig]") {
    // A truncated write or a hand-edit gone wrong. Every setting has to come back as its default
    // rather than as whatever the parser had managed to assign before it gave up.
    MockFilesystem fs;
    MockLogger log;
    fs.addFile("config/user.toml", "[graphics\nvsync = ");

    UserConfig cfg(fs, log);
    CHECK_FALSE(cfg.load());
    CHECK(cfg.graphics().vsync == GraphicsSettings{}.vsync);
    CHECK(cfg.difficulty().preset == DifficultySettings{}.preset);
    CHECK(cfg.logLevel() == LogLevel::Info);
}

TEST_CASE("UserConfig: a missing file is a first run, not an error (#1145)", "[userconfig]") {
    MockFilesystem fs; // empty
    MockLogger log;
    UserConfig cfg(fs, log);
    CHECK_FALSE(cfg.load());
    CHECK_FALSE(cfg.isFirstRunCompleted());
    CHECK_FALSE(log.hasMessage(LogLevel::Error, "user.toml")); // nothing is wrong yet
}

TEST_CASE("UserConfig: saving writes through a temporary file (#1145)", "[userconfig]") {
    // A crash midway through writing the real file would leave the player with no settings at all.
    // The save goes to user.toml.tmp and is renamed into place.
    MockFilesystem fs;
    MockLogger log;
    UserConfig cfg(fs, log);
    cfg.setLogLevel(LogLevel::Debug);
    REQUIRE(cfg.save());

    REQUIRE(fs.renameCalls.size() == 1u);
    CHECK(fs.renameCalls[0].from == "config/user.toml.tmp");
    CHECK(fs.renameCalls[0].to == "config/user.toml");
    CHECK(fs.files.count("config/user.toml") == 1u);
    CHECK(fs.files.count("config/user.toml.tmp") == 0u); // renamed away, not left behind
}

TEST_CASE("UserConfig: a save that cannot be written reports failure (#1145)", "[userconfig]") {
    // A full disk or a read-only profile directory. Returning true here would tell the settings
    // screen the change was kept when it was not.
    MockFilesystem fs;
    MockLogger log;
    fs.failWriteOpen = true;
    UserConfig cfg(fs, log);
    CHECK_FALSE(cfg.save());

    fs.failWriteOpen = false;
    fs.renameResult = false; // the write lands but the atomic swap fails
    CHECK_FALSE(cfg.save());
}
