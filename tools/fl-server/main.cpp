// SPDX-License-Identifier: GPL-3.0-or-later
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
#include "ENetNetwork.h"
#include "server_config.h"
#include <ILogger.h>
#include <Platform.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <enet/enet.h>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Version
// ---------------------------------------------------------------------------

static constexpr const char* kVersion = "0.0.1";

// ---------------------------------------------------------------------------
// Minimal stdout logger
// ---------------------------------------------------------------------------

struct StdoutLogger : ILogger {
    void log(LogLevel level, const char* /*file*/, int /*line*/, const char* message) override {
        const char* tag = level == LogLevel::Debug  ? "DEBUG"
                          : level == LogLevel::Info ? "INFO "
                          : level == LogLevel::Warn ? "WARN "
                                                    : "ERROR";
        std::printf("[%s] %s\n", tag, message);
        std::fflush(stdout);
    }
    void setMinLevel(LogLevel) override {}
    void flush() override {
        std::fflush(stdout);
    }
};

// ---------------------------------------------------------------------------
// Event handler
// ---------------------------------------------------------------------------

struct ServerEventHandler : INetworkEventHandler {
    ILogger* logger;
    INetwork* network;

    void onConnect(uint32_t peerId) override {
        const char* addr = network->getPeerAddress(peerId);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "peer %u connected from %s", peerId, addr ? addr : "unknown");
        logger->log(LogLevel::Info, __FILE__, __LINE__, buf);
    }
    void onDisconnect(uint32_t peerId) override {
        const char* addr = network->getPeerAddress(peerId);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "peer %u disconnected (%s)", peerId, addr ? addr : "unknown");
        logger->log(LogLevel::Info, __FILE__, __LINE__, buf);
    }
    void onReceive(uint32_t /*peerId*/, const void* /*data*/, std::size_t /*size*/) override {
        // Phase 1: no game protocol yet — packets are discarded.
    }
};

// ---------------------------------------------------------------------------
// Signal handling
// ---------------------------------------------------------------------------

static volatile sig_atomic_t g_quit = 0;

static void onSignal(int) {
    g_quit = 1;
}

// ---------------------------------------------------------------------------
// Default config template (written on first run)
// ---------------------------------------------------------------------------

