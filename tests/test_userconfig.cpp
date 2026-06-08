// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "config/UserConfig.h"
#include "mock_hal.h"

// ---------------------------------------------------------------------------
// parseLogLevel tests
// ---------------------------------------------------------------------------

TEST_CASE("parseLogLevel: known strings map correctly", "[userconfig]") {
    CHECK(parseLogLevel("debug") == LogLevel::Debug);
    CHECK(parseLogLevel("info") == LogLevel::Info);
    CHECK(parseLogLevel("warn") == LogLevel::Warn);
    CHECK(parseLogLevel("error") == LogLevel::Error);
}

TEST_CASE("parseLogLevel: unknown string falls back to Info", "[userconfig]") {
    CHECK(parseLogLevel("verbose") == LogLevel::Info);
    CHECK(parseLogLevel("UNKNOWN") == LogLevel::Info);
    CHECK(parseLogLevel("") == LogLevel::Info);
    CHECK(parseLogLevel(nullptr) == LogLevel::Info);
}

// ---------------------------------------------------------------------------
// UserConfig log level round-trip tests
// ---------------------------------------------------------------------------

TEST_CASE("UserConfig: logLevel default is Info when [engine] section absent", "[userconfig]") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("config/user.toml", "[first_run]\ncompleted = false\n");
    UserConfig config(fs, logger);
    config.load();
    CHECK(config.logLevel() == LogLevel::Info);
}

TEST_CASE("UserConfig: setLogLevel + save + reload round-trip for Debug", "[userconfig]") {
    MockFilesystem fs;
    MockLogger logger;
    UserConfig config(fs, logger);
    config.setLogLevel(LogLevel::Debug);
    config.save();

    // Reload into a fresh config
    MockLogger logger2;
    UserConfig config2(fs, logger2);
    config2.load();
    CHECK(config2.logLevel() == LogLevel::Debug);
}

TEST_CASE("UserConfig: setLogLevel + save + reload round-trip for Warn", "[userconfig]") {
    MockFilesystem fs;
    MockLogger logger;
    UserConfig config(fs, logger);
    config.setLogLevel(LogLevel::Warn);
    config.save();

    MockLogger logger2;
    UserConfig config2(fs, logger2);
    config2.load();
    CHECK(config2.logLevel() == LogLevel::Warn);
}

TEST_CASE("UserConfig: setLogLevel + save + reload round-trip for Error", "[userconfig]") {
    MockFilesystem fs;
    MockLogger logger;
    UserConfig config(fs, logger);
    config.setLogLevel(LogLevel::Error);
    config.save();

    MockLogger logger2;
    UserConfig config2(fs, logger2);
    config2.load();
    CHECK(config2.logLevel() == LogLevel::Error);
}

TEST_CASE("UserConfig: setLogLevel + save + reload round-trip for Info", "[userconfig]") {
    MockFilesystem fs;
    MockLogger logger;
    UserConfig config(fs, logger);
    config.setLogLevel(LogLevel::Info);
    config.save();

    MockLogger logger2;
    UserConfig config2(fs, logger2);
    config2.load();
    CHECK(config2.logLevel() == LogLevel::Info);
}

TEST_CASE("UserConfig: unknown log_level string in TOML falls back to Info and emits Warn", "[userconfig]") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("config/user.toml", "[engine]\nlog_level = \"verbose\"\n");
    UserConfig config(fs, logger);
    config.load();
    CHECK(config.logLevel() == LogLevel::Info);
    CHECK(logger.hasMessage(LogLevel::Warn, "verbose"));
}

TEST_CASE("UserConfig: [debug] overlay_mode Full round-trip", "[userconfig]") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("config/user.toml", "[debug]\noverlay_mode = 2\n");
    UserConfig config(fs, logger);
    config.load();
    CHECK(config.debug().overlayMode == OverlayMode::Full);
}

