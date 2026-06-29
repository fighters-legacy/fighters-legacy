// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <ILogger.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fl {

struct ServerConfig {
    // [server]
    std::string name = "Unnamed Server";
    uint16_t port = 4778;
    std::string bindAddress = "0.0.0.0";
    int maxPeers = 32;
    std::vector<std::string> gameModes = {"campaign", "mission", "sandbox"};
    std::string motd;
    uint16_t motdDisplayS{0}; // seconds; 0 = use client's motd_display_s setting
    std::string password;

    // [rotation]  — Phase 2: parsed and stored; rotation logic pending
    std::string rotationOrder = "sequential";
    std::vector<std::string> rotationItems;
    int rotationTimeLimitMin = 0;

    // [lobby]  — Phase 2: parsed and stored; lobby registration pending (issue #36)
    bool lobbyRegister = false;
    std::string lobbyUrl = "https://lobby.fighters-legacy.org";
    std::string lobbyVisibility = "public";

    // [mods]  — Phase 2: parsed and logged; ModLoader integration pending
    std::vector<std::string> modStack;

    // [world]  — Phase 2: active only with --persistent flag
    bool persistent = false;
    std::string worldSavePath = "world.sav";
    int worldAutosaveIntervalS = 300;
    int entitySoftCap = 0;               // 0 = unlimited; server-enforced object count limit
    double timeScale = 10.0;             // game seconds per real second; 10 = full day/night ~2.4 real hrs
    double planetRadiusM = 6'371'000.0;  // sphere radius (m); Earth default
    double drawDistanceKm = 200.0;       // per-peer interest radius (km); [1, 100000]
    uint32_t snapshotBudgetBytes = 1200; // per-client snapshot byte budget; 0 = unlimited; [0, 65535] (#516)
    uint32_t jitterBufferDepth = 4;      // per-peer input queue depth (ticks); [1, 32]
    uint32_t jitterAdaptWindow = 60;     // EWMA smoothing window in ticks; [10, 3600]
    uint32_t jitterHysteresis = 2;       // resize dead-band in ticks; [0, 8]
    float jitterMultiplier = 2.0f;       // k factor: depth = ceil(ewma + k*jitter); [0.0, 8.0]
    // Sim-tick CPU parallelism: total worker threads for the per-entity AI + integrate passes,
    // including the sim thread. 0 = auto (hardware_concurrency), 1 = serial. CPU knob, NOT a
    // capacity guarantee. CLI --sim-worker-threads overrides this. [0, 256]
    uint32_t simWorkerThreads = 0;

    // [ai]  — Phase 2: parsed and stored; enforcement lands with AI runtime
    std::string aiDifficultyFloor = "recruit";

    // [discovery]
    bool discoveryEnabled = true;
    int discoveryIntervalMs = 2000;

    // [shutdown]
    int shutdownWarningIntervalS = 300; // seconds between countdown broadcast notices (default 5 min)
    int minShutdownDelayS = 0;          // minimum seconds of warning required; 0 = no minimum
    bool shutdownRequireConfirm = true; // require --force flag before scheduling shutdown

    // [security]
    int connectRateLimitCount = 5;     // max connections per IP within the window
    int connectRateLimitWindowS = 10;  // sliding window size, seconds
    int packetFloodMultiplier = 3;     // disconnect if peer sends > N * 60 MsgClientInput/s
    std::string banlistPath;           // one normalized IP per line; empty = no persistence
    std::string allowlistPath;         // allowlist file; empty = disabled (all IPs allowed)
    uint32_t incomingBandwidthBps = 0; // ENet host incoming cap, bytes/s; 0 = unlimited
    uint32_t outgoingBandwidthBps = 0; // ENet host outgoing cap, bytes/s; 0 = unlimited
    std::string operatorPassword; // empty = network admin commands disabled; overridden by --admin-token at runtime
    int preHandshakeRateLimitCount = 20; // max CONNECT attempts per IP per window; 0 = disabled
    int preHandshakeWindowMs = 1000;     // sliding window in milliseconds
    int maxConnectionsPerIp = 0;         // max simultaneous connections per IP; 0 = unlimited
    int adminAuthMaxFailures = 5;        // consecutive wrong-password attempts before per-IP lockout [1,100]
    int adminAuthLockoutSeconds = 300;   // per-IP lockout duration in seconds [1,86400]
    int idleTimeoutS = 0;                // disconnect peers with no activity for N seconds; 0 = disabled [0,86400]

    // [rcon]
    struct RconConfig {
        bool enabled = false;
        uint16_t port = 27015;
        std::string password;    // empty + enabled = warn at startup; unauthenticated connections accepted
        int maxAuthFailures = 5; // lock out IP after this many consecutive failures
        int lockoutSeconds = 60; // per-IP lockout duration in seconds
    };
    RconConfig rcon;

    // [metrics]
    struct MetricsConfig {
        std::string tickJsonPath;           // empty = disabled; atomic per-interval tick-budget JSON export
        uint32_t tickJsonIntervalMs = 1000; // write cadence in ms; [100, 60000]
    };
    MetricsConfig metrics;

    // [spawn]
    struct SpawnPointDef {
        double x = 0.0;
        double z = 0.0;
    };
    struct SpawnConfig {
        double aglOffset = 2.0;            // metres AGL above terrain for all spawn points
        std::vector<SpawnPointDef> points; // empty = use origin
    };
    SpawnConfig spawn;
};

// Returns the embedded default server.toml content written on first run.
std::string_view defaultServerConfigToml();

// Parse server configuration from a TOML string.
// On parse error, logs a Warn and returns a default-constructed ServerConfig.
ServerConfig parseServerConfig(std::string_view content, ILogger* log);

} // namespace fl
