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

    CHECK(cfg.server.name == "Unnamed Server");
    CHECK(cfg.server.port == 4778);
    CHECK(cfg.server.bindAddress == "0.0.0.0");
    CHECK(cfg.server.maxPeers == 32);
    CHECK(cfg.server.gameModes == (std::vector<std::string>{"campaign", "mission", "sandbox"}));
    CHECK(cfg.server.motd.empty());
    CHECK(cfg.server.motdDisplayS == 0u);
    CHECK(cfg.server.password.empty());
    CHECK(cfg.rotation.order == "sequential");
    CHECK(cfg.rotation.items.empty());
    CHECK(cfg.rotation.timeLimitMin == 0);
    CHECK_FALSE(cfg.lobby.registerServer);
    // Empty by default (#1072): a URL with no service behind it is a default that only generates
    // confusing outbound failures. A lobby is opted into by naming one.
    CHECK(cfg.lobby.url.empty());
    CHECK(cfg.lobby.visibility == "public");
    CHECK(cfg.mods.stack.empty());
    CHECK(cfg.world.playerEntityType == "builtin:debug-entity");
    CHECK(cfg.ai.difficultyFloor == "recruit");
    CHECK(cfg.discovery.enabled == true);
    CHECK(cfg.discovery.intervalMs == 2000);
    CHECK(cfg.security.connectRateLimitCount == 5);
    CHECK(cfg.security.connectRateLimitWindowS == 10);
    CHECK(cfg.security.packetFloodMultiplier == 3);
    CHECK(cfg.security.banlistPath.empty());
    CHECK(cfg.security.allowlistPath.empty());
    CHECK(cfg.security.incomingBandwidthBps == 0u);
    CHECK(cfg.security.outgoingBandwidthBps == 0u);
    CHECK(cfg.security.operatorPassword.empty());
    CHECK(cfg.security.preHandshakeRateLimitCount == 20);
    CHECK(cfg.security.preHandshakeWindowMs == 1000);
    CHECK(cfg.security.maxConnectionsPerIp == 0);
    CHECK(cfg.security.idleTimeoutS == 0);
    CHECK(cfg.world.drawDistanceKm == 100.0);  // #1093: presentation relevance, not sensor truth
    CHECK(cfg.world.spatialCellSizeKm == 0.0); // #1093: 0 = auto (clamp(draw/32, 500 m, 10 km))
    CHECK(cfg.world.testSpawnAiCount == 0u);
    CHECK(cfg.world.testSpawnSpreadKm == 50.0);
    CHECK(cfg.world.testSpawnAglM == 500.0);
    CHECK(cfg.world.snapshotBudgetBytes == 1200u);
    CHECK(cfg.world.jitterBufferDepth == 4u);
    CHECK(cfg.world.jitterAdaptWindow == 60u);
    CHECK(cfg.world.jitterHysteresis == 2u);
    CHECK(cfg.world.jitterMultiplier == Catch::Approx(2.0f));
    CHECK(cfg.world.simWorkerThreads == 0u);
    CHECK(cfg.world.overrunGovernorEnabled == true);
    CHECK(cfg.world.overrunHighWatermark == Catch::Approx(0.90f));
    CHECK(cfg.world.overrunLowWatermark == Catch::Approx(0.60f));
    CHECK(cfg.world.overrunMinSnapshotHz == Catch::Approx(15.0f));
    CHECK(cfg.world.overrunMaxAiStride == 4u);
    CHECK(cfg.world.overrunBudgetFloorBytes == 400u);
    CHECK(cfg.world.overrunMinInterestFraction == Catch::Approx(0.5f));
    CHECK(cfg.world.maxCatchupTicks == 8);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: reads world.player_entity_type (#834)", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\nplayer_entity_type = \"fl-base:f5e\"\n", &log);
    CHECK(cfg.world.playerEntityType == "fl-base:f5e");
}

TEST_CASE("parseServerConfig: reads world.allow_observers (#857)", "[server_config]") {
    MockLogger log;
    CHECK(parseServerConfig("", &log).world.allowObservers == true); // default
    CHECK(parseServerConfig("[world]\nallow_observers = false\n", &log).world.allowObservers == false);
}

TEST_CASE("parseServerConfig: reads world.earth_rotation (#482)", "[server_config]") {
    MockLogger log;
    CHECK(parseServerConfig("", &log).world.earthRotation == true); // default: Earth-fixed rotating frame
    CHECK(parseServerConfig("[world]\nearth_rotation = false\n", &log).world.earthRotation == false);
}

TEST_CASE("parseServerConfig: reads mods.required (#872)", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[mods]\nrequired = [\"fl-base\", \"theater\"]\n", &log);
    REQUIRE(cfg.mods.requiredPacks.size() == 2u);
    CHECK(cfg.mods.requiredPacks[0] == "fl-base");
    CHECK(cfg.mods.requiredPacks[1] == "theater");
    CHECK(cfg.mods.requiredPackPolicy == "warn"); // default
}

TEST_CASE("parseServerConfig: reads mods.required_policy (#872)", "[server_config]") {
    MockLogger log;
    CHECK(parseServerConfig("[mods]\nrequired_policy = \"refuse\"\n", &log).mods.requiredPackPolicy == "refuse");
    CHECK(parseServerConfig("[mods]\nrequired_policy = \"allow_placeholder\"\n", &log).mods.requiredPackPolicy ==
          "allow_placeholder");
    // Invalid value warns and keeps the default.
    CHECK(parseServerConfig("[mods]\nrequired_policy = \"bogus\"\n", &log).mods.requiredPackPolicy == "warn");
}

TEST_CASE("parseServerConfig: reads world.sim_worker_threads", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\nsim_worker_threads = 8\n", &log);
    CHECK(cfg.world.simWorkerThreads == 8u);
}

TEST_CASE("parseServerConfig: world.sim_worker_threads boundary values accepted", "[server_config]") {
    MockLogger log;
    CHECK(parseServerConfig("[world]\nsim_worker_threads = 1\n", &log).world.simWorkerThreads == 1u);
    CHECK(parseServerConfig("[world]\nsim_worker_threads = 256\n", &log).world.simWorkerThreads == 256u);
}

TEST_CASE("parseServerConfig: world.sim_worker_threads out of range warns and keeps default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\nsim_worker_threads = 257\n", &log);
    CHECK(cfg.world.simWorkerThreads == 0u);
    CHECK_FALSE(log.entries.empty());
}

// ---------------------------------------------------------------------------
// SpatialIndex cell size + load-test spawn affordance (#573)
// ---------------------------------------------------------------------------

TEST_CASE("parseServerConfig: reads world.spatial_cell_size_km", "[server_config]") {
    MockLogger log;
    CHECK(parseServerConfig("[world]\nspatial_cell_size_km = 2.5\n", &log).world.spatialCellSizeKm == 2.5);
    // 0 = auto is a valid, accepted value (no warning).
    MockLogger log2;
    CHECK(parseServerConfig("[world]\nspatial_cell_size_km = 0\n", &log2).world.spatialCellSizeKm == 0.0);
    CHECK(log2.entries.empty());
}

