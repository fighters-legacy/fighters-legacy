// SPDX-License-Identifier: GPL-3.0-or-later
#include "mock_hal.h"
#include "server_config.h"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

using namespace fl;

// ---------------------------------------------------------------------------
// Defaults
// ---------------------------------------------------------------------------

TEST_CASE("parseServerConfig: empty TOML returns all defaults", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("", &log);

    CHECK(cfg.name == "Unnamed Server");
    CHECK(cfg.port == 4778);
    CHECK(cfg.bindAddress == "0.0.0.0");
    CHECK(cfg.maxPeers == 32);
    CHECK(cfg.gameModes == (std::vector<std::string>{"campaign", "mission", "sandbox"}));
    CHECK(cfg.motd.empty());
    CHECK(cfg.motdDisplayS == 0u);
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
    CHECK(cfg.discoveryEnabled == true);
    CHECK(cfg.discoveryIntervalMs == 2000);
    CHECK(cfg.connectRateLimitCount == 5);
    CHECK(cfg.connectRateLimitWindowS == 10);
    CHECK(cfg.packetFloodMultiplier == 3);
    CHECK(cfg.banlistPath.empty());
    CHECK(cfg.allowlistPath.empty());
    CHECK(cfg.incomingBandwidthBps == 0u);
    CHECK(cfg.outgoingBandwidthBps == 0u);
    CHECK(cfg.operatorPassword.empty());
    CHECK(cfg.preHandshakeRateLimitCount == 20);
    CHECK(cfg.preHandshakeWindowMs == 1000);
    CHECK(cfg.maxConnectionsPerIp == 0);
    CHECK(cfg.idleTimeoutS == 0);
    CHECK(cfg.drawDistanceKm == 200.0);
    CHECK(cfg.snapshotBudgetBytes == 1200u);
    CHECK(cfg.jitterBufferDepth == 4u);
    CHECK(cfg.jitterAdaptWindow == 60u);
    CHECK(cfg.jitterHysteresis == 2u);
    CHECK(cfg.jitterMultiplier == Catch::Approx(2.0f));
    CHECK(cfg.simWorkerThreads == 0u);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: reads world.sim_worker_threads", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\nsim_worker_threads = 8\n", &log);
    CHECK(cfg.simWorkerThreads == 8u);
}

TEST_CASE("parseServerConfig: world.sim_worker_threads boundary values accepted", "[server_config]") {
    MockLogger log;
    CHECK(parseServerConfig("[world]\nsim_worker_threads = 1\n", &log).simWorkerThreads == 1u);
    CHECK(parseServerConfig("[world]\nsim_worker_threads = 256\n", &log).simWorkerThreads == 256u);
}

TEST_CASE("parseServerConfig: world.sim_worker_threads out of range warns and keeps default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\nsim_worker_threads = 257\n", &log);
    CHECK(cfg.simWorkerThreads == 0u);
    CHECK_FALSE(log.entries.empty());
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

TEST_CASE("parseServerConfig: reads motd_display_s from [server] section", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[server]\nmotd_display_s = 30\n", &log);
    CHECK(cfg.motdDisplayS == 30u);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: motd_display_s 0 is accepted", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[server]\nmotd_display_s = 0\n", &log);
    CHECK(cfg.motdDisplayS == 0u);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: motd_display_s boundary 65535 is accepted", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[server]\nmotd_display_s = 65535\n", &log);
    CHECK(cfg.motdDisplayS == 65535u);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: motd_display_s 65536 warns and clamps to 65535", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[server]\nmotd_display_s = 65536\n", &log);
    CHECK(cfg.motdDisplayS == 65535u);
    CHECK(log.hasMessage(LogLevel::Warn, "server.motd_display_s out of range"));
}

TEST_CASE("parseServerConfig: motd_display_s negative warns and clamps to 0", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[server]\nmotd_display_s = -1\n", &log);
    CHECK(cfg.motdDisplayS == 0u);
    CHECK(log.hasMessage(LogLevel::Warn, "server.motd_display_s out of range"));
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
    CHECK(cfg.maxPeers == 32);
    CHECK(log.hasMessage(LogLevel::Warn, "server.max_peers out of range"));
}

