// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#endif
//
// fl-server — headless dedicated server for fighters-legacy
//
// Configuration (later tiers override earlier ones):
//   1. server.toml in CWD (or path in FL_CONFIG env var)
//   2. CLI positional args: fl-server [port] [maxPeers]
//   3. Environment variables: FL_PORT, FL_BIND_ADDRESS, FL_MAX_PEERS, FL_NAME,
//      FL_PERSISTENT, FL_LOBBY_REGISTER, FL_LOBBY_URL, FL_LOBBY_VISIBILITY,
//      FL_AI_DIFFICULTY_FLOOR  (highest precedence)
//
// See docs/fl-server-config.md for the full operator configuration reference.
// fl-lobby integration is tracked in issue #36.
#include "AdminConsole.h"
#include "ENetNetwork.h"
#include "ENetNetworkFactory.h"
#include "IpListFile.h"
#include "StdoutLogger.h"
#include "net/DiscoveryBeacon.h"
#include "server_config.h"

#include <ILogger.h>
#include <Platform.h>
#include <config/ConfigFile.h>
#include <debug/DebugCommandRegistry.h>
#include <entity/EntityDef.h>
#include <entity/EntityManager.h>
#include <entity/EntityTypeRegistry.h>
#include <loop/GameLoop.h>
#include <net/GameProtocol.h>
#include <net/WorldBroadcaster.h>
#include <weather/WeatherController.h>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

// ---------------------------------------------------------------------------
// Signal handling
// ---------------------------------------------------------------------------

static volatile sig_atomic_t g_quit = 0;

static void onSignal(int) {
    g_quit = 1;
}

// ---------------------------------------------------------------------------
// 3-tier config override: CLI positional args + environment variables
// ---------------------------------------------------------------------------

