// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "config/UserConfig.h"
#include "difficulty/DifficultyMultipliers.h"
#include "mock_hal.h"

using namespace fl;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static constexpr const char* kFullToml =
    "[cadet]\nreaction_time_s = 1.5\naim_error_deg = 8.0\n"
    "radar_sensor_range = 0.5\nflight_assists = \"all_on\"\naim_assist = true\n"
    "enemy_labels = \"always\"\nradar_realism = \"simple\"\nblackout_redout = false\n"
    "fuel_consumption = false\nin_flight_refueling = \"auto\"\nfriendly_fire = false\n"
    "crash_damage = false\nrearm_mode = \"instantaneous\"\n"
    "countermeasure_use = \"never\"\nenergy_management = \"passive\"\n"
    "sam_engagement_range = 0.6\nsam_radar_shutdown = \"never\"\n"
    "[pilot]\nreaction_time_s = 0.8\naim_error_deg = 4.0\n"
    "radar_sensor_range = 0.8\nflight_assists = \"g_limiter_only\"\naim_assist = true\n"
    "enemy_labels = \"on_lock\"\nradar_realism = \"standard\"\nblackout_redout = true\n"
    "fuel_consumption = true\nin_flight_refueling = \"simplified\"\nfriendly_fire = false\n"
    "crash_damage = true\nrearm_mode = \"timed\"\n"
    "countermeasure_use = \"reactive\"\nenergy_management = \"standard\"\n"
    "sam_engagement_range = 0.8\nsam_radar_shutdown = \"sometimes\"\n"
    "[ace]\nreaction_time_s = 0.3\naim_error_deg = 1.0\n"
    "radar_sensor_range = 1.0\nflight_assists = \"all_off\"\naim_assist = false\n"
    "enemy_labels = \"off\"\nradar_realism = \"full\"\nblackout_redout = true\n"
    "fuel_consumption = true\nin_flight_refueling = \"manual\"\nfriendly_fire = true\n"
    "crash_damage = true\nrearm_mode = \"supply_limited\"\n"
    "countermeasure_use = \"proactive\"\nenergy_management = \"aggressive_bfm\"\n"
    "sam_engagement_range = 1.0\nsam_radar_shutdown = \"always\"\n";

// ---------------------------------------------------------------------------
// DifficultySettings struct defaults
// ---------------------------------------------------------------------------

TEST_CASE("DifficultySettings default preset is Cadet", "[difficulty]") {
    CHECK(DifficultySettings{}.preset == DifficultyPreset::Cadet);
}

TEST_CASE("DifficultySettings default toggles match Cadet spec", "[difficulty]") {
    GameplayToggles t{};
    CHECK(t.flightAssists == FlightAssists::AllOn);
    CHECK(t.aimAssist == true);
    CHECK(t.enemyLabels == EnemyLabels::Always);
    CHECK(t.radarRealism == RadarRealism::Simple);
    CHECK(t.blackoutRedout == false);
    CHECK(t.fuelConsumption == false);
    CHECK(t.inFlightRefueling == RefuelingMode::Auto);
    CHECK(t.friendlyFire == false);
    CHECK(t.crashDamage == false);
    CHECK(t.rearmMode == RearmMode::Instantaneous);
}

TEST_CASE("DifficultySettings default AI scaling matches Cadet spec", "[difficulty]") {
    AiScaling ai{};
    CHECK(ai.reactionTimeS == Catch::Approx(1.5f));
    CHECK(ai.aimErrorDeg == Catch::Approx(8.0f));
    CHECK(ai.radarSensorRange == Catch::Approx(0.50f));
    CHECK(ai.countermeasureUse == CountermeasureUse::Never);
    CHECK(ai.energyManagement == EnergyManagement::Passive);
    CHECK(ai.samEngagementRange == Catch::Approx(0.60f));
    CHECK(ai.samRadarShutdown == SamRadarShutdown::Never);
}

TEST_CASE("DifficultySettings invulnerability and unlimitedWeapons default to false", "[difficulty]") {
    GameplayToggles t{};
    CHECK(t.invulnerability == false);
    CHECK(t.unlimitedWeapons == false);
}

// ---------------------------------------------------------------------------
// DifficultyMultipliers::defaults()
// ---------------------------------------------------------------------------