TEST_CASE("parseServerConfig: world.spatial_cell_size_km out of range warns and keeps default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\nspatial_cell_size_km = 2000\n", &log);
    CHECK(cfg.world.spatialCellSizeKm == 0.0); // the default is auto (#1093)
    CHECK_FALSE(log.entries.empty());
}

TEST_CASE("parseServerConfig: reads world.test_spawn_* keys", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig(R"(
[world]
test_spawn_ai_count = 5000
test_spawn_spread_km = 80.0
test_spawn_agl_m = 1200.0
)",
                                 &log);
    CHECK(cfg.world.testSpawnAiCount == 5000u);
    CHECK(cfg.world.testSpawnSpreadKm == 80.0);
    CHECK(cfg.world.testSpawnAglM == 1200.0);
    CHECK(cfg.world.testSpawnEntityType.empty()); // default = builtin:debug-entity
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: world.test_spawn_entity_type selects the load AI type (#980)", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig(R"(
[world]
test_spawn_ai_count = 128
test_spawn_entity_type = "builtin:bomber"
)",
                                 &log);
    CHECK(cfg.world.testSpawnAiCount == 128u);
    CHECK(cfg.world.testSpawnEntityType == "builtin:bomber"); // crewed-AI load
}

TEST_CASE("parseServerConfig: world.test_spawn_* out of range warns and keeps defaults", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig(R"(
[world]
test_spawn_ai_count = 2000000
test_spawn_spread_km = -1
test_spawn_agl_m = 999999
)",
                                 &log);
    CHECK(cfg.world.testSpawnAiCount == 0u);
    CHECK(cfg.world.testSpawnSpreadKm == 50.0);
    CHECK(cfg.world.testSpawnAglM == 500.0);
    CHECK_FALSE(log.entries.empty());
}

TEST_CASE("parseServerConfig: reads world AI-mix + projectile-churn keys (#580)", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig(R"(
[world]
test_spawn_ai_mix = "loiter:60,pursuit:25,patrol:15"
test_projectile_rate = 120.0
test_projectile_ttl_s = 2.5
)",
                                 &log);
    CHECK(cfg.world.testSpawnAiMix == "loiter:60,pursuit:25,patrol:15");
    CHECK(cfg.world.testProjectileRate == 120.0);
    CHECK(cfg.world.testProjectileTtlS == 2.5);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: AI-mix/churn defaults are off", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\n", &log);
    CHECK(cfg.world.testSpawnAiMix.empty()); // all loiter (the #573 baseline)
    CHECK(cfg.world.testProjectileRate == 0.0);
    CHECK(cfg.world.testProjectileTtlS == 3.0);
}

TEST_CASE("parseServerConfig: invalid AI-mix warns and falls back to all loiter (#580)", "[server_config]") {
    MockLogger log;
    // Unknown behavior 'evade' -> Warn + empty mix (all loiter); the valid churn keys still apply.
    auto cfg = parseServerConfig(R"(
[world]
test_spawn_ai_mix = "loiter:50,evade:50"
test_projectile_rate = 500000
test_projectile_ttl_s = 0.001
)",
                                 &log);
    CHECK(cfg.world.testSpawnAiMix.empty());
    CHECK(cfg.world.testProjectileRate == 0.0); // out of range [0, 100000] -> default
    CHECK(cfg.world.testProjectileTtlS == 3.0); // out of range [0.05, 600] -> default
    CHECK_FALSE(log.entries.empty());
}

// ---------------------------------------------------------------------------
// Graceful tick-overrun governor keys (#514)
// ---------------------------------------------------------------------------

TEST_CASE("parseServerConfig: reads overrun governor keys", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig(R"(
[world]
overrun_governor_enabled = false
overrun_high_watermark = 0.8
overrun_low_watermark = 0.5
overrun_min_snapshot_hz = 20.0
overrun_max_ai_stride = 8
overrun_budget_floor_bytes = 600
overrun_min_interest_fraction = 0.75
max_catchup_ticks = 16
)",
                                 &log);
    CHECK_FALSE(cfg.world.overrunGovernorEnabled);
    CHECK(cfg.world.overrunHighWatermark == Catch::Approx(0.8f));
    CHECK(cfg.world.overrunLowWatermark == Catch::Approx(0.5f));
    CHECK(cfg.world.overrunMinSnapshotHz == Catch::Approx(20.0f));
    CHECK(cfg.world.overrunMaxAiStride == 8u);
    CHECK(cfg.world.overrunBudgetFloorBytes == 600u);
    CHECK(cfg.world.overrunMinInterestFraction == Catch::Approx(0.75f));
    CHECK(cfg.world.maxCatchupTicks == 16);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: overrun_low_watermark must be below high warns and keeps default", "[server_config]") {
    MockLogger log;
    // low >= high (default high 0.90) is rejected.
    auto cfg = parseServerConfig("[world]\noverrun_low_watermark = 0.95\n", &log);
    CHECK(cfg.world.overrunLowWatermark == Catch::Approx(0.60f));
    CHECK(log.hasMessage(LogLevel::Warn, "world.overrun_low_watermark out of range"));
}

TEST_CASE("parseServerConfig: overrun_max_ai_stride out of range warns and keeps default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\noverrun_max_ai_stride = 99\n", &log);
    CHECK(cfg.world.overrunMaxAiStride == 4u);
    CHECK(log.hasMessage(LogLevel::Warn, "world.overrun_max_ai_stride out of range"));
}

TEST_CASE("parseServerConfig: overrun_min_interest_fraction boundary + out-of-range", "[server_config]") {
    MockLogger log;
    CHECK(parseServerConfig("[world]\noverrun_min_interest_fraction = 0.1\n", &log).world.overrunMinInterestFraction ==
          Catch::Approx(0.1f));
    CHECK(parseServerConfig("[world]\noverrun_min_interest_fraction = 1.0\n", &log).world.overrunMinInterestFraction ==
          Catch::Approx(1.0f)); // 1.0 = lever disabled
    CHECK(log.entries.empty());
    MockLogger log2;
    auto cfg = parseServerConfig("[world]\noverrun_min_interest_fraction = 0.05\n", &log2);
    CHECK(cfg.world.overrunMinInterestFraction == Catch::Approx(0.5f));
    CHECK(log2.hasMessage(LogLevel::Warn, "world.overrun_min_interest_fraction out of range"));
    MockLogger log3;
    auto cfg2 = parseServerConfig("[world]\noverrun_min_interest_fraction = 1.5\n", &log3);
    CHECK(cfg2.world.overrunMinInterestFraction == Catch::Approx(0.5f));
    CHECK(log3.hasMessage(LogLevel::Warn, "world.overrun_min_interest_fraction out of range"));
}