TEST_CASE("UserConfig: [debug] default Off when section absent", "[userconfig]") {
    MockFilesystem fs;
    MockLogger logger;
    UserConfig config(fs, logger);
    config.load(); // no file — defaults
    CHECK(config.debug().overlayMode == OverlayMode::Off);
}

TEST_CASE("UserConfig: [debug] out-of-range overlay_mode falls back to Off", "[userconfig]") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("config/user.toml", "[debug]\noverlay_mode = 99\n");
    UserConfig config(fs, logger);
    config.load();
    CHECK(config.debug().overlayMode == OverlayMode::Off);
}

TEST_CASE("UserConfig: [debug] Compact round-trip via save+load", "[userconfig]") {
    MockFilesystem fs;
    MockLogger logger;
    UserConfig config(fs, logger);
    DebugSettings ds;
    ds.overlayMode = OverlayMode::Compact;
    config.setDebug(ds);
    config.save();

    MockLogger logger2;
    UserConfig config2(fs, logger2);
    config2.load();
    CHECK(config2.debug().overlayMode == OverlayMode::Compact);
}

// ---------------------------------------------------------------------------
// [controls] tests
// ---------------------------------------------------------------------------

TEST_CASE("UserConfig: [controls] defaults when section absent", "[userconfig]") {
    MockFilesystem fs;
    MockLogger logger;
    UserConfig config(fs, logger);
    config.load(); // no file — defaults
    CHECK(config.controls().gamepadDeadzone == Catch::Approx(0.05f));
    CHECK_FALSE(config.controls().invertPitch);
    CHECK_FALSE(config.controls().invertRoll);
    CHECK_FALSE(config.controls().invertRudder);
    CHECK_FALSE(config.controls().invertThrottle);
}

TEST_CASE("UserConfig: [controls] deadzone out-of-range clamped to 0.99", "[userconfig]") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("config/user.toml", "[controls]\ngamepad_deadzone = 2.0\n");
    UserConfig config(fs, logger);
    config.load();
    CHECK(config.controls().gamepadDeadzone <= 0.99f);
}

TEST_CASE("UserConfig: [controls] roundtrip save+load", "[userconfig]") {
    MockFilesystem fs;
    MockLogger logger;
    UserConfig config(fs, logger);
    ControlsSettings cs;
    cs.gamepadDeadzone = 0.1f;
    cs.invertPitch = true;
    cs.invertRudder = true;
    config.setControls(cs);
    config.save();

    MockLogger logger2;
    UserConfig config2(fs, logger2);
    config2.load();
    CHECK(config2.controls().gamepadDeadzone == Catch::Approx(0.1f));
    CHECK(config2.controls().invertPitch);
    CHECK_FALSE(config2.controls().invertRoll);
    CHECK(config2.controls().invertRudder);
    CHECK_FALSE(config2.controls().invertThrottle);
}

TEST_CASE("UserConfig: [controls] HOTAS defaults when section absent", "[userconfig]") {
    MockFilesystem fs;
    MockLogger logger;
    UserConfig config(fs, logger);
    config.load();
    CHECK(config.controls().hotasAileronAxis == 0);
    CHECK(config.controls().hotasElevatorAxis == 1);
    CHECK(config.controls().hotasThrottleAxis == 2);
    CHECK(config.controls().hotasRudderAxis == 3);
    CHECK(config.controls().hotasDeadzone == Catch::Approx(0.05f));
    CHECK_FALSE(config.controls().hotasInvertPitch);
    CHECK_FALSE(config.controls().hotasInvertRoll);
    CHECK_FALSE(config.controls().hotasInvertRudder);
    CHECK_FALSE(config.controls().hotasInvertThrottle);
}

TEST_CASE("UserConfig: [controls] HOTAS deadzone clamped to 0.99", "[userconfig]") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("config/user.toml", "[controls]\nhotas_deadzone = 5.0\n");
    UserConfig config(fs, logger);
    config.load();
    CHECK(config.controls().hotasDeadzone <= 0.99f);
}