static void applyCliAndEnvOverrides(ServerConfig& cfg, int argc, char** argv, ILogger* log) {
    // Tier 2: CLI positional args — [port] [maxPeers]
    if (argc >= 2 && argv[1][0] != '-')
        cfg.port = static_cast<uint16_t>(std::atoi(argv[1]));
    if (argc >= 3 && argv[2][0] != '-')
        cfg.maxPeers = std::atoi(argv[2]);

    // Tier 3: environment variables (highest precedence)
    if (const char* e = std::getenv("FL_PORT"))
        cfg.port = static_cast<uint16_t>(std::atoi(e));
    if (const char* e = std::getenv("FL_BIND_ADDRESS"))
        cfg.bindAddress = e;
    if (const char* e = std::getenv("FL_MAX_PEERS"))
        cfg.maxPeers = std::atoi(e);
    if (const char* e = std::getenv("FL_NAME"))
        cfg.name = e;
    if (const char* e = std::getenv("FL_PERSISTENT"))
        cfg.persistent = (std::strcmp(e, "true") == 0 || std::strcmp(e, "1") == 0);
    if (const char* e = std::getenv("FL_LOBBY_REGISTER"))
        cfg.lobbyRegister = (std::strcmp(e, "true") == 0 || std::strcmp(e, "1") == 0);
    if (const char* e = std::getenv("FL_LOBBY_URL"))
        cfg.lobbyUrl = e;
    if (const char* e = std::getenv("FL_LOBBY_VISIBILITY")) {
        if (std::strcmp(e, "public") == 0 || std::strcmp(e, "private") == 0)
            cfg.lobbyVisibility = e;
        else
            log->log(LogLevel::Warn, __FILE__, __LINE__,
                     "FL_LOBBY_VISIBILITY must be \"public\" or \"private\"; ignoring");
    }
    if (const char* e = std::getenv("FL_AI_DIFFICULTY_FLOOR")) {
        if (std::strcmp(e, "recruit") == 0 || std::strcmp(e, "cadet") == 0 || std::strcmp(e, "veteran") == 0 ||
            std::strcmp(e, "ace") == 0)
            cfg.aiDifficultyFloor = e;
        else
            log->log(LogLevel::Warn, __FILE__, __LINE__,
                     "FL_AI_DIFFICULTY_FLOOR must be recruit/cadet/veteran/ace; ignoring");
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    // Pre-pass: --help / --version / --persistent / --bind
    bool flagPersistent = false;
    std::string flagBind; // non-empty if --bind addr was given

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::printf("Usage: fl-server [port] [maxPeers]\n"
                        "\n"
                        "Options:\n"
                        "  --help             Print this message and exit\n"
                        "  --version          Print version and exit\n"
                        "  --persistent       Enable persistent world mode (Phase 2 -- not yet active)\n"
                        "  --bind <addr>      Bind address (overrides server.toml and FL_BIND_ADDRESS)\n"
                        "\n"
                        "Admin console commands are available on stdin (type 'help' for a command list).\n"
                        "\n"
                        "Environment:\n"
                        "  FL_CONFIG              Path to server.toml (default: ./server.toml)\n"
                        "  FL_PORT                Bind port (default: 4778)\n"
                        "  FL_BIND_ADDRESS        Bind address (default: 0.0.0.0)\n"
                        "  FL_MAX_PEERS           Max simultaneous peers (default: 32)\n"
                        "  FL_NAME                Server name (default: \"Unnamed Server\")\n"
                        "  FL_PERSISTENT          \"true\" to enable persistent world, Phase 2\n"
                        "  FL_LOBBY_REGISTER      \"true\" to advertise to fl-lobby, Phase 2\n"
                        "  FL_LOBBY_URL           fl-lobby base URL, Phase 2\n"
                        "  FL_LOBBY_VISIBILITY    \"public\" or \"private\", Phase 2\n"
                        "  FL_AI_DIFFICULTY_FLOOR recruit/cadet/veteran/ace, Phase 2\n"
                        "\n"
                        "Config file is written with defaults on first run if absent.\n"
                        "See docs/fl-server-config.md for the full operator reference.\n");
            return 0;
        }
        if (std::strcmp(argv[i], "--version") == 0 || std::strcmp(argv[i], "-v") == 0) {
            std::printf("fl-server %s (%s)\n", "0.0.1", enetLibraryVersion());
            return 0;
        }
        if (std::strcmp(argv[i], "--persistent") == 0)
            flagPersistent = true;
        if (std::strcmp(argv[i], "--bind") == 0 && i + 1 < argc)
            flagBind = argv[++i];
    }

    // ---- Set up platform ----
    Platform p;
    p.logger = std::make_unique<StdoutLogger>();
    p.network = createENetNetwork();

    ILogger* log = p.logger.get();
    INetwork* net = p.network.get();

    {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "fl-server %s (%s) starting", "0.0.1", enetLibraryVersion());
        log->log(LogLevel::Info, __FILE__, __LINE__, buf);
    }

    // ---- Tier 1: server.toml ----
    const char* configEnv = std::getenv("FL_CONFIG");
    std::string configPath = configEnv ? configEnv : "server.toml";
    ServerConfig cfg = parseServerConfig(fl::ensureAndReadConfig(configPath, defaultServerConfigToml(), *log), log);

    // ---- Tier 2 + 3: CLI positional args and environment variables ----
    applyCliAndEnvOverrides(cfg, argc, argv, log);

    // --persistent / --bind flags from the pre-pass override any lower tier.
    if (flagPersistent)
        cfg.persistent = true;
    if (!flagBind.empty())
        cfg.bindAddress = flagBind;

    // ---- Phase 2 stub logs ----
    if (cfg.persistent)
        log->log(LogLevel::Info, __FILE__, __LINE__, "persistent world requested (Phase 2 -- not yet active)");
    if (!cfg.rotationItems.empty()) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "rotation: %zu item(s), order=%s (Phase 2 -- not yet active)",
                      cfg.rotationItems.size(), cfg.rotationOrder.c_str());
        log->log(LogLevel::Info, __FILE__, __LINE__, buf);
    }
    if (!cfg.modStack.empty()) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "mod stack: %zu mod(s) configured (Phase 2 -- not loaded yet)",
                      cfg.modStack.size());
        log->log(LogLevel::Info, __FILE__, __LINE__, buf);
    }
    if (cfg.lobbyRegister)
        log->log(LogLevel::Info, __FILE__, __LINE__, "lobby registration configured (Phase 2 -- not yet active)");

    // ---- Init network ----
    if (!net->init()) {
        log->log(LogLevel::Error, __FILE__, __LINE__, "network init failed");
        return 1;
    }

    if (!net->bind(cfg.bindAddress.c_str(), cfg.port, cfg.maxPeers)) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "bind failed: %s", net->getLastError() ? net->getLastError() : "unknown");
        log->log(LogLevel::Error, __FILE__, __LINE__, buf);
        net->shutdown();
        return 1;
    }

    {
        char buf[192];
        std::snprintf(buf, sizeof(buf), "listening on %s:%u (max %d peers) name=\"%s\"", cfg.bindAddress.c_str(),
                      cfg.port, cfg.maxPeers, cfg.name.c_str());
        log->log(LogLevel::Info, __FILE__, __LINE__, buf);
    }

    if (cfg.incomingBandwidthBps || cfg.outgoingBandwidthBps) {
        static_cast<ENetNetwork*>(net)->setBandwidthLimit(cfg.incomingBandwidthBps, cfg.outgoingBandwidthBps);
        char buf[96];
        std::snprintf(buf, sizeof(buf), "bandwidth cap: in=%u B/s out=%u B/s", cfg.incomingBandwidthBps,
                      cfg.outgoingBandwidthBps);
        log->log(LogLevel::Info, __FILE__, __LINE__, buf);
    }

    // ---- LAN discovery beacon ----
    uint8_t discoveryGameModeFlags = 0;
    for (const auto& m : cfg.gameModes) {
        if (m == "campaign")
            discoveryGameModeFlags |= fl::kGameModeCampaign;
        else if (m == "mission")
            discoveryGameModeFlags |= fl::kGameModeMission;
        else if (m == "sandbox")
            discoveryGameModeFlags |= fl::kGameModeSandbox;
    }
    std::unique_ptr<DiscoveryBeacon> beacon;
    if (cfg.discoveryEnabled) {
        DiscoveryBeacon::Config dcfg;
        dcfg.name = cfg.name;
        dcfg.port = cfg.port;
        dcfg.maxPlayers = static_cast<uint8_t>(cfg.maxPeers > 255 ? 255 : cfg.maxPeers);
        dcfg.gameModeFlags = discoveryGameModeFlags;
        dcfg.intervalMs = cfg.discoveryIntervalMs;
        dcfg.broadcastAddr = "255.255.255.255";
        beacon = std::make_unique<DiscoveryBeacon>(dcfg, *log);
        if (!beacon->isOpen()) {
            log->log(LogLevel::Warn, __FILE__, __LINE__, "LAN discovery beacon: no sockets opened; discovery disabled");
            beacon.reset();
        } else {
            log->log(LogLevel::Info, __FILE__, __LINE__, "LAN discovery beacon started");
        }
    }

    // ---- Entity system + sandbox entities ----
    fl::EntityTypeRegistry entityRegistry;
    fl::EntityManager entityManager(*log, entityRegistry);

    fl::EntityDef debugDef;
    debugDef.id = "builtin:debug-entity";
    debugDef.name = "Debug Entity";
    debugDef.category = fl::ObjectCategory::AirVehicle;
    debugDef.maxHp = 100.0f;
    entityRegistry.registerType(std::move(debugDef));

    // Spawn 5 entities in V-formation at 500 m altitude.
    const float kAlt = 500.0f;
    using Slot = std::pair<float, float>;
    const Slot kSlots[] = {{0.0f, 0.0f}, {-30.0f, -25.0f}, {30.0f, -25.0f}, {-60.0f, -50.0f}, {60.0f, -50.0f}};
    for (auto [x, z] : kSlots) {
        fl::EntityTransform t{};
        t.pos[0] = x;
        t.pos[1] = kAlt;
        t.pos[2] = z;
        entityManager.spawn("builtin:debug-entity", t);
    }

    // ---- WorldBroadcaster wires the sim loop to ENet ----
    fl::WeatherControllerParams wparams;
    wparams.timeScaleRatio = static_cast<float>(cfg.timeScale);
    fl::WeatherController weatherController(wparams);
    fl::WorldBroadcaster broadcaster(entityManager, entityRegistry, *net, *log, &weatherController);
    broadcaster.setRateLimitParams(cfg.connectRateLimitCount, cfg.connectRateLimitWindowS, cfg.packetFloodMultiplier);
    if (!cfg.banlistPath.empty()) {
        auto banned = loadIpListFile(cfg.banlistPath, log);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "banlist: loaded %zu IPs from %s", banned.size(), cfg.banlistPath.c_str());
        log->log(LogLevel::Info, __FILE__, __LINE__, buf);
        broadcaster.setBannedAddresses(std::move(banned));
    }
    if (!cfg.allowlistPath.empty()) {
        auto allowed = loadIpListFile(cfg.allowlistPath, log);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "allowlist: loaded %zu IPs from %s", allowed.size(), cfg.allowlistPath.c_str());
        log->log(LogLevel::Info, __FILE__, __LINE__, buf);
        broadcaster.setAllowedAddresses(std::move(allowed));
    }
    net->setEventHandler(&broadcaster);

    GameLoop gameLoop(broadcaster, *log);

    // ---- Signal handling ----
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    // ---- Start sim loop ----
    gameLoop.start();

    // ---- Admin console (stdin command loop) ----
    std::mutex stdinMutex;
    std::queue<std::string> stdinLines;
    std::thread([&stdinMutex, &stdinLines]() {
        std::string line;
        while (std::getline(std::cin, line)) {
            std::lock_guard<std::mutex> lk(stdinMutex);
            stdinLines.push(std::move(line));
        }
    }).detach();

    DebugCommandRegistry adminRegistry;
    ServerCommandContext adminCtx;
    adminCtx.broadcaster = &broadcaster;
    adminCtx.entityManager = &entityManager;
    adminCtx.typeRegistry = &entityRegistry;
    adminCtx.weatherController = &weatherController;
    adminCtx.beacon = beacon.get();
    adminCtx.gameLoop = &gameLoop;
    adminCtx.logger = log;
    adminCtx.configPath = &configPath;
    adminCtx.startTime = std::chrono::steady_clock::now();
    adminCtx.quitFlag = &g_quit;
    adminCtx.banlistPath = cfg.banlistPath.empty() ? nullptr : &cfg.banlistPath;
    adminCtx.allowlistPath = cfg.allowlistPath.empty() ? nullptr : &cfg.allowlistPath;
    adminCtx.saveBanlist = [&](const std::unordered_set<std::string>& b) { saveIpListFile(cfg.banlistPath, b, log); };
    adminCtx.loadBanlist = [&]() { return loadIpListFile(cfg.banlistPath, log); };
    adminCtx.loadAllowlist = [&]() { return loadIpListFile(cfg.allowlistPath, log); };
    adminCtx.shutdownWarningIntervalS = static_cast<uint32_t>(cfg.shutdownWarningIntervalS);
    adminCtx.minShutdownDelayS = static_cast<uint32_t>(cfg.minShutdownDelayS);
    adminCtx.shutdownRequireConfirm = cfg.shutdownRequireConfirm;

    broadcaster.setShutdownCallback([&]() { g_quit = 1; });
    registerServerCommands(adminRegistry, adminCtx);

    while (!g_quit) {
        {
            std::lock_guard<std::mutex> lk(stdinMutex);
            while (!stdinLines.empty()) {
                std::string line = std::move(stdinLines.front());
                stdinLines.pop();
                std::string result = adminRegistry.dispatch(line);
                if (!result.empty())
                    std::printf("[admin] %s\n", result.c_str());
            }
        }
        if (beacon)
            beacon->tick(broadcaster.getPeerCount());
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    gameLoop.stop();

    log->log(LogLevel::Info, __FILE__, __LINE__, "shutting down");
    net->disconnect();
    net->shutdown();

    return 0;
}