TEST_CASE("parseServerConfig: max_catchup_ticks boundary + out-of-range", "[server_config]") {
    MockLogger log;
    CHECK(parseServerConfig("[world]\nmax_catchup_ticks = 1\n", &log).world.maxCatchupTicks == 1);
    CHECK(parseServerConfig("[world]\nmax_catchup_ticks = 64\n", &log).world.maxCatchupTicks == 64);
    MockLogger log2;
    auto cfg = parseServerConfig("[world]\nmax_catchup_ticks = 65\n", &log2);
    CHECK(cfg.world.maxCatchupTicks == 8);
    CHECK(log2.hasMessage(LogLevel::Warn, "world.max_catchup_ticks out of range"));
}

// ---------------------------------------------------------------------------
// Parse errors
// ---------------------------------------------------------------------------

TEST_CASE("parseServerConfig: malformed TOML logs Warn and returns defaults", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("not valid toml [[[ ~~~", &log);

    CHECK(log.hasMessage(LogLevel::Warn, "failed to parse config"));
    CHECK(cfg.server.port == 4778);
    CHECK(cfg.server.name == "Unnamed Server");
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

    CHECK(cfg.server.name == "My Server");
    CHECK(cfg.server.port == 9000);
    CHECK(cfg.server.bindAddress == "127.0.0.1");
    CHECK(cfg.server.maxPeers == 32);
    CHECK(cfg.server.motd == "Welcome!");
    CHECK(cfg.server.password == "s3cr3t");
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: reads motd_display_s from [server] section", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[server]\nmotd_display_s = 30\n", &log);
    CHECK(cfg.server.motdDisplayS == 30u);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: motd_display_s 0 is accepted", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[server]\nmotd_display_s = 0\n", &log);
    CHECK(cfg.server.motdDisplayS == 0u);
    CHECK(log.entries.empty());
}

// ---------------------------------------------------------------------------
// [network] transport selection (#507)
// ---------------------------------------------------------------------------

TEST_CASE("parseServerConfig: network defaults are gns + allow_insecure", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("", &log);
    CHECK(cfg.network.transport == "gns");
    CHECK(cfg.network.allowInsecure == true);
}

TEST_CASE("parseServerConfig: reads network.transport enet", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[network]\ntransport = \"enet\"\n", &log);
    CHECK(cfg.network.transport == "enet");
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: reads network.transport gns", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[network]\ntransport = \"gns\"\n", &log);
    CHECK(cfg.network.transport == "gns");
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: invalid network.transport warns and keeps default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[network]\ntransport = \"quic\"\n", &log);
    CHECK(cfg.network.transport == "gns");
    CHECK_FALSE(log.entries.empty());
}

TEST_CASE("parseServerConfig: reads network.allow_insecure false", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[network]\nallow_insecure = false\n", &log);
    CHECK(cfg.network.allowInsecure == false);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: motd_display_s boundary 65535 is accepted", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[server]\nmotd_display_s = 65535\n", &log);
    CHECK(cfg.server.motdDisplayS == 65535u);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: motd_display_s 65536 warns and clamps to 65535", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[server]\nmotd_display_s = 65536\n", &log);
    CHECK(cfg.server.motdDisplayS == 65535u);
    CHECK(log.hasMessage(LogLevel::Warn, "server.motd_display_s out of range"));
}

TEST_CASE("parseServerConfig: motd_display_s negative warns and clamps to 0", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[server]\nmotd_display_s = -1\n", &log);
    CHECK(cfg.server.motdDisplayS == 0u);
    CHECK(log.hasMessage(LogLevel::Warn, "server.motd_display_s out of range"));
}

TEST_CASE("parseServerConfig: port 0 warns and keeps default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[server]\nport = 0\n", &log);
    CHECK(cfg.server.port == 4778);
    CHECK(log.hasMessage(LogLevel::Warn, "server.port out of range"));
}

TEST_CASE("parseServerConfig: port 65536 warns and keeps default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[server]\nport = 65536\n", &log);
    CHECK(cfg.server.port == 4778);
    CHECK(log.hasMessage(LogLevel::Warn, "server.port out of range"));
}

TEST_CASE("parseServerConfig: port boundary 1 is accepted", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[server]\nport = 1\n", &log);
    CHECK(cfg.server.port == 1);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: port boundary 65535 is accepted", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[server]\nport = 65535\n", &log);
    CHECK(cfg.server.port == 65535);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: max_peers 0 warns and keeps default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[server]\nmax_peers = 0\n", &log);
    CHECK(cfg.server.maxPeers == 32);
    CHECK(log.hasMessage(LogLevel::Warn, "server.max_peers out of range"));
}

TEST_CASE("parseServerConfig: max_peers 1025 warns and keeps default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[server]\nmax_peers = 1025\n", &log);
    CHECK(cfg.server.maxPeers == 32);
    CHECK(log.hasMessage(LogLevel::Warn, "server.max_peers out of range"));
}

TEST_CASE("parseServerConfig: max_peers boundary 1 is accepted", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[server]\nmax_peers = 1\n", &log);
    CHECK(cfg.server.maxPeers == 1);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: max_peers above the old 128 cap is accepted (128+ re-target)", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[server]\nmax_peers = 256\n", &log);
    CHECK(cfg.server.maxPeers == 256);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: max_peers boundary 1024 is accepted", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[server]\nmax_peers = 1024\n", &log);
    CHECK(cfg.server.maxPeers == 1024);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: valid game_modes subset replaces default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[server]\ngame_modes = [\"campaign\", \"sandbox\"]\n", &log);
    REQUIRE(cfg.server.gameModes.size() == 2);
    CHECK(cfg.server.gameModes[0] == "campaign");
    CHECK(cfg.server.gameModes[1] == "sandbox");
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: invalid game_mode entry warns and is skipped", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[server]\ngame_modes = [\"campaign\", \"bogus\", \"sandbox\"]\n", &log);
    REQUIRE(cfg.server.gameModes.size() == 2);
    CHECK(cfg.server.gameModes[0] == "campaign");
    CHECK(cfg.server.gameModes[1] == "sandbox");
    CHECK(log.hasMessage(LogLevel::Warn, "bogus"));
}

TEST_CASE("parseServerConfig: all-invalid game_modes keeps default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[server]\ngame_modes = [\"bogus1\", \"bogus2\"]\n", &log);
    CHECK(cfg.server.gameModes == (std::vector<std::string>{"campaign", "mission", "sandbox"}));
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

    CHECK(cfg.rotation.order == "random");
    REQUIRE(cfg.rotation.items.size() == 2);
    CHECK(cfg.rotation.items[0] == "mission-alpha");
    CHECK(cfg.rotation.items[1] == "mission-beta");
    CHECK(cfg.rotation.timeLimitMin == 30);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: invalid rotation.order warns and keeps default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[rotation]\norder = \"alphabetical\"\n", &log);
    CHECK(cfg.rotation.order == "sequential");
    CHECK(log.hasMessage(LogLevel::Warn, "rotation.order"));
}