TEST_CASE("UserConfig: [controls] HOTAS axis index out of range clamped", "[userconfig]") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("config/user.toml", "[controls]\nhotas_aileron_axis = 200\nhotas_elevator_axis = -99\n");
    UserConfig config(fs, logger);
    config.load();
    CHECK(config.controls().hotasAileronAxis == 127);
    CHECK(config.controls().hotasElevatorAxis == -1);
}

TEST_CASE("UserConfig: [controls] HOTAS axis index -1 preserved", "[userconfig]") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("config/user.toml", "[controls]\nhotas_aileron_axis = -1\nhotas_elevator_axis = -1\n"
                                   "hotas_throttle_axis = -1\nhotas_rudder_axis = -1\n");
    UserConfig config(fs, logger);
    config.load();
    CHECK(config.controls().hotasAileronAxis == -1);
    CHECK(config.controls().hotasElevatorAxis == -1);
    CHECK(config.controls().hotasThrottleAxis == -1);
    CHECK(config.controls().hotasRudderAxis == -1);
}

TEST_CASE("UserConfig: [controls] HOTAS roundtrip save+load", "[userconfig]") {
    MockFilesystem fs;
    MockLogger logger;
    UserConfig config(fs, logger);
    ControlsSettings cs;
    cs.hotasAileronAxis = 4;
    cs.hotasElevatorAxis = 5;
    cs.hotasThrottleAxis = 6;
    cs.hotasRudderAxis = 7;
    cs.hotasDeadzone = 0.12f;
    cs.hotasInvertPitch = true;
    cs.hotasInvertRoll = false;
    cs.hotasInvertRudder = true;
    cs.hotasInvertThrottle = true;
    config.setControls(cs);
    config.save();

    MockLogger logger2;
    UserConfig config2(fs, logger2);
    config2.load();
    CHECK(config2.controls().hotasAileronAxis == 4);
    CHECK(config2.controls().hotasElevatorAxis == 5);
    CHECK(config2.controls().hotasThrottleAxis == 6);
    CHECK(config2.controls().hotasRudderAxis == 7);
    CHECK(config2.controls().hotasDeadzone == Catch::Approx(0.12f));
    CHECK(config2.controls().hotasInvertPitch);
    CHECK_FALSE(config2.controls().hotasInvertRoll);
    CHECK(config2.controls().hotasInvertRudder);
    CHECK(config2.controls().hotasInvertThrottle);
}

// ---------------------------------------------------------------------------
// [pilot] tests
// ---------------------------------------------------------------------------

TEST_CASE("UserConfig: pilot defaults when section absent", "[userconfig][pilot]") {
    MockFilesystem fs;
    MockLogger logger;
    UserConfig config(fs, logger);
    config.load(); // no file
    CHECK(config.pilot().profile.callsign == "Pilot");
    CHECK(config.pilot().profile.guid.empty());
    CHECK(config.pilot().profile.kills == 0);
    CHECK(config.pilot().profile.losses == 0);
    CHECK(config.pilot().profile.flightTimeS == 0);
    CHECK(config.pilot().campaign.activeCampaign.empty());
    CHECK(config.pilot().campaign.currentMission == 0);
    CHECK(config.pilot().campaign.completed.empty());
    CHECK(config.pilot().campaign.factionStandings.empty());
}

TEST_CASE("UserConfig: pilot callsign round-trip", "[userconfig][pilot]") {
    MockFilesystem fs;
    MockLogger logger;
    UserConfig config(fs, logger);
    PilotSettings ps = config.pilot();
    ps.profile.callsign = "Maverick";
    config.setPilot(ps);
    config.save();

    MockLogger logger2;
    UserConfig config2(fs, logger2);
    config2.load();
    CHECK(config2.pilot().profile.callsign == "Maverick");
}