static const char* kDefaultToml =
    "[server]\n"
    "# Human-readable server name shown in the lobby browser.\n"
    "name = \"Unnamed Server\"\n"
    "\n"
    "# UDP port fl-server binds on. Port 4778 is the fighters-legacy default.\n"
    "# See IANA registration note in docs/architecture.md.\n"
    "port = 4778\n"
    "\n"
    "# Network interface to bind on.\n"
    "# \"0.0.0.0\"   = all interfaces (internet-accessible server)\n"
    "# \"127.0.0.1\" = localhost-only (single-player mode; game client sets this)\n"
    "# Phase 2: enforcement requires INetwork::bind() to be extended.\n"
    "bind_address = \"0.0.0.0\"\n"
    "\n"
    "# Maximum number of simultaneous connected peers (1-128).\n"
    "max_peers = 16\n"
    "\n"
    "# Scenario types this server will host.\n"
    "# Valid values: \"campaign\", \"mission\", \"sandbox\"\n"
    "# Whether a session is cooperative or adversarial depends on which faction\n"
    "# players join, not on a separate mode flag.\n"
    "game_modes = [\"campaign\", \"mission\", \"sandbox\"]\n"
    "\n"
    "# Message shown to connecting clients. Empty string = no message.\n"
    "motd = \"\"\n"
    "\n"
    "# Server password. Empty string = no password required.\n"
    "# Note: store passwords in this file only -- do not use an env var,\n"
    "# as environment variables appear in process listings.\n"
    "password = \"\"\n"
    "\n"
    "[rotation]\n"
    "# Phase 2 -- rotation logic lands with the game server runtime.\n"
    "# Mission/campaign time limits belong in their content files (win/loss conditions).\n"
    "# This section controls server-level scenario cycling and sandbox session capping.\n"
    "#\n"
    "# Cycle order for rotation items: \"sequential\" or \"random\".\n"
    "order = \"sequential\"\n"
    "\n"
    "# Ordered list of mission, campaign, or sandbox theater IDs to cycle through.\n"
    "# Empty = no automatic rotation.\n"
    "items = []\n"
    "\n"
    "# Sandbox session time limit in minutes. Triggers rotation advancement when elapsed.\n"
    "# 0 = no limit. Missions and campaigns are unaffected (they end on win/loss).\n"
    "time_limit_min = 0\n"
    "\n"
    "[lobby]\n"
    "# Phase 2 -- lobby registration is not yet active (see issue #36).\n"
    "#\n"
    "# Set to true to advertise this server to the fl-lobby matchmaking service.\n"
    "register = false\n"
    "\n"
    "# fl-lobby REST base URL. Ignored unless register = true.\n"
    "url = \"https://lobby.fighters-legacy.org\"\n"
    "\n"
    "# Server visibility in the lobby browser.\n"
    "# \"public\"  = visible to all players\n"
    "# \"private\" = token-gated (Phase 2)\n"
    "visibility = \"public\"\n"
    "\n"
    "[mods]\n"
    "# Phase 2 -- parsed and logged; ModLoader integration pending.\n"
    "#\n"
    "# Ordered list of mod IDs to load. Index 0 = highest priority.\n"
    "# IDs must match the [mod].id field in each mod's manifest.toml.\n"
    "stack = []\n"
    "\n"
    "[world]\n"
    "# Settings below apply only when fl-server is launched with --persistent.\n"
    "# Phase 2 -- persistent-world logic is not yet implemented.\n"
    "#\n"
    "# Path to the persistent world save file.\n"
    "save_path = \"world.sav\"\n"
    "\n"
    "# Autosave interval in seconds. 0 = disabled.\n"
    "autosave_interval_s = 300\n"
    "\n"
    "[ai]\n"
    "# Phase 2 -- parsed and stored; server-side enforcement lands with the AI runtime.\n"
    "#\n"
    "# Minimum AI difficulty enforced server-side regardless of client preference.\n"
    "# Valid values: \"recruit\", \"cadet\", \"veteran\", \"ace\"\n"
    "difficulty_floor = \"recruit\"\n";

// ---------------------------------------------------------------------------
// Config file helpers
// ---------------------------------------------------------------------------

static void writeDefaultConfig(const std::string& path, ILogger* logger) {
    std::ofstream f(path);
    if (!f) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "could not write default config to %s — continuing", path.c_str());
        logger->log(LogLevel::Warn, __FILE__, __LINE__, buf);
        return;
    }
    f << kDefaultToml;
    logger->log(LogLevel::Info, __FILE__, __LINE__, "wrote default server.toml");
}