TEST_CASE("parseServerConfig: empty rotation.items stays empty", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[rotation]\nitems = []\n", &log);
    CHECK(cfg.rotation.items.empty());
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

    CHECK(cfg.lobby.registerServer == true);
    CHECK(cfg.lobby.url == "https://my.lobby.example");
    CHECK(cfg.lobby.visibility == "private");
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: invalid lobby.visibility warns and keeps default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[lobby]\nvisibility = \"unlisted\"\n", &log);
    CHECK(cfg.lobby.visibility == "public");
    CHECK(log.hasMessage(LogLevel::Warn, "lobby.visibility"));
}

// ---------------------------------------------------------------------------
// [mods]
// ---------------------------------------------------------------------------

TEST_CASE("parseServerConfig: reads mods.stack", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[mods]\nstack = [\"fl-base-pack\", \"my-theater\"]\n", &log);
    REQUIRE(cfg.mods.stack.size() == 2);
    CHECK(cfg.mods.stack[0] == "fl-base-pack");
    CHECK(cfg.mods.stack[1] == "my-theater");
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: empty mods.stack stays empty", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[mods]\nstack = []\n", &log);
    CHECK(cfg.mods.stack.empty());
    CHECK(log.entries.empty());
}

// ---------------------------------------------------------------------------
// [world]
// ---------------------------------------------------------------------------

TEST_CASE("parseServerConfig: reads [world] fields", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig(R"(
[world]
jitter_buffer_depth           = 8
jitter_buffer_adapt_window    = 90
jitter_buffer_hysteresis      = 3
jitter_buffer_jitter_multiplier = 1.5
)",
                                 &log);

    CHECK(cfg.world.jitterBufferDepth == 8u);
    CHECK(cfg.world.jitterAdaptWindow == 90u);
    CHECK(cfg.world.jitterHysteresis == 3u);
    CHECK(cfg.world.jitterMultiplier == Catch::Approx(1.5f));
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: entity_soft_cap is parsed from [world]", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\nentity_soft_cap = 800\n", &log);
    CHECK(cfg.world.entitySoftCap == 800);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: absent entity_soft_cap defaults to 0", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("", &log);
    CHECK(cfg.world.entitySoftCap == 0);
}

TEST_CASE("parseServerConfig: negative entity_soft_cap warns and uses 0", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\nentity_soft_cap = -1\n", &log);
    CHECK(cfg.world.entitySoftCap == 0);
    CHECK(log.hasMessage(LogLevel::Warn, "entity_soft_cap"));
}

TEST_CASE("parseServerConfig: planet_radius_m valid value accepted", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\nplanet_radius_m = 3000000.0\n", &log);
    CHECK(cfg.world.planetRadiusM == 3'000'000.0);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: planet_radius_m too small warns and uses default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\nplanet_radius_m = 500.0\n", &log);
    CHECK(cfg.world.planetRadiusM == 6'371'000.0);
    CHECK(log.hasMessage(LogLevel::Warn, "planet_radius_m"));
}

TEST_CASE("parseServerConfig: planet_radius_m too large warns and uses default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\nplanet_radius_m = 2000000000.0\n", &log);
    CHECK(cfg.world.planetRadiusM == 6'371'000.0);
    CHECK(log.hasMessage(LogLevel::Warn, "planet_radius_m"));
}

TEST_CASE("parseServerConfig: reads world.draw_distance_km", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\ndraw_distance_km = 150.5\n", &log);
    CHECK(cfg.world.drawDistanceKm == 150.5);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: draw_distance_km below minimum warns and uses default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\ndraw_distance_km = 0.5\n", &log);
    CHECK(cfg.world.drawDistanceKm == 100.0);
    CHECK(log.hasMessage(LogLevel::Warn, "draw_distance_km"));
}

TEST_CASE("parseServerConfig: draw_distance_km above maximum warns and uses default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\ndraw_distance_km = 100001.0\n", &log);
    CHECK(cfg.world.drawDistanceKm == 100.0);
    CHECK(log.hasMessage(LogLevel::Warn, "draw_distance_km"));
}

TEST_CASE("parseServerConfig: reads world.snapshot_budget_bytes", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\nsnapshot_budget_bytes = 800\n", &log);
    CHECK(cfg.world.snapshotBudgetBytes == 800u);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: snapshot_budget_bytes 0 is accepted (unlimited)", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\nsnapshot_budget_bytes = 0\n", &log);
    CHECK(cfg.world.snapshotBudgetBytes == 0u);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: snapshot_budget_bytes 70000 warns and uses default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\nsnapshot_budget_bytes = 70000\n", &log);
    CHECK(cfg.world.snapshotBudgetBytes == 1200u);
    CHECK(log.hasMessage(LogLevel::Warn, "snapshot_budget_bytes"));
}

TEST_CASE("parseServerConfig: reads world.congestion_* fields", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\ncongestion_enabled = false\ncongestion_min_send_hz = 20.0\n"
                                 "congestion_loss_threshold = 0.05\ncongestion_budget_floor_bytes = 600\n",
                                 &log);
    CHECK(cfg.world.congestionEnabled == false);
    CHECK(cfg.world.congestionMinSendHz == 20.0f);
    CHECK(cfg.world.congestionLossThreshold == 0.05f);
    CHECK(cfg.world.congestionBudgetFloorBytes == 600u);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: congestion_min_send_hz out of range warns and keeps default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\ncongestion_min_send_hz = 120.0\n", &log);
    CHECK(cfg.world.congestionMinSendHz == 10.0f);
    CHECK(log.hasMessage(LogLevel::Warn, "congestion_min_send_hz"));
}

TEST_CASE("parseServerConfig: congestion_loss_threshold out of range warns and keeps default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\ncongestion_loss_threshold = 2.0\n", &log);
    CHECK(cfg.world.congestionLossThreshold == 0.02f);
    CHECK(log.hasMessage(LogLevel::Warn, "congestion_loss_threshold"));
}

TEST_CASE("parseServerConfig: congestion_budget_floor_bytes out of range warns and keeps default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\ncongestion_budget_floor_bytes = 70000\n", &log);
    CHECK(cfg.world.congestionBudgetFloorBytes == 400u);
    CHECK(log.hasMessage(LogLevel::Warn, "congestion_budget_floor_bytes"));
}

TEST_CASE("parseServerConfig: reads world.jitter_buffer_depth", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\njitter_buffer_depth = 16\n", &log);
    CHECK(cfg.world.jitterBufferDepth == 16u);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: jitter_buffer_depth 0 warns and uses default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\njitter_buffer_depth = 0\n", &log);
    CHECK(cfg.world.jitterBufferDepth == 4u);
    CHECK(log.hasMessage(LogLevel::Warn, "jitter_buffer_depth"));
}

TEST_CASE("parseServerConfig: jitter_buffer_depth 33 warns and uses default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\njitter_buffer_depth = 33\n", &log);
    CHECK(cfg.world.jitterBufferDepth == 4u);
    CHECK(log.hasMessage(LogLevel::Warn, "jitter_buffer_depth"));
}

