// SPDX-License-Identifier: GPL-3.0-or-later
//
// fl-server — headless dedicated server for fighters-legacy
//
// Configuration (later tiers override earlier ones):
//   1. server.toml in CWD (or path in FL_CONFIG env var)
//   2. CLI positional args: fl-server [port] [maxPeers]
//   3. Environment variables: FL_PORT, FL_MAX_PEERS  (highest precedence)
//
// See docs/development.md for full operator documentation.
// fl-lobby integration is tracked in issue #36.
#include "ENetNetwork.h"
#include <ILogger.h>
#include <Platform.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <enet/enet.h>
#include <fstream>
#include <memory>
#include <string>
#include <toml++/toml.hpp>

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
// Defaults
// ---------------------------------------------------------------------------

static constexpr uint16_t kDefaultPort = 4778;
static constexpr int kDefaultMaxPeers = 16;

static const char* kDefaultToml = "[server]\n"
                                  "# UDP port fl-server binds on. Port 4778 is the fighters-legacy default.\n"
                                  "# See IANA registration note in docs/architecture.md.\n"
                                  "port = 4778\n"
                                  "\n"
                                  "# Maximum number of simultaneous connected peers.\n"
                                  "max_peers = 16\n";

// ---------------------------------------------------------------------------
// Config resolution
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

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    // --help
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::printf("Usage: fl-server [port] [maxPeers]\n"
                        "\n"
                        "Options:\n"
                        "  --help      Print this message and exit\n"
                        "  --version   Print version and exit\n"
                        "\n"
                        "Environment:\n"
                        "  FL_PORT       Bind port (default: 4778)\n"
                        "  FL_MAX_PEERS  Max simultaneous peers (default: 16)\n"
                        "  FL_CONFIG     Path to server.toml (default: ./server.toml)\n"
                        "\n"
                        "Config file is written with defaults on first run if absent.\n");
            return 0;
        }
        if (std::strcmp(argv[i], "--version") == 0 || std::strcmp(argv[i], "-v") == 0) {
            std::printf("fl-server %s (ENet %d.%d.%d)\n", kVersion, ENET_VERSION_MAJOR, ENET_VERSION_MINOR,
                        ENET_VERSION_PATCH);
            return 0;
        }
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

    // ---- Resolve config ----
    uint16_t port = kDefaultPort;
    int maxPeers = kDefaultMaxPeers;

    // Tier 1: server.toml
    const char* configEnv = std::getenv("FL_CONFIG");
    std::string configPath = configEnv ? configEnv : "server.toml";

    {
        std::ifstream probe(configPath);
        if (!probe) {
            writeDefaultConfig(configPath, log);
        }
    }

    try {
        auto tbl = toml::parse_file(configPath);
        if (auto v = tbl["server"]["port"].value<int64_t>())
            port = static_cast<uint16_t>(*v);
        if (auto v = tbl["server"]["max_peers"].value<int64_t>())
            maxPeers = static_cast<int>(*v);
    } catch (const toml::parse_error& e) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "failed to parse %s: %s — using defaults", configPath.c_str(), e.what());
        log->log(LogLevel::Warn, __FILE__, __LINE__, buf);
    }

    // Tier 2: CLI positional args
    if (argc >= 2)
        port = static_cast<uint16_t>(std::atoi(argv[1]));
    if (argc >= 3)
        maxPeers = std::atoi(argv[2]);

    // Tier 3: environment variables (highest precedence)
    if (const char* e = std::getenv("FL_PORT"))
        port = static_cast<uint16_t>(std::atoi(e));
    if (const char* e = std::getenv("FL_MAX_PEERS"))
        maxPeers = std::atoi(e);

    // ---- Init network ----
    if (!net->init()) {
        log->log(LogLevel::Error, __FILE__, __LINE__, "network init failed");
        return 1;
    }

    ServerEventHandler handler;
    handler.logger = log;
    handler.network = net;
    net->setEventHandler(&handler);

    if (!net->bind(port, maxPeers)) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "bind failed: %s", net->getLastError() ? net->getLastError() : "unknown");
        log->log(LogLevel::Error, __FILE__, __LINE__, buf);
        net->shutdown();
        return 1;
    }

    {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "listening on 0.0.0.0:%u (max %d peers)", port, maxPeers);
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
