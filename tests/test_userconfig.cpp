// SPDX-License-Identifier: GPL-3.0-or-later
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