TEST_CASE("parseServerConfig: jitter_buffer_depth boundary 1 is accepted", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\njitter_buffer_depth = 1\n", &log);
    CHECK(cfg.world.jitterBufferDepth == 1u);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: jitter_buffer_depth boundary 32 is accepted", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\njitter_buffer_depth = 32\n", &log);
    CHECK(cfg.world.jitterBufferDepth == 32u);
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
        CHECK(cfg.ai.difficultyFloor == val);
    }
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: invalid ai.difficulty_floor warns and keeps default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[ai]\ndifficulty_floor = \"expert\"\n", &log);
    CHECK(cfg.ai.difficultyFloor == "recruit");
    CHECK(log.hasMessage(LogLevel::Warn, "difficulty_floor"));
}

// ---------------------------------------------------------------------------
// Partial config / cross-section isolation
// ---------------------------------------------------------------------------

TEST_CASE("parseServerConfig: partial config keeps unspecified keys at defaults", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[server]\nport = 9000\n", &log);
    CHECK(cfg.server.port == 9000);
    CHECK(cfg.server.name == "Unnamed Server");
    CHECK(cfg.server.maxPeers == 32);
    CHECK(cfg.rotation.order == "sequential");
    CHECK(cfg.ai.difficultyFloor == "recruit");
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: unknown TOML keys are silently ignored", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[server]\nport = 5000\nunknown_key = \"whatever\"\n", &log);
    CHECK(cfg.server.port == 5000);
    CHECK(log.entries.empty());
}

// ---------------------------------------------------------------------------
// [discovery] section
// ---------------------------------------------------------------------------

TEST_CASE("parseServerConfig: reads [discovery] fields", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[discovery]\nenabled = false\ninterval_ms = 5000\n", &log);
    CHECK_FALSE(cfg.discovery.enabled);
    CHECK(cfg.discovery.intervalMs == 5000);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: discovery interval_ms below 100 warns and keeps default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[discovery]\ninterval_ms = 50\n", &log);
    CHECK(cfg.discovery.intervalMs == 2000);
    CHECK(log.hasMessage(LogLevel::Warn, "out of range"));
}

TEST_CASE("parseServerConfig: discovery interval_ms above 60000 warns and keeps default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[discovery]\ninterval_ms = 70000\n", &log);
    CHECK(cfg.discovery.intervalMs == 2000);
    CHECK(log.hasMessage(LogLevel::Warn, "out of range"));
}

TEST_CASE("parseServerConfig: discovery interval_ms boundaries are accepted", "[server_config]") {
    {
        MockLogger log;
        auto cfg = parseServerConfig("[discovery]\ninterval_ms = 100\n", &log);
        CHECK(cfg.discovery.intervalMs == 100);
        CHECK(log.entries.empty());
    }
    {
        MockLogger log;
        auto cfg = parseServerConfig("[discovery]\ninterval_ms = 60000\n", &log);
        CHECK(cfg.discovery.intervalMs == 60000);
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
    CHECK(cfg.security.connectRateLimitCount == 10);
    CHECK(cfg.security.connectRateLimitWindowS == 30);
    CHECK(cfg.security.packetFloodMultiplier == 5);
    CHECK(cfg.security.banlistPath == "/etc/fl/ban.txt");
    CHECK(cfg.security.allowlistPath == "/etc/fl/allow.txt");
    CHECK(cfg.security.incomingBandwidthBps == 1000000u);
    CHECK(cfg.security.outgoingBandwidthBps == 2000000u);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: security defaults when [security] absent", "[server_config][security]") {
    MockLogger log;
    auto cfg = parseServerConfig("", &log);
    CHECK(cfg.security.connectRateLimitCount == 5);
    CHECK(cfg.security.connectRateLimitWindowS == 10);
    CHECK(cfg.security.packetFloodMultiplier == 3);
    CHECK(cfg.security.banlistPath.empty());
    CHECK(cfg.security.allowlistPath.empty());
    CHECK(cfg.security.incomingBandwidthBps == 0u);
    CHECK(cfg.security.outgoingBandwidthBps == 0u);
    CHECK(cfg.security.preHandshakeRateLimitCount == 20);
    CHECK(cfg.security.preHandshakeWindowMs == 1000);
}

TEST_CASE("parseServerConfig: connect_rate_limit_count out of range warns and uses default",
          "[server_config][security]") {
    {
        MockLogger log;
        auto cfg = parseServerConfig("[security]\nconnect_rate_limit_count = 0\n", &log);
        CHECK(cfg.security.connectRateLimitCount == 5);
        CHECK(log.hasMessage(LogLevel::Warn, "out of range"));
    }
    {
        MockLogger log;
        auto cfg = parseServerConfig("[security]\nconnect_rate_limit_count = 100001\n", &log);
        CHECK(cfg.security.connectRateLimitCount == 5);
        CHECK(log.hasMessage(LogLevel::Warn, "out of range"));
    }
    {
        // 101 was rejected before the 128+ re-target raised the ceiling to 100000.
        MockLogger log;
        auto cfg = parseServerConfig("[security]\nconnect_rate_limit_count = 1000\n", &log);
        CHECK(cfg.security.connectRateLimitCount == 1000);
        CHECK(log.entries.empty());
    }
}

TEST_CASE("parseServerConfig: connect_rate_limit_window_s out of range warns and uses default",
          "[server_config][security]") {
    {
        MockLogger log;
        auto cfg = parseServerConfig("[security]\nconnect_rate_limit_window_s = 0\n", &log);
        CHECK(cfg.security.connectRateLimitWindowS == 10);
        CHECK(log.hasMessage(LogLevel::Warn, "out of range"));
    }
    {
        MockLogger log;
        auto cfg = parseServerConfig("[security]\nconnect_rate_limit_window_s = 3601\n", &log);
        CHECK(cfg.security.connectRateLimitWindowS == 10);
        CHECK(log.hasMessage(LogLevel::Warn, "out of range"));
    }
}

TEST_CASE("parseServerConfig: packet_flood_multiplier out of range warns and uses default",
          "[server_config][security]") {
    {
        MockLogger log;
        auto cfg = parseServerConfig("[security]\npacket_flood_multiplier = 0\n", &log);
        CHECK(cfg.security.packetFloodMultiplier == 3);
        CHECK(log.hasMessage(LogLevel::Warn, "out of range"));
    }
    {
        MockLogger log;
        auto cfg = parseServerConfig("[security]\npacket_flood_multiplier = 101\n", &log);
        CHECK(cfg.security.packetFloodMultiplier == 3);
        CHECK(log.hasMessage(LogLevel::Warn, "out of range"));
    }
}

TEST_CASE("parseServerConfig: negative bandwidth warns and uses 0", "[server_config][security]") {
    {
        MockLogger log;
        auto cfg = parseServerConfig("[security]\nincoming_bandwidth_bps = -1\n", &log);
        CHECK(cfg.security.incomingBandwidthBps == 0u);
        CHECK(log.hasMessage(LogLevel::Warn, "must be >= 0"));
    }
    {
        MockLogger log;
        auto cfg = parseServerConfig("[security]\noutgoing_bandwidth_bps = -1\n", &log);
        CHECK(cfg.security.outgoingBandwidthBps == 0u);
        CHECK(log.hasMessage(LogLevel::Warn, "must be >= 0"));
    }
}

TEST_CASE("parseServerConfig: reads security.operator_password", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[security]\noperator_password = \"hunter2\"\n", &log);
    CHECK(cfg.security.operatorPassword == "hunter2");
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: operator_password empty string is accepted", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[security]\noperator_password = \"\"\n", &log);
    CHECK(cfg.security.operatorPassword.empty());
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: reads pre_handshake_rate_limit fields", "[server_config][security]") {
    MockLogger log;
    auto cfg =
        parseServerConfig("[security]\npre_handshake_rate_limit_count = 50\npre_handshake_window_ms = 500\n", &log);
    CHECK(cfg.security.preHandshakeRateLimitCount == 50);
    CHECK(cfg.security.preHandshakeWindowMs == 500);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: pre_handshake_rate_limit_count 0 is accepted without warning",
          "[server_config][security]") {
    MockLogger log;
    auto cfg = parseServerConfig("[security]\npre_handshake_rate_limit_count = 0\n", &log);
    CHECK(cfg.security.preHandshakeRateLimitCount == 0);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: pre_handshake_rate_limit_count out of range warns and uses default",
          "[server_config][security]") {
    {
        MockLogger log;
        auto cfg = parseServerConfig("[security]\npre_handshake_rate_limit_count = -1\n", &log);
        CHECK(cfg.security.preHandshakeRateLimitCount == 20);
        CHECK(log.hasMessage(LogLevel::Warn, "out of range"));
    }
    {
        MockLogger log;
        auto cfg = parseServerConfig("[security]\npre_handshake_rate_limit_count = 10001\n", &log);
        CHECK(cfg.security.preHandshakeRateLimitCount == 20);
        CHECK(log.hasMessage(LogLevel::Warn, "out of range"));
    }
}