TEST_CASE("DifficultyMultipliers defaults Cadet preset matches spec", "[difficulty]") {
    auto dm = DifficultyMultipliers::defaults();
    const auto& c = dm.preset(DifficultyPreset::Cadet);
    CHECK(c.flightAssists == FlightAssists::AllOn);
    CHECK(c.aimAssist == true);
    CHECK(c.enemyLabels == EnemyLabels::Always);
    CHECK(c.radarRealism == RadarRealism::Simple);
    CHECK(c.blackoutRedout == false);
    CHECK(c.fuelConsumption == false);
    CHECK(c.inFlightRefueling == RefuelingMode::Auto);
    CHECK(c.friendlyFire == false);
    CHECK(c.crashDamage == false);
    CHECK(c.rearmMode == RearmMode::Instantaneous);
    CHECK(c.reactionTimeS == Catch::Approx(1.5f));
    CHECK(c.aimErrorDeg == Catch::Approx(8.0f));
    CHECK(c.radarSensorRange == Catch::Approx(0.50f));
    CHECK(c.countermeasureUse == CountermeasureUse::Never);
    CHECK(c.energyManagement == EnergyManagement::Passive);
    CHECK(c.samEngagementRange == Catch::Approx(0.60f));
    CHECK(c.samRadarShutdown == SamRadarShutdown::Never);
}

TEST_CASE("DifficultyMultipliers defaults Pilot preset matches spec", "[difficulty]") {
    auto dm = DifficultyMultipliers::defaults();
    const auto& p = dm.preset(DifficultyPreset::Pilot);
    CHECK(p.flightAssists == FlightAssists::GLimiterOnly);
    CHECK(p.aimAssist == true);
    CHECK(p.enemyLabels == EnemyLabels::OnLock);
    CHECK(p.radarRealism == RadarRealism::Standard);
    CHECK(p.blackoutRedout == true);
    CHECK(p.fuelConsumption == true);
    CHECK(p.inFlightRefueling == RefuelingMode::Simplified);
    CHECK(p.friendlyFire == false);
    CHECK(p.crashDamage == true);
    CHECK(p.rearmMode == RearmMode::Timed);
    CHECK(p.reactionTimeS == Catch::Approx(0.8f));
    CHECK(p.aimErrorDeg == Catch::Approx(4.0f));
    CHECK(p.radarSensorRange == Catch::Approx(0.80f));
    CHECK(p.countermeasureUse == CountermeasureUse::Reactive);
    CHECK(p.energyManagement == EnergyManagement::Standard);
    CHECK(p.samEngagementRange == Catch::Approx(0.80f));
    CHECK(p.samRadarShutdown == SamRadarShutdown::Sometimes);
}

TEST_CASE("DifficultyMultipliers defaults Ace preset matches spec", "[difficulty]") {
    auto dm = DifficultyMultipliers::defaults();
    const auto& a = dm.preset(DifficultyPreset::Ace);
    CHECK(a.flightAssists == FlightAssists::AllOff);
    CHECK(a.aimAssist == false);
    CHECK(a.enemyLabels == EnemyLabels::Off);
    CHECK(a.radarRealism == RadarRealism::Full);
    CHECK(a.blackoutRedout == true);
    CHECK(a.fuelConsumption == true);
    CHECK(a.inFlightRefueling == RefuelingMode::Manual);
    CHECK(a.friendlyFire == true);
    CHECK(a.crashDamage == true);
    CHECK(a.rearmMode == RearmMode::SupplyLimited);
    CHECK(a.reactionTimeS == Catch::Approx(0.3f));
    CHECK(a.aimErrorDeg == Catch::Approx(1.0f));
    CHECK(a.radarSensorRange == Catch::Approx(1.00f));
    CHECK(a.countermeasureUse == CountermeasureUse::Proactive);
    CHECK(a.energyManagement == EnergyManagement::AggressiveBfm);
    CHECK(a.samEngagementRange == Catch::Approx(1.00f));
    CHECK(a.samRadarShutdown == SamRadarShutdown::Always);
}

// ---------------------------------------------------------------------------
// DifficultyMultipliers::load(IFilesystem&, ILogger&)
// ---------------------------------------------------------------------------

TEST_CASE("DifficultyMultipliers load parses full TOML with no warnings", "[difficulty]") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("data/difficulty.toml", kFullToml);

    auto dm = DifficultyMultipliers::load(fs, logger);
    CHECK(logger.entries.empty());
    CHECK(dm.preset(DifficultyPreset::Cadet).reactionTimeS == Catch::Approx(1.5f));
    CHECK(dm.preset(DifficultyPreset::Pilot).aimErrorDeg == Catch::Approx(4.0f));
    CHECK(dm.preset(DifficultyPreset::Ace).samRadarShutdown == SamRadarShutdown::Always);
}

TEST_CASE("DifficultyMultipliers load returns defaults silently when file missing", "[difficulty]") {
    MockFilesystem fs;
    MockLogger logger;

    auto dm = DifficultyMultipliers::load(fs, logger);
    CHECK(logger.entries.empty());
    CHECK(dm.preset(DifficultyPreset::Cadet).reactionTimeS == Catch::Approx(1.5f));
}

