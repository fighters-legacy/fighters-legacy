// SPDX-License-Identifier: GPL-3.0-or-later
//
// net_check — ENet transport smoke-test and latency bench for fighters-legacy
//
// Usage: net_check [host] [port] [--count N] [--interval MS]
//        net_check [host] [port] --bench N [--bench-rate HZ]
//
// Smoke-test mode: connects to fl-server, sends periodic "net_check ping N" packets,
// then disconnects cleanly.
//
// Bench mode (--bench N): connects, collects N round-trip-time samples from ENet's
// internal RTT tracker at the given rate (default 60 Hz), then prints statistics and
// disconnects. Intended for the loopback latency analysis (see tools/latency_analysis/).
#include "ENetNetworkFactory.h"
#include "NetStats.h"
#include <ILogger.h>
#include <Platform.h>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

using namespace fl;

// ---------------------------------------------------------------------------
// Version
// ---------------------------------------------------------------------------

static constexpr const char* kVersion = "0.1.0";

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
static constexpr int kDefaultInterval = 1000;  // ms between pings (smoke-test mode)
static constexpr int kConnectTimeoutMs = 5000; // 5 s connect timeout
static constexpr int kDefaultBenchRate = 60;   // Hz

// Statistics helpers (Stats / computeStats / printStats) live in tools/common/NetStats.h.

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    const char* host = kDefaultHost;
    uint16_t port = kDefaultPort;
    int count = 0; // 0 = unlimited (smoke-test mode)
    int interval = kDefaultInterval;
    int benchCount = 0; // 0 = smoke-test mode
    int benchRate = kDefaultBenchRate;

    // Parse args — two positional args first, then named flags
    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::printf("Usage: net_check [host] [port] [--count N] [--interval MS]\n"
                        "       net_check [host] [port] --bench N [--bench-rate HZ]\n"
                        "\n"
                        "Smoke-test mode (default):\n"
                        "  Connects and sends N periodic pings; --count 0 = unlimited.\n"
                        "\n"
                        "Bench mode (--bench N):\n"
                        "  Collects N ENet RTT samples at the given rate, then prints statistics.\n"
                        "  Requires fl-server to be running on the target host:port.\n"
                        "\n"
                        "Options:\n"
                        "  --help              Print this message and exit\n"
                        "  --version           Print version and exit\n"
                        "  --count N / -n N    Smoke-test: send N packets then disconnect\n"
                        "  --interval MS       Smoke-test: ms between pings (default: 1000)\n"
                        "  --bench N           Bench mode: collect N RTT samples\n"
                        "  --bench-rate HZ     Bench mode: sample rate in Hz (default: 60)\n"
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
        if (std::strcmp(argv[i], "--bench") == 0 && i + 1 < argc) {
            benchCount = std::atoi(argv[++i]);
            continue;
        }
        if (std::strcmp(argv[i], "--bench-rate") == 0 && i + 1 < argc) {
            benchRate = std::atoi(argv[++i]);
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

    if (benchRate <= 0)
        benchRate = kDefaultBenchRate;

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

    using Clock = std::chrono::steady_clock;

    // ---- Bench mode ----
    if (benchCount > 0) {
        const int serviceMs = 1000 / benchRate;
        std::vector<double> rttSamples;
        std::vector<double> dtSamples;
        rttSamples.reserve(static_cast<std::size_t>(benchCount));
        dtSamples.reserve(static_cast<std::size_t>(benchCount));

        {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "bench: collecting %d samples at %d Hz", benchCount, benchRate);
            log->log(LogLevel::Info, __FILE__, __LINE__, buf);
        }

        for (int i = 0; i < benchCount && !g_quit && !handler.disconnected; ++i) {
            auto t0 = Clock::now();
            net->service(serviceMs);
            auto t1 = Clock::now();
            rttSamples.push_back(static_cast<double>(net->getPeerRtt(0)));
            dtSamples.push_back(
                static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()) / 1000.0);
        }

        // ---- Print statistics ----
        int n = static_cast<int>(rttSamples.size());
        Stats rttStats = computeStats(rttSamples);
        Stats dtStats = computeStats(dtSamples);
        std::printf("\n--- bench results ---\n");
        printStats("ENet RTT", rttStats, n, "ms");
        printStats("Round dt", dtStats, n, "ms");
        std::printf("---\n");

        log->log(LogLevel::Info, __FILE__, __LINE__, "disconnecting");
        net->disconnect();
        net->shutdown();
        return 0;
    }

    // ---- Smoke-test mode ----
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