TEST_CASE("parseServerConfig: max_peers 1025 warns and keeps default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[server]\nmax_peers = 1025\n", &log);
    CHECK(cfg.maxPeers == 32);
    CHECK(log.hasMessage(LogLevel::Warn, "server.max_peers out of range"));
}

TEST_CASE("parseServerConfig: max_peers boundary 1 is accepted", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[server]\nmax_peers = 1\n", &log);
    CHECK(cfg.maxPeers == 1);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: max_peers above the old 128 cap is accepted (128+ re-target)", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[server]\nmax_peers = 256\n", &log);
    CHECK(cfg.maxPeers == 256);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: max_peers boundary 1024 is accepted", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[server]\nmax_peers = 1024\n", &log);
    CHECK(cfg.maxPeers == 1024);
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
save_path                     = "/data/world.sav"
autosave_interval_s           = 600
jitter_buffer_depth           = 8
jitter_buffer_adapt_window    = 90
jitter_buffer_hysteresis      = 3
jitter_buffer_jitter_multiplier = 1.5
)",
                                 &log);

    CHECK(cfg.worldSavePath == "/data/world.sav");
    CHECK(cfg.worldAutosaveIntervalS == 600);
    CHECK(cfg.jitterBufferDepth == 8u);
    CHECK(cfg.jitterAdaptWindow == 90u);
    CHECK(cfg.jitterHysteresis == 3u);
    CHECK(cfg.jitterMultiplier == Catch::Approx(1.5f));
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: entity_soft_cap is parsed from [world]", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\nentity_soft_cap = 800\n", &log);
    CHECK(cfg.entitySoftCap == 800);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: absent entity_soft_cap defaults to 0", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("", &log);
    CHECK(cfg.entitySoftCap == 0);
}

TEST_CASE("parseServerConfig: negative entity_soft_cap warns and uses 0", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\nentity_soft_cap = -1\n", &log);
    CHECK(cfg.entitySoftCap == 0);
    CHECK(log.hasMessage(LogLevel::Warn, "entity_soft_cap"));
}

TEST_CASE("parseServerConfig: planet_radius_m valid value accepted", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\nplanet_radius_m = 3000000.0\n", &log);
    CHECK(cfg.planetRadiusM == 3'000'000.0);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: planet_radius_m too small warns and uses default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\nplanet_radius_m = 500.0\n", &log);
    CHECK(cfg.planetRadiusM == 6'371'000.0);
    CHECK(log.hasMessage(LogLevel::Warn, "planet_radius_m"));
}

TEST_CASE("parseServerConfig: planet_radius_m too large warns and uses default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\nplanet_radius_m = 2000000000.0\n", &log);
    CHECK(cfg.planetRadiusM == 6'371'000.0);
    CHECK(log.hasMessage(LogLevel::Warn, "planet_radius_m"));
}

TEST_CASE("parseServerConfig: reads world.draw_distance_km", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\ndraw_distance_km = 150.5\n", &log);
    CHECK(cfg.drawDistanceKm == 150.5);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: draw_distance_km below minimum warns and uses default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\ndraw_distance_km = 0.5\n", &log);
    CHECK(cfg.drawDistanceKm == 200.0);
    CHECK(log.hasMessage(LogLevel::Warn, "draw_distance_km"));
}

TEST_CASE("parseServerConfig: draw_distance_km above maximum warns and uses default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\ndraw_distance_km = 100001.0\n", &log);
    CHECK(cfg.drawDistanceKm == 200.0);
    CHECK(log.hasMessage(LogLevel::Warn, "draw_distance_km"));
}

TEST_CASE("parseServerConfig: reads world.snapshot_budget_bytes", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\nsnapshot_budget_bytes = 800\n", &log);
    CHECK(cfg.snapshotBudgetBytes == 800u);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: snapshot_budget_bytes 0 is accepted (unlimited)", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\nsnapshot_budget_bytes = 0\n", &log);
    CHECK(cfg.snapshotBudgetBytes == 0u);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: snapshot_budget_bytes 70000 warns and uses default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\nsnapshot_budget_bytes = 70000\n", &log);
    CHECK(cfg.snapshotBudgetBytes == 1200u);
    CHECK(log.hasMessage(LogLevel::Warn, "snapshot_budget_bytes"));
}