TEST_CASE("parseServerConfig: pre_handshake_window_ms out of range warns and uses default",
          "[server_config][security]") {
    {
        MockLogger log;
        auto cfg = parseServerConfig("[security]\npre_handshake_window_ms = 50\n", &log);
        CHECK(cfg.security.preHandshakeWindowMs == 1000);
        CHECK(log.hasMessage(LogLevel::Warn, "out of range"));
    }
    {
        MockLogger log;
        auto cfg = parseServerConfig("[security]\npre_handshake_window_ms = 90000\n", &log);
        CHECK(cfg.security.preHandshakeWindowMs == 1000);
        CHECK(log.hasMessage(LogLevel::Warn, "out of range"));
    }
}

TEST_CASE("parseServerConfig: reads security.max_connections_per_ip", "[server_config][security]") {
    MockLogger log;
    auto cfg = parseServerConfig("[security]\nmax_connections_per_ip = 4\n", &log);
    CHECK(cfg.security.maxConnectionsPerIp == 4);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: max_connections_per_ip of zero is accepted without warning",
          "[server_config][security]") {
    MockLogger log;
    auto cfg = parseServerConfig("[security]\nmax_connections_per_ip = 0\n", &log);
    CHECK(cfg.security.maxConnectionsPerIp == 0);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: max_connections_per_ip out of range warns and uses default",
          "[server_config][security]") {
    {
        MockLogger log;
        auto cfg = parseServerConfig("[security]\nmax_connections_per_ip = -1\n", &log);
        CHECK(cfg.security.maxConnectionsPerIp == 0);
        CHECK(log.hasMessage(LogLevel::Warn, "out of range"));
    }
    {
        MockLogger log;
        auto cfg = parseServerConfig("[security]\nmax_connections_per_ip = 1025\n", &log);
        CHECK(cfg.security.maxConnectionsPerIp == 0);
        CHECK(log.hasMessage(LogLevel::Warn, "out of range"));
    }
    {
        // 129 was rejected before the 128+ re-target raised the ceiling to 1024.
        MockLogger log;
        auto cfg = parseServerConfig("[security]\nmax_connections_per_ip = 256\n", &log);
        CHECK(cfg.security.maxConnectionsPerIp == 256);
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
    CHECK(cfg.security.idleTimeoutS == 60);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: idle_timeout_s 0 is accepted (disabled)", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[security]\nidle_timeout_s = 0\n", &log);
    CHECK(cfg.security.idleTimeoutS == 0);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: idle_timeout_s negative warns and keeps 0", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[security]\nidle_timeout_s = -1\n", &log);
    CHECK(cfg.security.idleTimeoutS == 0);
    CHECK(log.hasMessage(LogLevel::Warn, "idle_timeout_s out of range"));
}

TEST_CASE("parseServerConfig: idle_timeout_s 86401 warns and keeps 0", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[security]\nidle_timeout_s = 86401\n", &log);
    CHECK(cfg.security.idleTimeoutS == 0);
    CHECK(log.hasMessage(LogLevel::Warn, "idle_timeout_s out of range"));
}

// ---------------------------------------------------------------------------
// Adaptive jitter buffer config tests (#424 + #429)
// ---------------------------------------------------------------------------

TEST_CASE("parseServerConfig: reads world.jitter_buffer_adapt_window", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\njitter_buffer_adapt_window = 90\n", &log);
    CHECK(cfg.world.jitterAdaptWindow == 90u);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: jitter_buffer_adapt_window below 10 warns and uses default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\njitter_buffer_adapt_window = 9\n", &log);
    CHECK(cfg.world.jitterAdaptWindow == 60u);
    CHECK(log.hasMessage(LogLevel::Warn, "jitter_buffer_adapt_window"));
}

TEST_CASE("parseServerConfig: jitter_buffer_adapt_window above 3600 warns and uses default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\njitter_buffer_adapt_window = 3601\n", &log);
    CHECK(cfg.world.jitterAdaptWindow == 60u);
    CHECK(log.hasMessage(LogLevel::Warn, "jitter_buffer_adapt_window"));
}

TEST_CASE("parseServerConfig: jitter_buffer_adapt_window boundary 10 accepted", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\njitter_buffer_adapt_window = 10\n", &log);
    CHECK(cfg.world.jitterAdaptWindow == 10u);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: jitter_buffer_adapt_window boundary 3600 accepted", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\njitter_buffer_adapt_window = 3600\n", &log);
    CHECK(cfg.world.jitterAdaptWindow == 3600u);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: reads world.jitter_buffer_hysteresis", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\njitter_buffer_hysteresis = 3\n", &log);
    CHECK(cfg.world.jitterHysteresis == 3u);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: jitter_buffer_hysteresis above 8 warns and uses default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\njitter_buffer_hysteresis = 9\n", &log);
    CHECK(cfg.world.jitterHysteresis == 2u);
    CHECK(log.hasMessage(LogLevel::Warn, "jitter_buffer_hysteresis"));
}

