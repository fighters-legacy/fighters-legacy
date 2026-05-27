// SPDX-License-Identifier: GPL-3.0-or-later
#include "mock_hal.h"
#include "server_config.h"
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Defaults
// ---------------------------------------------------------------------------

TEST_CASE("parseServerConfig: empty TOML returns all defaults", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("", &log);

    CHECK(cfg.name == "Unnamed Server");
    CHECK(cfg.port == 4778);
    CHECK(cfg.bindAddress == "0.0.0.0");
    CHECK(cfg.maxPeers == 16);
    CHECK(cfg.gameModes == (std::vector<std::string>{"campaign", "mission", "sandbox"}));
    CHECK(cfg.motd.empty());
    CHECK(cfg.password.empty());
    CHECK(cfg.rotationOrder == "sequential");
    CHECK(cfg.rotationItems.empty());
    CHECK(cfg.rotationTimeLimitMin == 0);
    CHECK_FALSE(cfg.lobbyRegister);
    CHECK(cfg.lobbyUrl == "https://lobby.fighters-legacy.org");
    CHECK(cfg.lobbyVisibility == "public");
    CHECK(cfg.modStack.empty());
    CHECK_FALSE(cfg.persistent);
    CHECK(cfg.worldSavePath == "world.sav");
    CHECK(cfg.worldAutosaveIntervalS == 300);
    CHECK(cfg.aiDifficultyFloor == "recruit");
    CHECK(log.entries.empty());
}

// ---------------------------------------------------------------------------
// Parse errors
// ---------------------------------------------------------------------------

TEST_CASE("parseServerConfig: malformed TOML logs Warn and returns defaults", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("not valid toml [[[ ~~~", &log);

    CHECK(log.hasMessage(LogLevel::Warn, "failed to parse config"));
    CHECK(cfg.port == 4778);
    CHECK(cfg.name == "Unnamed Server");
}

// ---------------------------------------------------------------------------
// [server] fields
// ---------------------------------------------------------------------------

TEST_CASE("parseServerConfig: reads [server] identity fields", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig(R"(
[server]
name         = "My Server"
port         = 9000
bind_address = "127.0.0.1"
max_peers    = 32
motd         = "Welcome!"
password     = "s3cr3t"
)",
                                 &log);

    CHECK(cfg.name == "My Server");
    CHECK(cfg.port == 9000);
    CHECK(cfg.bindAddress == "127.0.0.1");
    CHECK(cfg.maxPeers == 32);
    CHECK(cfg.motd == "Welcome!");
    CHECK(cfg.password == "s3cr3t");
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: port 0 warns and keeps default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[server]\nport = 0\n", &log);
    CHECK(cfg.port == 4778);
    CHECK(log.hasMessage(LogLevel::Warn, "server.port out of range"));
}

TEST_CASE("parseServerConfig: port 65536 warns and keeps default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[server]\nport = 65536\n", &log);
    CHECK(cfg.port == 4778);
    CHECK(log.hasMessage(LogLevel::Warn, "server.port out of range"));
}

TEST_CASE("parseServerConfig: port boundary 1 is accepted", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[server]\nport = 1\n", &log);
    CHECK(cfg.port == 1);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: port boundary 65535 is accepted", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[server]\nport = 65535\n", &log);
    CHECK(cfg.port == 65535);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: max_peers 0 warns and keeps default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[server]\nmax_peers = 0\n", &log);
    CHECK(cfg.maxPeers == 16);
    CHECK(log.hasMessage(LogLevel::Warn, "server.max_peers out of range"));
}

TEST_CASE("parseServerConfig: max_peers 129 warns and keeps default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[server]\nmax_peers = 129\n", &log);
    CHECK(cfg.maxPeers == 16);
    CHECK(log.hasMessage(LogLevel::Warn, "server.max_peers out of range"));
}

TEST_CASE("parseServerConfig: max_peers boundary 1 is accepted", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[server]\nmax_peers = 1\n", &log);
    CHECK(cfg.maxPeers == 1);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: max_peers boundary 128 is accepted", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[server]\nmax_peers = 128\n", &log);
    CHECK(cfg.maxPeers == 128);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: valid game_modes subset replaces default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[server]\ngame_modes = [\"campaign\", \"sandbox\"]\n", &log);
    REQUIRE(cfg.gameModes.size() == 2);
    CHECK(cfg.gameModes[0] == "campaign");
    CHECK(cfg.gameModes[1] == "sandbox");
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: invalid game_mode entry warns and is skipped", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[server]\ngame_modes = [\"campaign\", \"bogus\", \"sandbox\"]\n", &log);
    REQUIRE(cfg.gameModes.size() == 2);
    CHECK(cfg.gameModes[0] == "campaign");
    CHECK(cfg.gameModes[1] == "sandbox");
    CHECK(log.hasMessage(LogLevel::Warn, "bogus"));
}