TEST_CASE("parseServerConfig: reads world.jitter_buffer_depth", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\njitter_buffer_depth = 16\n", &log);
    CHECK(cfg.jitterBufferDepth == 16u);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: jitter_buffer_depth 0 warns and uses default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\njitter_buffer_depth = 0\n", &log);
    CHECK(cfg.jitterBufferDepth == 4u);
    CHECK(log.hasMessage(LogLevel::Warn, "jitter_buffer_depth"));
}

TEST_CASE("parseServerConfig: jitter_buffer_depth 33 warns and uses default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\njitter_buffer_depth = 33\n", &log);
    CHECK(cfg.jitterBufferDepth == 4u);
    CHECK(log.hasMessage(LogLevel::Warn, "jitter_buffer_depth"));
}

TEST_CASE("parseServerConfig: jitter_buffer_depth boundary 1 is accepted", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\njitter_buffer_depth = 1\n", &log);
    CHECK(cfg.jitterBufferDepth == 1u);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: jitter_buffer_depth boundary 32 is accepted", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\njitter_buffer_depth = 32\n", &log);
    CHECK(cfg.jitterBufferDepth == 32u);
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
    CHECK(cfg.maxPeers == 32);
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

// ---------------------------------------------------------------------------
// [discovery] section
// ---------------------------------------------------------------------------

TEST_CASE("parseServerConfig: reads [discovery] fields", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[discovery]\nenabled = false\ninterval_ms = 5000\n", &log);
    CHECK_FALSE(cfg.discoveryEnabled);
    CHECK(cfg.discoveryIntervalMs == 5000);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: discovery interval_ms below 100 warns and keeps default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[discovery]\ninterval_ms = 50\n", &log);
    CHECK(cfg.discoveryIntervalMs == 2000);
    CHECK(log.hasMessage(LogLevel::Warn, "out of range"));
}

TEST_CASE("parseServerConfig: discovery interval_ms above 60000 warns and keeps default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[discovery]\ninterval_ms = 70000\n", &log);
    CHECK(cfg.discoveryIntervalMs == 2000);
    CHECK(log.hasMessage(LogLevel::Warn, "out of range"));
}

TEST_CASE("parseServerConfig: discovery interval_ms boundaries are accepted", "[server_config]") {
    {
        MockLogger log;
        auto cfg = parseServerConfig("[discovery]\ninterval_ms = 100\n", &log);
        CHECK(cfg.discoveryIntervalMs == 100);
        CHECK(log.entries.empty());
    }
    {
        MockLogger log;
        auto cfg = parseServerConfig("[discovery]\ninterval_ms = 60000\n", &log);
        CHECK(cfg.discoveryIntervalMs == 60000);
        CHECK(log.entries.empty());
    }
}

// ---------------------------------------------------------------------------
// [security]
// ---------------------------------------------------------------------------

TEST_CASE("parseServerConfig: reads [security] fields", "[server_config][security]") {
    MockLogger log;
    auto cfg = parseServerConfig(R"(
[security]
connect_rate_limit_count    = 10
connect_rate_limit_window_s = 30
packet_flood_multiplier     = 5
banlist_path                = "/etc/fl/ban.txt"
allowlist_path              = "/etc/fl/allow.txt"
incoming_bandwidth_bps      = 1000000
outgoing_bandwidth_bps      = 2000000
)",
                                 &log);
    CHECK(cfg.connectRateLimitCount == 10);
    CHECK(cfg.connectRateLimitWindowS == 30);
    CHECK(cfg.packetFloodMultiplier == 5);
    CHECK(cfg.banlistPath == "/etc/fl/ban.txt");
    CHECK(cfg.allowlistPath == "/etc/fl/allow.txt");
    CHECK(cfg.incomingBandwidthBps == 1000000u);
    CHECK(cfg.outgoingBandwidthBps == 2000000u);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: security defaults when [security] absent", "[server_config][security]") {
    MockLogger log;
    auto cfg = parseServerConfig("", &log);
    CHECK(cfg.connectRateLimitCount == 5);
    CHECK(cfg.connectRateLimitWindowS == 10);
    CHECK(cfg.packetFloodMultiplier == 3);
    CHECK(cfg.banlistPath.empty());
    CHECK(cfg.allowlistPath.empty());
    CHECK(cfg.incomingBandwidthBps == 0u);
    CHECK(cfg.outgoingBandwidthBps == 0u);
    CHECK(cfg.preHandshakeRateLimitCount == 20);
    CHECK(cfg.preHandshakeWindowMs == 1000);
}