static std::string readFileContent(const std::string& path) {
    std::ifstream f(path);
    if (!f)
        return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    // Pre-pass: --help / --version / --persistent
    bool flagPersistent = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::printf("Usage: fl-server [port] [maxPeers]\n"
                        "\n"
                        "Options:\n"
                        "  --help          Print this message and exit\n"
                        "  --version       Print version and exit\n"
                        "  --persistent    Enable persistent world mode (Phase 2 -- not yet active)\n"
                        "\n"
                        "Environment:\n"
                        "  FL_CONFIG              Path to server.toml (default: ./server.toml)\n"
                        "  FL_PORT                Bind port (default: 4778)\n"
                        "  FL_BIND_ADDRESS        Bind address (default: 0.0.0.0; use 127.0.0.1 for localhost-only)\n"
                        "  FL_MAX_PEERS           Max simultaneous peers (default: 16)\n"
                        "  FL_NAME                Server name (default: \"Unnamed Server\")\n"
                        "  FL_PERSISTENT          \"true\" to enable persistent world, Phase 2 (default: \"false\")\n"
                        "  FL_LOBBY_REGISTER      \"true\" to advertise to fl-lobby, Phase 2 (default: \"false\")\n"
                        "  FL_LOBBY_URL           fl-lobby base URL, Phase 2\n"
                        "  FL_LOBBY_VISIBILITY    \"public\" or \"private\", Phase 2 (default: \"public\")\n"
                        "  FL_AI_DIFFICULTY_FLOOR recruit/cadet/veteran/ace, Phase 2 (default: \"recruit\")\n"
                        "\n"
                        "Config file is written with defaults on first run if absent.\n"
                        "See docs/fl-server-config.md for the full operator reference.\n");
            return 0;
        }
        if (std::strcmp(argv[i], "--version") == 0 || std::strcmp(argv[i], "-v") == 0) {
            std::printf("fl-server %s (ENet %d.%d.%d)\n", kVersion, ENET_VERSION_MAJOR, ENET_VERSION_MINOR,
                        ENET_VERSION_PATCH);
            return 0;
        }
        if (std::strcmp(argv[i], "--persistent") == 0)
            flagPersistent = true;
    }

    // ---- Set up platform ----
    Platform p;
    p.logger = std::make_unique<StdoutLogger>();
    p.network = std::make_unique<ENetNetwork>();

    ILogger* log = p.logger.get();
    INetwork* net = p.network.get();

    {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "fl-server %s (ENet %d.%d.%d) starting", kVersion, ENET_VERSION_MAJOR,
                      ENET_VERSION_MINOR, ENET_VERSION_PATCH);
        log->log(LogLevel::Info, __FILE__, __LINE__, buf);
    }

    // ---- Tier 1: server.toml ----
    const char* configEnv = std::getenv("FL_CONFIG");
    std::string configPath = configEnv ? configEnv : "server.toml";
    {
        std::ifstream probe(configPath);
        if (!probe)
            writeDefaultConfig(configPath, log);
    }
    ServerConfig cfg = parseServerConfig(readFileContent(configPath), log);

    // ---- Tier 2: CLI positional args ----
    if (argc >= 2 && argv[1][0] != '-')
        cfg.port = static_cast<uint16_t>(std::atoi(argv[1]));
    if (argc >= 3 && argv[2][0] != '-')
        cfg.maxPeers = std::atoi(argv[2]);

    // ---- Tier 3: environment variables (highest precedence) ----
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
        // Validate: accepted values are "public" and "private"
        if (std::strcmp(e, "public") == 0 || std::strcmp(e, "private") == 0) {
            cfg.lobbyVisibility = e;
        } else {
            log->log(LogLevel::Warn, __FILE__, __LINE__,
                     "FL_LOBBY_VISIBILITY must be \"public\" or \"private\"; ignoring");
        }
    }
    if (const char* e = std::getenv("FL_AI_DIFFICULTY_FLOOR")) {
        if (std::strcmp(e, "recruit") == 0 || std::strcmp(e, "cadet") == 0 || std::strcmp(e, "veteran") == 0 ||
            std::strcmp(e, "ace") == 0) {
            cfg.aiDifficultyFloor = e;
        } else {
            log->log(LogLevel::Warn, __FILE__, __LINE__,
                     "FL_AI_DIFFICULTY_FLOOR must be recruit/cadet/veteran/ace; ignoring");
        }
    }

    // --persistent flag (CLI pre-pass result applied after config merge)
    if (flagPersistent)
        cfg.persistent = true;

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

    ServerEventHandler handler;
    handler.logger = log;
    handler.network = net;
    net->setEventHandler(&handler);

    if (!net->bind(cfg.port, cfg.maxPeers)) {
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

    // ---- Signal handling ----
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal); // no-op on Windows; fine for Linux/macOS containers

    // ---- Main loop ----
    // Phase 1: 10 ms service tick (~100 Hz). When the engine game loop workstream
    // lands, fl-server will run a fixed-timestep update and switch to service(0).
    while (!g_quit) {
        net->service(10);
    }

    // ---- Graceful shutdown ----
    log->log(LogLevel::Info, __FILE__, __LINE__, "shutting down");
    net->disconnect(); // drains peers up to 100 ms
    net->shutdown();

    return 0;
}
