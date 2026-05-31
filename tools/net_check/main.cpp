// SPDX-License-Identifier: GPL-3.0-or-later
//
// net_check — ENet transport smoke-test for fighters-legacy
//
// Usage: net_check [host] [port] [--count N] [--interval MS]
//
// Connects to fl-server, sends periodic "net_check ping N" packets, then
// disconnects cleanly. Intended for manual smoke-testing of the ENet backend
// alongside fl-server; not a production binary.
#include "ENetNetworkFactory.h"
#include <ILogger.h>
#include <Platform.h>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

// ---------------------------------------------------------------------------
// Version
// ---------------------------------------------------------------------------

static constexpr const char* kVersion = "0.0.1";

// ---------------------------------------------------------------------------
// Minimal stdout logger (identical to fl-server)
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

struct ClientEventHandler : INetworkEventHandler {
    ILogger* logger;
    bool connected{false};
    bool disconnected{false};

    void onConnect(uint32_t /*peerId*/) override {
        connected = true;
        logger->log(LogLevel::Info, __FILE__, __LINE__, "connected");
    }
    void onDisconnect(uint32_t /*peerId*/) override {
        disconnected = true;
        logger->log(LogLevel::Info, __FILE__, __LINE__, "disconnected");
    }
    void onReceive(uint32_t /*peerId*/, const void* /*data*/, std::size_t /*size*/) override {}
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

static constexpr const char* kDefaultHost = "127.0.0.1";
static constexpr uint16_t kDefaultPort = 4778;
static constexpr int kDefaultInterval = 1000;  // ms between pings
static constexpr int kConnectTimeoutMs = 5000; // 5 s connect timeout

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    const char* host = kDefaultHost;
    uint16_t port = kDefaultPort;
    int count = 0; // 0 = unlimited
    int interval = kDefaultInterval;

    // Parse args — two positional args first, then named flags
    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::printf("Usage: net_check [host] [port] [--count N] [--interval MS]\n"
                        "\n"
                        "Options:\n"
                        "  --help           Print this message and exit\n"
                        "  --version        Print version and exit\n"
                        "  --count N / -n N Send N packets then disconnect (default: unlimited)\n"
                        "  --interval MS    Milliseconds between pings (default: 1000)\n"
                        "\n"
                        "Environment:\n"
                        "  FL_HOST    Server host (default: 127.0.0.1)\n"
                        "  FL_PORT    Server port (default: 4778)\n");
            return 0;
        }
        if (std::strcmp(argv[i], "--version") == 0 || std::strcmp(argv[i], "-v") == 0) {
            std::printf("net_check %s (%s)\n", kVersion, enetLibraryVersion());
            return 0;
        }
        if ((std::strcmp(argv[i], "--count") == 0 || std::strcmp(argv[i], "-n") == 0) && i + 1 < argc) {
            count = std::atoi(argv[++i]);
            continue;
        }
        if (std::strcmp(argv[i], "--interval") == 0 && i + 1 < argc) {
            interval = std::atoi(argv[++i]);
            continue;
        }
        // Positional
        if (positional == 0) {
            host = argv[i];
            ++positional;
        } else if (positional == 1) {
            port = static_cast<uint16_t>(std::atoi(argv[i]));
            ++positional;
        }
    }

    // Env vars override positional defaults but not explicit positional args
    if (positional < 1) {
        if (const char* e = std::getenv("FL_HOST"))
            host = e;
    }
    if (positional < 2) {
        if (const char* e = std::getenv("FL_PORT"))
            port = static_cast<uint16_t>(std::atoi(e));
    }

    // ---- Set up platform ----
    Platform p;
    p.logger = std::make_unique<StdoutLogger>();
    p.network = createENetNetwork();

    ILogger* log = p.logger.get();
    INetwork* net = p.network.get();

    // ---- Init network ----
    if (!net->init()) {
        log->log(LogLevel::Error, __FILE__, __LINE__, "network init failed");
        return 1;
    }

    ClientEventHandler handler;
    handler.logger = log;
    net->setEventHandler(&handler);

    // ---- Connect ----
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "connecting to %s:%u...", host, port);
        log->log(LogLevel::Info, __FILE__, __LINE__, buf);
    }

    if (!net->connect(host, port)) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "connect failed: %s", net->getLastError() ? net->getLastError() : "unknown");
        log->log(LogLevel::Error, __FILE__, __LINE__, buf);
        net->shutdown();
        return 1;
    }

    // Pump up to kConnectTimeoutMs for onConnect to fire
    {
        const int kStep = 10;
        int elapsed = 0;
        while (!handler.connected && elapsed < kConnectTimeoutMs) {
            net->service(kStep);
            elapsed += kStep;
        }
    }

    if (!handler.connected) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "connection to %s:%u timed out", host, port);
        log->log(LogLevel::Error, __FILE__, __LINE__, buf);
        net->shutdown();
        return 1;
    }

    {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "connected to %s:%u", host, port);
        log->log(LogLevel::Info, __FILE__, __LINE__, buf);
    }

    // ---- Signal handling ----
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    // ---- Main loop ----
    using Clock = std::chrono::steady_clock;
    auto lastPing = Clock::now() - std::chrono::milliseconds(interval); // send first ping immediately
    int pingsSent = 0;

    while (!g_quit && !handler.disconnected) {
        net->service(10);

        auto now = Clock::now();
        auto msSincePing = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastPing).count();
        if (msSincePing >= interval) {
            ++pingsSent;
            char payload[64];
            std::snprintf(payload, sizeof(payload), "net_check ping %d", pingsSent);

            {
                char buf[80];
                std::snprintf(buf, sizeof(buf), "sent ping %d", pingsSent);
                log->log(LogLevel::Info, __FILE__, __LINE__, buf);
            }

            net->send(0, payload, std::strlen(payload) + 1, /*reliable=*/true);
            lastPing = now;

            if (count > 0 && pingsSent >= count)
                break;
        }
    }

    // ---- Graceful shutdown ----
    log->log(LogLevel::Info, __FILE__, __LINE__, "disconnecting");
    net->disconnect();
    net->shutdown();

    return 0;
}