TEST_CASE("UserConfig: pilot GUID auto-generated on first save", "[userconfig][pilot]") {
    MockFilesystem fs;
    MockLogger logger;
    UserConfig config(fs, logger);
    config.load(); // no file
    CHECK(config.pilot().profile.guid.empty());
    config.save();
    CHECK(!config.pilot().profile.guid.empty());
}

TEST_CASE("UserConfig: pilot GUID has correct UUID-v4 format", "[userconfig][pilot]") {
    MockFilesystem fs;
    MockLogger logger;
    UserConfig config(fs, logger);
    config.save();
    const std::string& guid = config.pilot().profile.guid;
    REQUIRE(guid.size() == 36);
    CHECK(guid[8] == '-');
    CHECK(guid[13] == '-');
    CHECK(guid[18] == '-');
    CHECK(guid[23] == '-');
    CHECK(guid[14] == '4'); // version nibble
}

TEST_CASE("UserConfig: pilot GUID stable across save+load", "[userconfig][pilot]") {
    MockFilesystem fs;
    MockLogger logger;
    UserConfig config(fs, logger);
    config.save();
    std::string guid = config.pilot().profile.guid;
    REQUIRE(!guid.empty());

    MockLogger logger2;
    UserConfig config2(fs, logger2);
    config2.load();
    CHECK(config2.pilot().profile.guid == guid);
}

TEST_CASE("UserConfig: pilot GUID not regenerated when already set", "[userconfig][pilot]") {
    MockFilesystem fs;
    MockLogger logger;
    UserConfig config(fs, logger);
    config.save();
    std::string guid = config.pilot().profile.guid;

    MockLogger logger2;
    UserConfig config2(fs, logger2);
    config2.load();
    config2.save(); // second save should keep existing GUID
    CHECK(config2.pilot().profile.guid == guid);
}

TEST_CASE("UserConfig: pilot stats round-trip", "[userconfig][pilot]") {
    MockFilesystem fs;
    MockLogger logger;
    UserConfig config(fs, logger);
    PilotSettings ps = config.pilot();
    ps.profile.kills = 7;
    ps.profile.losses = 3;
    ps.profile.flightTimeS = 3600;
    config.setPilot(ps);
    config.save();

    MockLogger logger2;
    UserConfig config2(fs, logger2);
    config2.load();
    CHECK(config2.pilot().profile.kills == 7);
    CHECK(config2.pilot().profile.losses == 3);
    CHECK(config2.pilot().profile.flightTimeS == 3600);
}

TEST_CASE("UserConfig: pilot negative fields clamped to 0 on load", "[userconfig][pilot]") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("config/user.toml", "[pilot]\n"
                                   "kills = -1\n"
                                   "losses = -2\n"
                                   "flight_time_s = -100\n"
                                   "[pilot.campaign]\n"
                                   "current_mission = -1\n");
    UserConfig config(fs, logger);
    config.load();
    CHECK(config.pilot().profile.kills == 0);
    CHECK(config.pilot().profile.losses == 0);
    CHECK(config.pilot().profile.flightTimeS == 0);
    CHECK(config.pilot().campaign.currentMission == 0);
}

TEST_CASE("UserConfig: pilot campaign active_campaign and current_mission round-trip", "[userconfig][pilot]") {
    MockFilesystem fs;
    MockLogger logger;
    UserConfig config(fs, logger);
    PilotSettings ps = config.pilot();
    ps.campaign.activeCampaign = "fa:us_campaign";
    ps.campaign.currentMission = 5;
    config.setPilot(ps);
    config.save();

    MockLogger logger2;
    UserConfig config2(fs, logger2);
    config2.load();
    CHECK(config2.pilot().campaign.activeCampaign == "fa:us_campaign");
    CHECK(config2.pilot().campaign.currentMission == 5);
}