TEST_CASE("parseServerConfig: jitter_buffer_hysteresis boundary 0 accepted", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\njitter_buffer_hysteresis = 0\n", &log);
    CHECK(cfg.world.jitterHysteresis == 0u);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: jitter_buffer_hysteresis boundary 8 accepted", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\njitter_buffer_hysteresis = 8\n", &log);
    CHECK(cfg.world.jitterHysteresis == 8u);
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: reads world.jitter_buffer_jitter_multiplier", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\njitter_buffer_jitter_multiplier = 1.5\n", &log);
    CHECK(cfg.world.jitterMultiplier == Catch::Approx(1.5f));
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: jitter_buffer_jitter_multiplier negative warns and uses default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\njitter_buffer_jitter_multiplier = -0.1\n", &log);
    CHECK(cfg.world.jitterMultiplier == Catch::Approx(2.0f));
    CHECK(log.hasMessage(LogLevel::Warn, "jitter_buffer_jitter_multiplier"));
}

TEST_CASE("parseServerConfig: jitter_buffer_jitter_multiplier above 8.0 warns and uses default", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\njitter_buffer_jitter_multiplier = 8.1\n", &log);
    CHECK(cfg.world.jitterMultiplier == Catch::Approx(2.0f));
    CHECK(log.hasMessage(LogLevel::Warn, "jitter_buffer_jitter_multiplier"));
}

TEST_CASE("parseServerConfig: jitter_buffer_jitter_multiplier boundary 0.0 accepted", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\njitter_buffer_jitter_multiplier = 0.0\n", &log);
    CHECK(cfg.world.jitterMultiplier == Catch::Approx(0.0f));
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: jitter_buffer_jitter_multiplier boundary 8.0 accepted", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[world]\njitter_buffer_jitter_multiplier = 8.0\n", &log);
    CHECK(cfg.world.jitterMultiplier == Catch::Approx(8.0f));
    CHECK(log.entries.empty());
}

TEST_CASE("parseServerConfig: adaptive jitter defaults are correct", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("", &log);
    CHECK(cfg.world.jitterAdaptWindow == 60u);
    CHECK(cfg.world.jitterHysteresis == 2u);
    CHECK(cfg.world.jitterMultiplier == Catch::Approx(2.0f));
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

// Regression (#1072): the config an operator is handed on first run must not advertise a control that
// does nothing. `save_path` and `autosave_interval_s` described a persistent world that has no store
// behind it -- IWorldStore does not exist anywhere in the tree -- so the operator was told the world
// would be saved every five minutes and it never was. Same defect class as #1049's entity_soft_cap.
// Persistence returns as real work under Epic H (#500) in M5.0; it gets these keys back then.
//
// This asserts on the TEMPLATE, not the parse result: the failure being guarded is a key reappearing
// in the file the server writes, which is the surface an operator actually reads.
// The keys named here are the WORLD-STATE persistence that `--persistent` still only stubs -- not
// the [persistence] store, which does ship keys and does implement them (#533). Renamed when that
// landed: a case called "ships no persistence keys" sitting next to a shipped [persistence] section
// reads as a contradiction, and the next person to see it would have had to open it to find out
// which of the two was wrong.
TEST_CASE("defaultServerConfigToml: ships no WORLD-STATE persistence keys, which have no implementation",
          "[server_config]") {
    const std::string_view toml = defaultServerConfigToml();
    CHECK(toml.find("save_path") == std::string_view::npos);
    CHECK(toml.find("autosave_interval_s") == std::string_view::npos);
    CHECK(toml.find("persistent") == std::string_view::npos);
}

// Regression (#1072): a default URL pointing at a host with no service behind it is not a helpful
// default -- it turns "I never configured a lobby" into recurring outbound POST failures the operator
// has to chase. Registration itself ships (#143); the reference lobby service is #999. So the shipped
// default is EMPTY, and LobbyRegistration treats empty as disabled.
TEST_CASE("defaultServerConfigToml: the lobby url default is empty, not a host with no service", "[server_config]") {
    MockLogger log;
    CHECK(parseServerConfig(defaultServerConfigToml(), &log).lobby.url.empty());
    CHECK(std::string_view(defaultServerConfigToml()).find("lobby.fighters-legacy.org") == std::string_view::npos);
}

// Regression (#94 fuzzing): the logger parameter is documented as optional (nullptr = no logging).
// Malformed input that fires a parse-error or a validation-warning log path must tolerate a null log.
TEST_CASE("parseServerConfig: null logger is tolerated on malformed input", "[server_config]") {
    auto cfg = parseServerConfig("this = = not valid toml [[[", nullptr);
    CHECK(cfg.server.name == "Unnamed Server"); // parse error -> defaults

    auto cfg2 = parseServerConfig("[server]\nport = 999999\n", nullptr);
    CHECK(cfg2.server.port == 4778); // out-of-range -> warning logged (to the null sink) + default
}

// --- [ai] difficulty (#682) ---------------------------------------------------------------------

TEST_CASE("parseServerConfig: each valid ai.difficulty is accepted", "[server_config]") {
    for (const char* val : {"cadet", "pilot", "ace"}) {
        const std::string toml = std::string("[ai]\ndifficulty = \"") + val + "\"\n";
        MockLogger logger;
        const ServerConfig cfg = parseServerConfig(toml, &logger);
        CHECK(cfg.ai.difficulty == val);
    }
}

TEST_CASE("parseServerConfig: ai.difficulty defaults to pilot and rejects unknown values", "[server_config]") {
    MockLogger logger;
    CHECK(parseServerConfig("", &logger).ai.difficulty == "pilot");

    // An unknown preset keeps the default rather than inventing one — the established
    // range-validated-default idiom, and a warn so the operator sees the typo.
    const ServerConfig cfg = parseServerConfig("[ai]\ndifficulty = \"godlike\"\n", &logger);
    CHECK(cfg.ai.difficulty == "pilot");
}

TEST_CASE("parseServerConfig: ai.difficulty and ai.difficulty_floor are distinct keys", "[server_config]") {
    // `difficulty` is what the SERVER runs (it scales AI radar range + reaction time in the sim
    // tick). `difficulty_floor` is the future per-client clamp. Setting one must not move the other.
    MockLogger logger;
    const ServerConfig cfg = parseServerConfig("[ai]\ndifficulty = \"ace\"\ndifficulty_floor = \"veteran\"\n", &logger);
    CHECK(cfg.ai.difficulty == "ace");
    CHECK(cfg.ai.difficultyFloor == "veteran");
}

// ---------------------------------------------------------------------------
// Numeric hardening (#824) — found by the scheduled deep fuzz run.
//
// toml++'s float→int conversion performs static_cast<int64_t>(double) INSIDE the guard that is
// supposed to decide whether the cast is safe. A float literal outside int64's range is therefore
// undefined behaviour in the library, and TOML makes it trivially reachable: nothing stops anyone
// writing a floating-point literal where an integer field is expected. fl-server parses server.toml,
// and content packs parse entity/flight-model TOML from untrusted mods, so this is reachable input.
//
// fl::tomlInt (engine/config/TomlNumeric.h) is what stands between the parsers and that cast.
// ---------------------------------------------------------------------------

TEST_CASE("parseServerConfig: an out-of-range float port does not invoke UB", "[server_config][fuzz]") {
    // THE REPRODUCER, verbatim (fuzz/corpus/fuzz_server_config_toml/regress-824-float-port.bin).
    MockLogger log;
    auto cfg = parseServerConfig("[server]\n port = 10888888888888888888888888888888.0\n", &log);

    // It must not crash, and it must not silently adopt garbage: an unusable value is no value, so
    // the field keeps its default, exactly as it would for a missing key.
    CHECK(cfg.server.port == 4778);
}

TEST_CASE("parseServerConfig: assorted hostile numeric literals are all survivable", "[server_config][fuzz]") {
    MockLogger log;

    // Every one of these reaches the same float→int path.
    for (const char* src : {
             "[server]\nport = 1e308\n",
             "[server]\nport = -1e308\n",
             "[server]\nport = nan\n",
             "[server]\nport = inf\n",
             "[server]\nport = -inf\n",
             "[world]\nsim_worker_threads = 1e30\n",
             "[world]\nsnapshot_budget_bytes = -1e30\n",
             "[security]\nmax_connections_per_ip = 9.3e18\n", // just past INT64_MAX
         }) {
        INFO(src);
        CHECK_NOTHROW(parseServerConfig(src, &log));
    }
}

TEST_CASE("parseServerConfig: a whole-number float IS accepted for an integer field", "[server_config]") {
    // `port = 4778.0` is a legitimate spelling and toml++ means to allow it. The hardening rejects
    // values that cannot be represented, not values that merely look like floats.
    MockLogger log;
    auto cfg = parseServerConfig("[server]\nport = 4778.0\n", &log);
    CHECK(cfg.server.port == 4778);
}

TEST_CASE("parseServerConfig: a fractional value for an integer field is refused", "[server_config]") {
    // Silently truncating 4778.7 to 4778 would be a lie about what the operator asked for.
    MockLogger log;
    auto cfg = parseServerConfig("[server]\nport = 4778.7\n", &log);
    CHECK(cfg.server.port == 4778); // the default, not a truncation — the field was never applied
}

TEST_CASE("parseServerConfig: [wind] profile_path is parsed", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[wind]\nprofile_path = \"theaters/nevada.toml\"\n", &log);
    CHECK(cfg.wind.profilePath == "theaters/nevada.toml");
}

