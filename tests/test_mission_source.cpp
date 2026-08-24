// SPDX-License-Identifier: GPL-3.0-or-later
#include "MissionSource.h"

#include "ILogger.h"
#include "mock_log.h"
#include "temp_path.h"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace {

// A unique temp .yaml path per test run; removed on destruction. ctest runs each TEST_CASE as its
// own process, so the name must be unique across processes too -- a stem-derived path is not
// (docs/developer/development.md, #787).
struct TempYaml {
    std::filesystem::path path;
    explicit TempYaml(const std::string& stem, const std::string& contents) {
        path = fl::test::uniqueTempPath(stem, ".yaml");
        std::ofstream out(path, std::ios::binary);
        out << contents;
    }
    ~TempYaml() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};

} // namespace

TEST_CASE("loadMissionYaml resolves builtin mission ids first", "[mission-source]") {
    fl::NullLogger log;
    auto yaml = fl::loadMissionYaml("builtin:sandbox", nullptr, log);
    REQUIRE(yaml.has_value());
    CHECK(yaml->find("Sandbox Skirmish") != std::string::npos);

    auto gallery = fl::loadMissionYaml("builtin:shape-gallery", nullptr, log);
    REQUIRE(gallery.has_value());
    CHECK(gallery->find("Shape Gallery") != std::string::npos);
}

TEST_CASE("loadMissionYaml reads a .yaml file path directly", "[mission-source]") {
    fl::NullLogger log;
    const std::string contents = "name: \"From Disk\"\n";
    // Random stem: ctest runs in parallel, so never a fixed temp path.
    std::random_device rd;
    TempYaml f(std::string("fl_mission_source_") + std::to_string(rd()), contents);

    auto yaml = fl::loadMissionYaml(f.path.string(), nullptr, log);
    REQUIRE(yaml.has_value());
    CHECK(*yaml == contents);
}

TEST_CASE("loadMissionYaml returns nullopt for an unknown name and an unreadable file path", "[mission-source]") {
    fl::NullLogger log;
    CHECK_FALSE(fl::loadMissionYaml("builtin:nope", nullptr, log).has_value());
    CHECK_FALSE(fl::loadMissionYaml("no_such_mission", nullptr, log).has_value());
    // Shaped like a file but missing: warns and falls through; with no assets it resolves nothing.
    CHECK_FALSE(fl::loadMissionYaml("/nonexistent/dir/mission.yaml", nullptr, log).has_value());
}