TEST_CASE("parseServerConfig: connect_rate_limit_count out of range warns and uses default",
          "[server_config][security]") {
    {
        MockLogger log;
        auto cfg = parseServerConfig("[security]\nconnect_rate_limit_count = 0\n", &log);
        CHECK(cfg.connectRateLimitCount == 5);
        CHECK(log.hasMessage(LogLevel::Warn, "out of range"));
    }
    {
        MockLogger log;
        auto cfg = parseServerConfig("[security]\nconnect_rate_limit_count = 100001\n", &log);
        CHECK(cfg.connectRateLimitCount == 5);
        CHECK(log.hasMessage(LogLevel::Warn, "out of range"));
    }
    {
        // 101 was rejected before the 128+ re-target raised the ceiling to 100000.
        MockLogger log;
        auto cfg = parseServerConfig("[security]\nconnect_rate_limit_count = 1000\n", &log);
        CHECK(cfg.connectRateLimitCount == 1000);
        CHECK(log.entries.empty());
    }
}

TEST_CASE("parseServerConfig: connect_rate_limit_window_s out of range warns and uses default",
          "[server_config][security]") {
    {
        MockLogger log;
        auto cfg = parseServerConfig("[security]\nconnect_rate_limit_window_s = 0\n", &log);
        CHECK(cfg.connectRateLimitWindowS == 10);
        CHECK(log.hasMessage(LogLevel::Warn, "out of range"));
    }
    {
        MockLogger log;
        auto cfg = parseServerConfig("[security]\nconnect_rate_limit_window_s = 3601\n", &log);
        CHECK(cfg.connectRateLimitWindowS == 10);
        CHECK(log.hasMessage(LogLevel::Warn, "out of range"));
    }
}

TEST_CASE("parseServerConfig: packet_flood_multiplier out of range warns and uses default",
          "[server_config][security]") {
    {
        MockLogger log;
        auto cfg = parseServerConfig("[security]\npacket_flood_multiplier = 0\n", &log);
        CHECK(cfg.packetFloodMultiplier == 3);
        CHECK(log.hasMessage(LogLevel::Warn, "out of range"));
    }
    {
        MockLogger log;
        auto cfg = parseServerConfig("[security]\npacket_flood_multiplier = 101\n", &log);
        CHECK(cfg.packetFloodMultiplier == 3);
        CHECK(log.hasMessage(LogLevel::Warn, "out of range"));
    }
}

TEST_CASE("parseServerConfig: negative bandwidth warns and uses 0", "[server_config][security]") {
    {
        MockLogger log;
        auto cfg = parseServerConfig("[security]\nincoming_bandwidth_bps = -1\n", &log);
        CHECK(cfg.incomingBandwidthBps == 0u);
        CHECK(log.hasMessage(LogLevel::Warn, "must be >= 0"));
    }
    {
        MockLogger log;
        auto cfg = parseServerConfig("[security]\noutgoing_bandwidth_bps = -1\n", &log);
        CHECK(cfg.outgoingBandwidthBps == 0u);
        CHECK(log.hasMessage(LogLevel::Warn, "must be >= 0"));
    }
}

TEST_CASE("parseServerConfig: reads security.operator_password", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[security]\noperator_password = \"hunter2\"\n", &log);
    CHECK(cfg.operatorPassword == "hunter2");
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: operator_password empty string is accepted", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[security]\noperator_password = \"\"\n", &log);
    CHECK(cfg.operatorPassword.empty());
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: reads pre_handshake_rate_limit fields", "[server_config][security]") {
    MockLogger log;
    auto cfg =
        parseServerConfig("[security]\npre_handshake_rate_limit_count = 50\npre_handshake_window_ms = 500\n", &log);
    CHECK(cfg.preHandshakeRateLimitCount == 50);
    CHECK(cfg.preHandshakeWindowMs == 500);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: pre_handshake_rate_limit_count 0 is accepted without warning",
          "[server_config][security]") {
    MockLogger log;
    auto cfg = parseServerConfig("[security]\npre_handshake_rate_limit_count = 0\n", &log);
    CHECK(cfg.preHandshakeRateLimitCount == 0);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: pre_handshake_rate_limit_count out of range warns and uses default",
          "[server_config][security]") {
    {
        MockLogger log;
        auto cfg = parseServerConfig("[security]\npre_handshake_rate_limit_count = -1\n", &log);
        CHECK(cfg.preHandshakeRateLimitCount == 20);
        CHECK(log.hasMessage(LogLevel::Warn, "out of range"));
    }
    {
        MockLogger log;
        auto cfg = parseServerConfig("[security]\npre_handshake_rate_limit_count = 10001\n", &log);
        CHECK(cfg.preHandshakeRateLimitCount == 20);
        CHECK(log.hasMessage(LogLevel::Warn, "out of range"));
    }
}

