// SPDX-License-Identifier: GPL-3.0-or-later
#include "ILogger.h"
#include "config/ConfigFile.h"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

// Minimal no-op logger for tests that don't care about log output.
struct SilentLogger : ILogger {
    void log(LogLevel, const char*, int, const char*) override {}
    void setMinLevel(LogLevel) override {}
    void flush() override {}
};

// ---------------------------------------------------------------------------
// ensureAndReadConfig
// ---------------------------------------------------------------------------

TEST_CASE("ensureAndReadConfig creates file with default content when absent", "[config_file]") {
    SilentLogger log;
    fs::path tmp = fs::temp_directory_path() / "fl_test_ensure_absent.toml";
    fs::remove(tmp);

    std::string result = fl::ensureAndReadConfig(tmp, "default content", log);

    REQUIRE(result == "default content");
    REQUIRE(fs::exists(tmp));

    // Verify the file was actually written. Close before remove — on Windows open
    // file handles prevent deletion.
    {
        std::ifstream f(tmp);
        std::string onDisk((std::istreambuf_iterator<char>(f)), {});
        REQUIRE(onDisk == "default content");
    }
    fs::remove(tmp);
}

TEST_CASE("ensureAndReadConfig reads existing file without overwriting", "[config_file]") {
    SilentLogger log;
    fs::path tmp = fs::temp_directory_path() / "fl_test_ensure_existing.toml";
    {
        std::ofstream f(tmp);
        f << "existing content";
    }

    std::string result = fl::ensureAndReadConfig(tmp, "default content", log);

    REQUIRE(result == "existing content");

    fs::remove(tmp);
}

TEST_CASE("ensureAndReadConfig returns empty string when directory does not exist", "[config_file]") {
    SilentLogger log;
    fs::path nonExistentDir = fs::temp_directory_path() / "fl_no_such_dir_xyz";
    fs::path tmp = nonExistentDir / "file.toml";
    fs::remove_all(nonExistentDir);

    std::string result = fl::ensureAndReadConfig(tmp, "default", log);

    REQUIRE(result.empty());
}

// ---------------------------------------------------------------------------
// writeConfigFile
// ---------------------------------------------------------------------------

TEST_CASE("writeConfigFile writes content atomically and reads it back", "[config_file]") {
    SilentLogger log;
    fs::path tmp = fs::temp_directory_path() / "fl_test_write_config.toml";
    fs::remove(tmp);

    bool ok = fl::writeConfigFile(tmp, "written content", log);

    REQUIRE(ok);
    REQUIRE(fs::exists(tmp));

    // Close before remove — on Windows open file handles prevent deletion.
    {
        std::ifstream f(tmp);
        std::string onDisk((std::istreambuf_iterator<char>(f)), {});
        REQUIRE(onDisk == "written content");
    }

    // Tmp file should not remain after rename.
    fs::path tmpFile = tmp;
    tmpFile += ".tmp";
    REQUIRE_FALSE(fs::exists(tmpFile));

    fs::remove(tmp);
}

TEST_CASE("writeConfigFile returns false when directory does not exist", "[config_file]") {
    SilentLogger log;
    fs::path nonExistentDir = fs::temp_directory_path() / "fl_no_such_dir_write";
    fs::path tmp = nonExistentDir / "file.toml";
    fs::remove_all(nonExistentDir);

    bool ok = fl::writeConfigFile(tmp, "content", log);

    REQUIRE_FALSE(ok);
}