TEST_CASE("DifficultyMultipliers load warns and returns defaults on malformed TOML", "[difficulty]") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("data/difficulty.toml", "this is {{{ not toml");

    auto dm = DifficultyMultipliers::load(fs, logger);
    CHECK(logger.hasMessage(LogLevel::Warn, "failed to parse"));
    CHECK(dm.preset(DifficultyPreset::Ace).reactionTimeS == Catch::Approx(0.3f));
}

TEST_CASE("DifficultyMultipliers load warns and falls back when cadet section missing", "[difficulty]") {
    MockFilesystem fs;
    MockLogger logger;
    // Only pilot and ace sections present
    fs.addFile("data/difficulty.toml", "[pilot]\nreaction_time_s = 0.8\n[ace]\nreaction_time_s = 0.3\n");

    auto dm = DifficultyMultipliers::load(fs, logger);
    CHECK(logger.hasMessage(LogLevel::Warn, "cadet"));
    CHECK(dm.preset(DifficultyPreset::Cadet).reactionTimeS == Catch::Approx(1.5f));
    CHECK(dm.preset(DifficultyPreset::Pilot).reactionTimeS == Catch::Approx(0.8f));
}

TEST_CASE("DifficultyMultipliers load warns on unknown flight_assists string", "[difficulty]") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("data/difficulty.toml", "[cadet]\nflight_assists = \"turbo\"\n[pilot]\n[ace]\n");

    auto dm = DifficultyMultipliers::load(fs, logger);
    CHECK(logger.hasMessage(LogLevel::Warn, "turbo"));
    CHECK(dm.preset(DifficultyPreset::Cadet).flightAssists == FlightAssists::AllOn);
}

TEST_CASE("DifficultyMultipliers load warns and clamps out-of-range reaction_time_s", "[difficulty]") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("data/difficulty.toml", "[cadet]\nreaction_time_s = -5.0\n[pilot]\n[ace]\n");

    auto dm = DifficultyMultipliers::load(fs, logger);
    CHECK(logger.hasMessage(LogLevel::Warn, "reaction_time_s"));
    CHECK(dm.preset(DifficultyPreset::Cadet).reactionTimeS == Catch::Approx(0.0f));
}

// ---------------------------------------------------------------------------
// DifficultyMultipliers::applyPreset
// ---------------------------------------------------------------------------

TEST_CASE("DifficultyMultipliers applyPreset Ace overwrites fields and stamps preset", "[difficulty]") {
    auto dm = DifficultyMultipliers::defaults();
    DifficultySettings ds;
    ds.toggles.invulnerability = true;
    ds.toggles.unlimitedWeapons = true;

    dm.applyPreset(DifficultyPreset::Ace, ds);

    CHECK(ds.preset == DifficultyPreset::Ace);
    CHECK(ds.toggles.flightAssists == FlightAssists::AllOff);
    CHECK(ds.toggles.aimAssist == false);
    CHECK(ds.toggles.friendlyFire == true);
    CHECK(ds.ai.reactionTimeS == Catch::Approx(0.3f));
    CHECK(ds.toggles.invulnerability == true);  // preserved
    CHECK(ds.toggles.unlimitedWeapons == true); // preserved
}

TEST_CASE("DifficultyMultipliers applyPreset Pilot stamps preset == Pilot", "[difficulty]") {
    auto dm = DifficultyMultipliers::defaults();
    DifficultySettings ds;

    dm.applyPreset(DifficultyPreset::Pilot, ds);

    CHECK(ds.preset == DifficultyPreset::Pilot);
    CHECK(ds.toggles.flightAssists == FlightAssists::GLimiterOnly);
}

// ---------------------------------------------------------------------------
// UserConfig difficulty round-trips
// ---------------------------------------------------------------------------

static UserConfig makeAndSaveDifficulty(MockFilesystem& fs, MockLogger& logger, const DifficultySettings& ds) {
    UserConfig cfg(fs, logger);
    cfg.setDifficulty(ds);
    cfg.save();
    return cfg;
}

static DifficultySettings reloadDifficulty(MockFilesystem& fs) {
    MockLogger dummy;
    UserConfig cfg(fs, dummy);
    cfg.load();
    return cfg.difficulty();
}

TEST_CASE("Difficulty UserConfig round-trip for Pilot preset", "[difficulty]") {
    MockFilesystem fs;
    MockLogger logger;

    DifficultySettings ds;
    auto dm = DifficultyMultipliers::defaults();
    dm.applyPreset(DifficultyPreset::Pilot, ds);

    makeAndSaveDifficulty(fs, logger, ds);
    auto ds2 = reloadDifficulty(fs);

    CHECK(ds2.preset == DifficultyPreset::Pilot);
    CHECK(ds2.toggles.flightAssists == FlightAssists::GLimiterOnly);
    CHECK(ds2.toggles.aimAssist == true);
    CHECK(ds2.ai.reactionTimeS == Catch::Approx(0.8f));
    CHECK(ds2.ai.aimErrorDeg == Catch::Approx(4.0f));
}

