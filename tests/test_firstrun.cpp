// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "config/UserConfig.h"
#include "firstrun/FirstRun.h"
#include "mock_hal.h"

// ---------------------------------------------------------------------------
// Helper: extract written file content as string
// ---------------------------------------------------------------------------

static std::string written(const MockFilesystem& fs, const char* path) {
    auto it = fs.files.find(path);
    if (it == fs.files.end())
        return {};
    return std::string(it->second.begin(), it->second.end());
}

// ---------------------------------------------------------------------------
// UserConfig tests
// ---------------------------------------------------------------------------

TEST_CASE("UserConfig: missing config file returns false with no log", "[firstrun]") {
    MockFilesystem fs;
    MockLogger logger;
    UserConfig config(fs, logger);

    REQUIRE_FALSE(config.load());
    REQUIRE_FALSE(config.isFirstRunCompleted());
    REQUIRE(logger.entries.empty());
}

TEST_CASE("UserConfig: completed = true is read correctly", "[firstrun]") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("config/user.toml", "[first_run]\ncompleted = true\n");
    UserConfig config(fs, logger);

    REQUIRE(config.load());
    REQUIRE(config.isFirstRunCompleted());
}

TEST_CASE("UserConfig: completed = false is read correctly", "[firstrun]") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("config/user.toml", "[first_run]\ncompleted = false\n");
    UserConfig config(fs, logger);

    REQUIRE(config.load());
    REQUIRE_FALSE(config.isFirstRunCompleted());
}

TEST_CASE("UserConfig: save writes correct TOML content", "[firstrun]") {
    MockFilesystem fs;
    MockLogger logger;
    UserConfig config(fs, logger);

    config.setFirstRunCompleted(false);
    REQUIRE(config.save());

    std::string w = written(fs, "config/user.toml");
    REQUIRE(w.find("[first_run]") != std::string::npos);
    REQUIRE(w.find("completed = false") != std::string::npos);
}

TEST_CASE("UserConfig: save is atomic (write tmp then rename)", "[firstrun]") {
    MockFilesystem fs;
    MockLogger logger;
    UserConfig config(fs, logger);

    REQUIRE(config.save());
    REQUIRE(fs.renameCalls.size() == 1);
    REQUIRE(fs.renameCalls[0].from == "config/user.toml.tmp");
    REQUIRE(fs.renameCalls[0].to == "config/user.toml");
}

TEST_CASE("UserConfig: malformed TOML logs Warn and returns false", "[firstrun]") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("config/user.toml", "this is }{not valid toml");
    UserConfig config(fs, logger);

    REQUIRE_FALSE(config.load());
    REQUIRE(logger.hasMessage(LogLevel::Warn, "failed to parse"));
}

TEST_CASE("UserConfig: save returns false and logs Warn when createDirectory fails", "[firstrun]") {
    MockFilesystem fs;
    MockLogger logger;
    UserConfig config(fs, logger);

    fs.createDirectoryResult = false;
    REQUIRE_FALSE(config.save());
    REQUIRE(logger.hasMessage(LogLevel::Warn, "config directory"));
}

TEST_CASE("UserConfig: save returns false and logs Warn when openFile fails", "[firstrun]") {
    MockFilesystem fs;
    MockLogger logger;
    UserConfig config(fs, logger);

    fs.failWriteOpen = true;
    REQUIRE_FALSE(config.save());
    REQUIRE(logger.hasMessage(LogLevel::Warn, "tmp file"));
}

// ---------------------------------------------------------------------------
// FirstRun tests
// ---------------------------------------------------------------------------

TEST_CASE("FirstRun::check returns ShowWelcome when flag is false", "[firstrun]") {
    MockFilesystem fs;
    MockLogger logger;
    UserConfig config(fs, logger);
    FirstRun fr(config, logger);

    REQUIRE(fr.check() == FirstRunOutcome::ShowWelcome);
}

TEST_CASE("FirstRun::check returns Skip when flag is true", "[firstrun]") {
    MockFilesystem fs;
    MockLogger logger;
    fs.addFile("config/user.toml", "[first_run]\ncompleted = true\n");
    UserConfig config(fs, logger);
    config.load();
    FirstRun fr(config, logger);

    REQUIRE(fr.check() == FirstRunOutcome::Skip);
}

TEST_CASE("FirstRun::complete(GetStarted) sets flag, saves, and logs path", "[firstrun]") {
    MockFilesystem fs;
    MockLogger logger;
    UserConfig config(fs, logger);
    FirstRun fr(config, logger);

    fr.complete(WelcomePath::GetStarted);

    REQUIRE(config.isFirstRunCompleted());
    REQUIRE_FALSE(fs.renameCalls.empty());
    REQUIRE(logger.hasMessage(LogLevel::Info, "GetStarted"));
}

TEST_CASE("FirstRun::complete(ModDeveloper) sets flag, saves, and logs path", "[firstrun]") {
    MockFilesystem fs;
    MockLogger logger;
    UserConfig config(fs, logger);
    FirstRun fr(config, logger);

    fr.complete(WelcomePath::ModDeveloper);

    REQUIRE(config.isFirstRunCompleted());
    REQUIRE_FALSE(fs.renameCalls.empty());
    REQUIRE(logger.hasMessage(LogLevel::Info, "ModDeveloper"));
}

TEST_CASE("FirstRun::complete logs Warn when save fails, flag still set in memory", "[firstrun]") {
    MockFilesystem fs;
    MockLogger logger;
    UserConfig config(fs, logger);
    FirstRun fr(config, logger);

    fs.renameResult = false;
    fr.complete(WelcomePath::GetStarted);

    REQUIRE(config.isFirstRunCompleted());
    REQUIRE(logger.hasMessage(LogLevel::Warn, "persist"));
}

TEST_CASE("FirstRun round-trip: no file -> ShowWelcome -> complete -> saved as true", "[firstrun]") {
    MockFilesystem fs;
    MockLogger logger;
    UserConfig config(fs, logger);
    FirstRun fr(config, logger);

    REQUIRE_FALSE(config.load());
    REQUIRE(fr.check() == FirstRunOutcome::ShowWelcome);

    fr.complete(WelcomePath::GetStarted);

    REQUIRE(config.isFirstRunCompleted());
    std::string w = written(fs, "config/user.toml");
    REQUIRE(w.find("completed = true") != std::string::npos);
}
