// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <ILogger.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct ServerConfig {
    // [server]
    std::string name = "Unnamed Server";
    uint16_t port = 4778;
    std::string bindAddress = "0.0.0.0";
    int maxPeers = 32;
    std::vector<std::string> gameModes = {"campaign", "mission", "sandbox"};
    std::string motd;
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
    int entitySoftCap = 0;   // 0 = unlimited; server-enforced object count limit
    double timeScale = 10.0; // game seconds per real second; 10 = full day/night ~2.4 real hrs

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
};

// Returns the embedded default server.toml content written on first run.
std::string_view defaultServerConfigToml();

// Parse server configuration from a TOML string.
// On parse error, logs a Warn and returns a default-constructed ServerConfig.
ServerConfig parseServerConfig(std::string_view content, ILogger* log);