TEST_CASE("parseServerConfig: all-invalid game_modes keeps default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[server]\ngame_modes = [\"bogus1\", \"bogus2\"]\n", &log);
    CHECK(cfg.gameModes == (std::vector<std::string>{"campaign", "mission", "sandbox"}));
}

// ---------------------------------------------------------------------------
// [rotation]
// ---------------------------------------------------------------------------

TEST_CASE("parseServerConfig: reads [rotation] fields", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig(R"(
[rotation]
order          = "random"
items          = ["mission-alpha", "mission-beta"]
time_limit_min = 30
)",
                                 &log);

    CHECK(cfg.rotationOrder == "random");
    REQUIRE(cfg.rotationItems.size() == 2);
    CHECK(cfg.rotationItems[0] == "mission-alpha");
    CHECK(cfg.rotationItems[1] == "mission-beta");
    CHECK(cfg.rotationTimeLimitMin == 30);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: invalid rotation.order warns and keeps default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[rotation]\norder = \"alphabetical\"\n", &log);
    CHECK(cfg.rotationOrder == "sequential");
    CHECK(log.hasMessage(LogLevel::Warn, "rotation.order"));
}

TEST_CASE("parseServerConfig: empty rotation.items stays empty", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[rotation]\nitems = []\n", &log);
    CHECK(cfg.rotationItems.empty());
    CHECK(log.entries.empty());
}

// ---------------------------------------------------------------------------
// [lobby]
// ---------------------------------------------------------------------------

TEST_CASE("parseServerConfig: reads [lobby] fields", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig(R"(
[lobby]
register   = true
url        = "https://my.lobby.example"
visibility = "private"
)",
                                 &log);

    CHECK(cfg.lobbyRegister == true);
    CHECK(cfg.lobbyUrl == "https://my.lobby.example");
    CHECK(cfg.lobbyVisibility == "private");
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: invalid lobby.visibility warns and keeps default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[lobby]\nvisibility = \"unlisted\"\n", &log);
    CHECK(cfg.lobbyVisibility == "public");
    CHECK(log.hasMessage(LogLevel::Warn, "lobby.visibility"));
}

// ---------------------------------------------------------------------------
// [mods]
// ---------------------------------------------------------------------------

TEST_CASE("parseServerConfig: reads mods.stack", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[mods]\nstack = [\"fl-base-pack\", \"my-theater\"]\n", &log);
    REQUIRE(cfg.modStack.size() == 2);
    CHECK(cfg.modStack[0] == "fl-base-pack");
    CHECK(cfg.modStack[1] == "my-theater");
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: empty mods.stack stays empty", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[mods]\nstack = []\n", &log);
    CHECK(cfg.modStack.empty());
    CHECK(log.entries.empty());
}

// ---------------------------------------------------------------------------
// [world]
// ---------------------------------------------------------------------------

TEST_CASE("parseServerConfig: reads [world] fields", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig(R"(
[world]
save_path           = "/data/world.sav"
autosave_interval_s = 600
)",
                                 &log);

    CHECK(cfg.worldSavePath == "/data/world.sav");
    CHECK(cfg.worldAutosaveIntervalS == 600);
    CHECK(log.entries.empty());
}

// ---------------------------------------------------------------------------
// [ai]
// ---------------------------------------------------------------------------

TEST_CASE("parseServerConfig: each valid difficulty_floor is accepted", "[server_config]") {
    MockLogger log;
    for (const char* val : {"recruit", "cadet", "veteran", "ace"}) {
        std::string toml = std::string("[ai]\ndifficulty_floor = \"") + val + "\"\n";
        auto cfg = parseServerConfig(toml, &log);
        CHECK(cfg.aiDifficultyFloor == val);
    }
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: invalid ai.difficulty_floor warns and keeps default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[ai]\ndifficulty_floor = \"expert\"\n", &log);
    CHECK(cfg.aiDifficultyFloor == "recruit");
    CHECK(log.hasMessage(LogLevel::Warn, "difficulty_floor"));
}

// ---------------------------------------------------------------------------
// Partial config / cross-section isolation
// ---------------------------------------------------------------------------

TEST_CASE("parseServerConfig: partial config keeps unspecified keys at defaults", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[server]\nport = 9000\n", &log);
    CHECK(cfg.port == 9000);
    CHECK(cfg.name == "Unnamed Server");
    CHECK(cfg.maxPeers == 16);
    CHECK(cfg.rotationOrder == "sequential");
    CHECK(cfg.aiDifficultyFloor == "recruit");
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: unknown TOML keys are silently ignored", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[server]\nport = 5000\nunknown_key = \"whatever\"\n", &log);
    CHECK(cfg.port == 5000);
    CHECK(log.entries.empty());
}