TEST_CASE("parseWindProfile: reads knots and skips out-of-range/malformed", "[server_config][wind]") {
    MockLogger log;
    const char* toml = "[[wind.profile]]\naltitude_m = 0\nspeed_ms = 10\nheading_deg = 270\n"
                       "[[wind.profile]]\naltitude_m = 5000\nspeed_ms = 40\nheading_deg = 260\n"
                       "[[wind.profile]]\naltitude_m = 99999\nspeed_ms = 40\nheading_deg = 260\n" // alt out of range
                       "[[wind.profile]]\naltitude_m = 3000\nspeed_ms = 20\n";                    // missing heading
    auto knots = fl::parseWindProfile(toml, &log);
    REQUIRE(knots.size() == 2);
    CHECK(knots[0].altM == 0.0f);
    CHECK(knots[0].speedMs == 10.0f);
    CHECK(knots[0].headingDeg == 270.0f);
    CHECK(knots[1].altM == 5000.0f);
}

TEST_CASE("parseWindProfile: empty/no-profile input yields no knots", "[server_config][wind]") {
    MockLogger log;
    CHECK(fl::parseWindProfile("[server]\nport = 4778\n", &log).empty());
    CHECK(fl::parseWindProfile("", &log).empty());
}

// ---------------------------------------------------------------------------
// [persistence] (#533)
// ---------------------------------------------------------------------------

TEST_CASE("parseServerConfig: [persistence] defaults to an enabled sqlite store", "[server_config]") {
    // The default is the assertion. Persistence is the one optional surface in this file that is ON
    // out of the box (#533), because the acceptance clause is that bans and stats survive a restart
    // on a SERVER, not on a server whose operator read the release notes.
    MockLogger log;
    auto cfg = parseServerConfig("", &log);

    CHECK(cfg.persistence.enabled);
    CHECK(cfg.persistence.backend == "sqlite");
    CHECK(cfg.persistence.sqlitePath == "cache/fl-server.db");
    CHECK(cfg.persistence.postgresDsn.empty());
    CHECK(cfg.persistence.busyTimeoutMs == 5000);
    CHECK(cfg.persistence.writeQueueMax == 4096);
}

TEST_CASE("parseServerConfig: [persistence] keys are read", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[persistence]\n"
                                 "enabled = false\n"
                                 "backend = \"postgres\"\n"
                                 "sqlite_path = \"/var/lib/fl/server.db\"\n"
                                 "postgres_dsn = \"host=db user=fl password=secret\"\n"
                                 "busy_timeout_ms = 250\n"
                                 "write_queue_max = 128\n",
                                 &log);

    CHECK_FALSE(cfg.persistence.enabled);
    CHECK(cfg.persistence.backend == "postgres");
    CHECK(cfg.persistence.sqlitePath == "/var/lib/fl/server.db");
    CHECK(cfg.persistence.postgresDsn == "host=db user=fl password=secret");
    CHECK(cfg.persistence.busyTimeoutMs == 250);
    CHECK(cfg.persistence.writeQueueMax == 128);
}

TEST_CASE("parseServerConfig: an unknown persistence backend warns and keeps sqlite", "[server_config]") {
    // A typo is a CONFIG error and is named as one here. Passing it through to the store factory
    // would report it as a failure to open a database, which sends the operator to look at a
    // service instead of at this line of their config.
    MockLogger log;
    auto cfg = parseServerConfig("[persistence]\nbackend = \"mysql\"\n", &log);

    CHECK(cfg.persistence.backend == "sqlite");
    CHECK(log.count(LogLevel::Warn, "persistence.backend") == 1);
}

TEST_CASE("parseServerConfig: persistence numeric keys are clamped to their documented ranges", "[server_config]") {
    MockLogger log;
    auto cfg = parseServerConfig("[persistence]\nbusy_timeout_ms = 0\nwrite_queue_max = 0\n", &log);
    CHECK(cfg.persistence.busyTimeoutMs == 5000);
    CHECK(cfg.persistence.writeQueueMax == 4096);

    auto high = parseServerConfig("[persistence]\nbusy_timeout_ms = 999999\nwrite_queue_max = 99999999\n", &log);
    CHECK(high.persistence.busyTimeoutMs == 5000);
    CHECK(high.persistence.writeQueueMax == 4096);
}