TEST_CASE("parseServerConfig: pre_handshake_window_ms out of range warns and uses default",
          "[server_config][security]") {
    {
        MockLogger log;
        auto cfg = parseServerConfig("[security]\npre_handshake_window_ms = 50\n", &log);
        CHECK(cfg.preHandshakeWindowMs == 1000);
        CHECK(log.hasMessage(LogLevel::Warn, "out of range"));
    }
    {
        MockLogger log;
        auto cfg = parseServerConfig("[security]\npre_handshake_window_ms = 90000\n", &log);
        CHECK(cfg.preHandshakeWindowMs == 1000);
        CHECK(log.hasMessage(LogLevel::Warn, "out of range"));
    }
}

TEST_CASE("parseServerConfig: reads security.max_connections_per_ip", "[server_config][security]") {
    MockLogger log;
    auto cfg = parseServerConfig("[security]\nmax_connections_per_ip = 4\n", &log);
    CHECK(cfg.maxConnectionsPerIp == 4);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: max_connections_per_ip of zero is accepted without warning",
          "[server_config][security]") {
    MockLogger log;
    auto cfg = parseServerConfig("[security]\nmax_connections_per_ip = 0\n", &log);
    CHECK(cfg.maxConnectionsPerIp == 0);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: max_connections_per_ip out of range warns and uses default",
          "[server_config][security]") {
    {
        MockLogger log;
        auto cfg = parseServerConfig("[security]\nmax_connections_per_ip = -1\n", &log);
        CHECK(cfg.maxConnectionsPerIp == 0);
        CHECK(log.hasMessage(LogLevel::Warn, "out of range"));
    }
    {
        MockLogger log;
        auto cfg = parseServerConfig("[security]\nmax_connections_per_ip = 1025\n", &log);
        CHECK(cfg.maxConnectionsPerIp == 0);
        CHECK(log.hasMessage(LogLevel::Warn, "out of range"));
    }
    {
        // 129 was rejected before the 128+ re-target raised the ceiling to 1024.
        MockLogger log;
        auto cfg = parseServerConfig("[security]\nmax_connections_per_ip = 256\n", &log);
        CHECK(cfg.maxConnectionsPerIp == 256);
        CHECK(log.entries.empty());
    }
}

// ---------------------------------------------------------------------------
// [spawn]
// ---------------------------------------------------------------------------

TEST_CASE("parseServerConfig: spawn defaults when section absent", "[server_config][spawn]") {
    MockLogger log;
    auto cfg = parseServerConfig("", &log);
    CHECK(cfg.spawn.aglOffset == 2.0);
    CHECK(cfg.spawn.points.empty());
}

TEST_CASE("parseServerConfig: reads spawn.agl_offset", "[server_config][spawn]") {
    MockLogger log;
    auto cfg = parseServerConfig("[spawn]\nagl_offset = 1000.0\n", &log);
    CHECK(cfg.spawn.aglOffset == 1000.0);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: spawn.agl_offset below 0 warns and uses default", "[server_config][spawn]") {
    MockLogger log;
    auto cfg = parseServerConfig("[spawn]\nagl_offset = -1.0\n", &log);
    CHECK(cfg.spawn.aglOffset == 2.0);
    CHECK(log.hasMessage(LogLevel::Warn, "out of range"));
}

TEST_CASE("parseServerConfig: spawn.agl_offset above 50000 warns and uses default", "[server_config][spawn]") {
    MockLogger log;
    auto cfg = parseServerConfig("[spawn]\nagl_offset = 50001.0\n", &log);
    CHECK(cfg.spawn.aglOffset == 2.0);
    CHECK(log.hasMessage(LogLevel::Warn, "out of range"));
}