TEST_CASE("UserConfig: pilot completed missions array round-trip", "[userconfig][pilot]") {
    MockFilesystem fs;
    MockLogger logger;
    UserConfig config(fs, logger);
    PilotSettings ps = config.pilot();
    ps.campaign.completed = {"mission_001", "mission_002"};
    config.setPilot(ps);
    config.save();

    MockLogger logger2;
    UserConfig config2(fs, logger2);
    config2.load();
    REQUIRE(config2.pilot().campaign.completed.size() == 2);
    CHECK(config2.pilot().campaign.completed[0] == "mission_001");
    CHECK(config2.pilot().campaign.completed[1] == "mission_002");
}

TEST_CASE("UserConfig: pilot completed empty array round-trip", "[userconfig][pilot]") {
    MockFilesystem fs;
    MockLogger logger;
    UserConfig config(fs, logger);
    // completed is empty by default; save produces completed = []
    config.save();

    MockLogger logger2;
    UserConfig config2(fs, logger2);
    config2.load();
    CHECK(config2.pilot().campaign.completed.empty());
}

TEST_CASE("UserConfig: pilot faction standings map round-trip", "[userconfig][pilot]") {
    MockFilesystem fs;
    MockLogger logger;
    UserConfig config(fs, logger);
    PilotSettings ps = config.pilot();
    ps.campaign.factionStandings["usa"] = 10;
    ps.campaign.factionStandings["ussr"] = -5;
    config.setPilot(ps);
    config.save();

    MockLogger logger2;
    UserConfig config2(fs, logger2);
    config2.load();
    REQUIRE(config2.pilot().campaign.factionStandings.size() == 2);
    CHECK(config2.pilot().campaign.factionStandings.at("usa") == 10);
    CHECK(config2.pilot().campaign.factionStandings.at("ussr") == -5);
}

TEST_CASE("UserConfig: pilot faction standings empty map round-trip", "[userconfig][pilot]") {
    MockFilesystem fs;
    MockLogger logger;
    UserConfig config(fs, logger);
    // factionStandings is empty by default
    config.save();

    MockLogger logger2;
    UserConfig config2(fs, logger2);
    config2.load();
    CHECK(config2.pilot().campaign.factionStandings.empty());
}

TEST_CASE("UserConfig: pilot full section round-trip", "[userconfig][pilot]") {
    MockFilesystem fs;
    MockLogger logger;
    UserConfig config(fs, logger);
    PilotSettings ps;
    ps.profile.callsign = "Goose";
    ps.profile.kills = 12;
    ps.profile.losses = 1;
    ps.profile.flightTimeS = 7200;
    ps.campaign.activeCampaign = "fa:ussr_campaign";
    ps.campaign.currentMission = 3;
    ps.campaign.completed = {"m_001", "m_002", "m_003"};
    ps.campaign.factionStandings["nato"] = 50;
    ps.campaign.factionStandings["pact"] = -20;
    config.setPilot(ps);
    config.save();
    std::string guid = config.pilot().profile.guid;
    REQUIRE(!guid.empty());

    MockLogger logger2;
    UserConfig config2(fs, logger2);
    config2.load();
    CHECK(config2.pilot().profile.callsign == "Goose");
    CHECK(config2.pilot().profile.guid == guid);
    CHECK(config2.pilot().profile.kills == 12);
    CHECK(config2.pilot().profile.losses == 1);
    CHECK(config2.pilot().profile.flightTimeS == 7200);
    CHECK(config2.pilot().campaign.activeCampaign == "fa:ussr_campaign");
    CHECK(config2.pilot().campaign.currentMission == 3);
    REQUIRE(config2.pilot().campaign.completed.size() == 3);
    CHECK(config2.pilot().campaign.completed[0] == "m_001");
    CHECK(config2.pilot().campaign.factionStandings.at("nato") == 50);
    CHECK(config2.pilot().campaign.factionStandings.at("pact") == -20);
}