TEST_CASE("Difficulty UserConfig round-trip for Custom preset", "[difficulty]") {
    MockFilesystem fs;
    MockLogger logger;

    DifficultySettings ds;
    ds.preset = DifficultyPreset::Custom;
    ds.toggles.aimAssist = false;

    makeAndSaveDifficulty(fs, logger, ds);
    auto ds2 = reloadDifficulty(fs);

    CHECK(ds2.preset == DifficultyPreset::Custom);
    CHECK(ds2.toggles.aimAssist == false);
    CHECK(logger.entries.empty()); // "custom" is valid, no Warn
}

TEST_CASE("Difficulty UserConfig preserves invulnerability and unlimitedWeapons", "[difficulty]") {
    MockFilesystem fs;
    MockLogger logger;

    DifficultySettings ds;
    ds.toggles.invulnerability = true;
    ds.toggles.unlimitedWeapons = true;

    makeAndSaveDifficulty(fs, logger, ds);
    auto ds2 = reloadDifficulty(fs);

    CHECK(ds2.toggles.invulnerability == true);
    CHECK(ds2.toggles.unlimitedWeapons == true);
}

TEST_CASE("Difficulty UserConfig Ace AI scaling floats round-trip", "[difficulty]") {
    MockFilesystem fs;
    MockLogger logger;

    DifficultySettings ds;
    auto dm = DifficultyMultipliers::defaults();
    dm.applyPreset(DifficultyPreset::Ace, ds);

    makeAndSaveDifficulty(fs, logger, ds);
    auto ds2 = reloadDifficulty(fs);

    CHECK(ds2.ai.reactionTimeS == Catch::Approx(0.3f));
    CHECK(ds2.ai.aimErrorDeg == Catch::Approx(1.0f));
    CHECK(ds2.ai.radarSensorRange == Catch::Approx(1.0f));
    CHECK(ds2.ai.samEngagementRange == Catch::Approx(1.0f));
    CHECK(ds2.ai.samRadarShutdown == SamRadarShutdown::Always);
}

TEST_CASE("Difficulty UserConfig missing difficulty section loads defaults with no Warn", "[difficulty]") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("config/user.toml", "[first_run]\ncompleted = true\n");
    UserConfig cfg(fs, logger);
    cfg.load();

    CHECK(logger.entries.empty());
    CHECK(cfg.difficulty().preset == DifficultyPreset::Cadet);
    CHECK(cfg.difficulty().toggles.aimAssist == true);
    CHECK(cfg.difficulty().ai.reactionTimeS == Catch::Approx(1.5f));
}

TEST_CASE("Difficulty UserConfig unknown preset string falls back to Cadet with Warn", "[difficulty]") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("config/user.toml", "[difficulty]\npreset = \"legendary\"\n");
    UserConfig cfg(fs, logger);
    cfg.load();

    CHECK(cfg.difficulty().preset == DifficultyPreset::Cadet);
    CHECK(logger.hasMessage(LogLevel::Warn, "legendary"));
}

TEST_CASE("Difficulty UserConfig unknown flight_assists falls back to AllOn with Warn", "[difficulty]") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("config/user.toml", "[difficulty]\nflight_assists = \"warp_drive\"\n");
    UserConfig cfg(fs, logger);
    cfg.load();

    CHECK(cfg.difficulty().toggles.flightAssists == FlightAssists::AllOn);
    CHECK(logger.hasMessage(LogLevel::Warn, "warp_drive"));
}

TEST_CASE("Difficulty UserConfig unknown countermeasure_use falls back to Never with Warn", "[difficulty]") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("config/user.toml", "[difficulty]\ncountermeasure_use = \"always_on\"\n");
    UserConfig cfg(fs, logger);
    cfg.load();

    CHECK(cfg.difficulty().ai.countermeasureUse == CountermeasureUse::Never);
    CHECK(logger.hasMessage(LogLevel::Warn, "always_on"));
}

TEST_CASE("Difficulty UserConfig unknown energy_management falls back to Passive with Warn", "[difficulty]") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("config/user.toml", "[difficulty]\nenergy_management = \"berserker\"\n");
    UserConfig cfg(fs, logger);
    cfg.load();

    CHECK(cfg.difficulty().ai.energyManagement == EnergyManagement::Passive);
    CHECK(logger.hasMessage(LogLevel::Warn, "berserker"));
}