TEST_CASE("parseServerConfig: reads [[spawn.points]] entries", "[server_config][spawn]") {
    MockLogger log;
    const char* toml = "[spawn]\nagl_offset = 250.0\n\n"
                       "[[spawn.points]]\nx = 0.0\nz = 0.0\n\n"
                       "[[spawn.points]]\nx = 1000.0\nz = -500.0\n";
    auto cfg = parseServerConfig(toml, &log);
    REQUIRE(cfg.spawn.points.size() == 2);
    CHECK(cfg.spawn.points[0].x == 0.0);
    CHECK(cfg.spawn.points[0].z == 0.0);
    CHECK(cfg.spawn.points[1].x == 1000.0);
    CHECK(cfg.spawn.points[1].z == -500.0);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: spawn.points entry missing z is skipped with warning", "[server_config][spawn]") {
    MockLogger log;
    const char* toml = "[[spawn.points]]\nx = 100.0\n\n"
                       "[[spawn.points]]\nx = 200.0\nz = 300.0\n";
    auto cfg = parseServerConfig(toml, &log);
    // Only the complete entry is kept.
    REQUIRE(cfg.spawn.points.size() == 1);
    CHECK(cfg.spawn.points[0].x == 200.0);
    CHECK(cfg.spawn.points[0].z == 300.0);
    CHECK(log.hasMessage(LogLevel::Warn, "missing x or z"));
}

TEST_CASE("parseServerConfig: reads security.idle_timeout_s", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[security]\nidle_timeout_s = 60\n", &log);
    CHECK(cfg.idleTimeoutS == 60);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: idle_timeout_s 0 is accepted (disabled)", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[security]\nidle_timeout_s = 0\n", &log);
    CHECK(cfg.idleTimeoutS == 0);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: idle_timeout_s negative warns and keeps 0", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[security]\nidle_timeout_s = -1\n", &log);
    CHECK(cfg.idleTimeoutS == 0);
    CHECK(log.hasMessage(LogLevel::Warn, "idle_timeout_s out of range"));
}

TEST_CASE("parseServerConfig: idle_timeout_s 86401 warns and keeps 0", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[security]\nidle_timeout_s = 86401\n", &log);
    CHECK(cfg.idleTimeoutS == 0);
    CHECK(log.hasMessage(LogLevel::Warn, "idle_timeout_s out of range"));
}

// ---------------------------------------------------------------------------
// Adaptive jitter buffer config tests (#424 + #429)
// ---------------------------------------------------------------------------

TEST_CASE("parseServerConfig: reads world.jitter_buffer_adapt_window", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\njitter_buffer_adapt_window = 90\n", &log);
    CHECK(cfg.jitterAdaptWindow == 90u);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: jitter_buffer_adapt_window below 10 warns and uses default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\njitter_buffer_adapt_window = 9\n", &log);
    CHECK(cfg.jitterAdaptWindow == 60u);
    CHECK(log.hasMessage(LogLevel::Warn, "jitter_buffer_adapt_window"));
}

TEST_CASE("parseServerConfig: jitter_buffer_adapt_window above 3600 warns and uses default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\njitter_buffer_adapt_window = 3601\n", &log);
    CHECK(cfg.jitterAdaptWindow == 60u);
    CHECK(log.hasMessage(LogLevel::Warn, "jitter_buffer_adapt_window"));
}

TEST_CASE("parseServerConfig: jitter_buffer_adapt_window boundary 10 accepted", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\njitter_buffer_adapt_window = 10\n", &log);
    CHECK(cfg.jitterAdaptWindow == 10u);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: jitter_buffer_adapt_window boundary 3600 accepted", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\njitter_buffer_adapt_window = 3600\n", &log);
    CHECK(cfg.jitterAdaptWindow == 3600u);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: reads world.jitter_buffer_hysteresis", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\njitter_buffer_hysteresis = 3\n", &log);
    CHECK(cfg.jitterHysteresis == 3u);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: jitter_buffer_hysteresis above 8 warns and uses default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\njitter_buffer_hysteresis = 9\n", &log);
    CHECK(cfg.jitterHysteresis == 2u);
    CHECK(log.hasMessage(LogLevel::Warn, "jitter_buffer_hysteresis"));
}

TEST_CASE("parseServerConfig: jitter_buffer_hysteresis boundary 0 accepted", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\njitter_buffer_hysteresis = 0\n", &log);
    CHECK(cfg.jitterHysteresis == 0u);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: jitter_buffer_hysteresis boundary 8 accepted", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\njitter_buffer_hysteresis = 8\n", &log);
    CHECK(cfg.jitterHysteresis == 8u);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: reads world.jitter_buffer_jitter_multiplier", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\njitter_buffer_jitter_multiplier = 1.5\n", &log);
    CHECK(cfg.jitterMultiplier == Catch::Approx(1.5f));
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: jitter_buffer_jitter_multiplier negative warns and uses default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\njitter_buffer_jitter_multiplier = -0.1\n", &log);
    CHECK(cfg.jitterMultiplier == Catch::Approx(2.0f));
    CHECK(log.hasMessage(LogLevel::Warn, "jitter_buffer_jitter_multiplier"));
}

TEST_CASE("parseServerConfig: jitter_buffer_jitter_multiplier above 8.0 warns and uses default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\njitter_buffer_jitter_multiplier = 8.1\n", &log);
    CHECK(cfg.jitterMultiplier == Catch::Approx(2.0f));
    CHECK(log.hasMessage(LogLevel::Warn, "jitter_buffer_jitter_multiplier"));
}

TEST_CASE("parseServerConfig: jitter_buffer_jitter_multiplier boundary 0.0 accepted", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\njitter_buffer_jitter_multiplier = 0.0\n", &log);
    CHECK(cfg.jitterMultiplier == Catch::Approx(0.0f));
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: jitter_buffer_jitter_multiplier boundary 8.0 accepted", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\njitter_buffer_jitter_multiplier = 8.0\n", &log);
    CHECK(cfg.jitterMultiplier == Catch::Approx(8.0f));
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: adaptive jitter defaults are correct", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("", &log);
    CHECK(cfg.jitterAdaptWindow == 60u);
    CHECK(cfg.jitterHysteresis == 2u);
    CHECK(cfg.jitterMultiplier == Catch::Approx(2.0f));
}

// ---------------------------------------------------------------------------
// [metrics]
// ---------------------------------------------------------------------------

TEST_CASE("parseServerConfig: metrics defaults are correct", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("", &log);
    CHECK(cfg.metrics.tickJsonPath.empty());
    CHECK(cfg.metrics.tickJsonIntervalMs == 1000u);
}

TEST_CASE("parseServerConfig: reads [metrics] tick_json_path and interval", "[server_config]") {
    MockLogger log;
    auto cfg =
        parseServerConfig("[metrics]\ntick_json_path = '/var/run/fl/tick.json'\ntick_json_interval_ms = 500\n", &log);
    CHECK(cfg.metrics.tickJsonPath == "/var/run/fl/tick.json");
    CHECK(cfg.metrics.tickJsonIntervalMs == 500u);
}

TEST_CASE("parseServerConfig: tick_json_interval_ms below range warns and keeps default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[metrics]\ntick_json_interval_ms = 99\n", &log);
    CHECK(cfg.metrics.tickJsonIntervalMs == 1000u);
}

TEST_CASE("parseServerConfig: tick_json_interval_ms above range warns and keeps default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[metrics]\ntick_json_interval_ms = 60001\n", &log);
    CHECK(cfg.metrics.tickJsonIntervalMs == 1000u);
}

TEST_CASE("parseServerConfig: tick_json_interval_ms boundaries are accepted", "[server_config]") {
    MockLogger log;
    CHECK(parseServerConfig("[metrics]\ntick_json_interval_ms = 100\n", &log).metrics.tickJsonIntervalMs == 100u);
    CHECK(parseServerConfig("[metrics]\ntick_json_interval_ms = 60000\n", &log).metrics.tickJsonIntervalMs == 60000u);
}

TEST_CASE("parseServerConfig: default template parses with the [metrics] section", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig(defaultServerConfigToml(), &log);
    // The default template ships [metrics] with an empty path (disabled) and 1000 ms interval.
    CHECK(cfg.metrics.tickJsonPath.empty());
    CHECK(cfg.metrics.tickJsonIntervalMs == 1000u);
}
