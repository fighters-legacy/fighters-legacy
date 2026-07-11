// SPDX-License-Identifier: GPL-3.0-or-later
//
// bot_swarm — headless multi-client load generator for fighters-legacy.
//
// Spins up N synthetic game clients (each its own ENetNetwork) against a running fl-server,
// sustains MsgClientInput at a configurable rate using a pluggable flight pattern, and reports
// the client-observable scale metrics: connect success, downstream KB/s per client, RTT,
// observed server tick-Hz (from snapshot tickIndex progression), and worker loop dt.
//
// Pure client — point it at an existing host:port. See tools/bot_swarm/run_loadtest.sh to
// launch fl-server with a load-test config, and docs/load-testing.md for the methodology.
//
// Usage: bot_swarm [host] [port] [--clients N] [--duration S] [--rate HZ] [--ramp-ms MS]
//                  [--threads N] [--pattern NAME] [--json PATH]
//                  [--assert-min-tick-hz X] [--assert-max-kbs Y]
#include "BotClient.h"
#include "ENetNetworkFactory.h"
#include "SwarmConfig.h"
#include "SwarmMetrics.h"
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/resource.h>
#endif
#ifdef _WIN32
// NOMINMAX: <windows.h> otherwise defines min()/max() macros that clobber std::min/std::max
// (the threads-count math below). Keep the full header so <mmsystem.h>'s timeBeginPeriod
// (winmm) stays declared — WIN32_LEAN_AND_MEAN would drop it.
#define NOMINMAX
#include <windows.h>
#endif

using namespace fl;

namespace {

constexpr const char* kVersion = "0.1.0";

volatile sig_atomic_t g_quit = 0;
void onSignal(int) {
    g_quit = 1;
}

std::chrono::steady_clock::time_point g_start;
double nowS() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
}

// Shared result storage. Each worker writes a disjoint slice — no locks.
std::vector<ClientMetrics> g_metrics;
std::vector<std::vector<double>> g_loopDt;
std::vector<double> g_windowS;

void printHelp() {
    std::printf("Usage: bot_swarm [host] [port] [options]\n"
                "\n"
                "Headless load generator: connects N synthetic clients to a running fl-server.\n"
                "Point it at an existing host:port (default 127.0.0.1:4778). The server must allow\n"
                "the client count (max_peers) and rapid connects (connect_rate_limit) — see\n"
                "tools/bot_swarm/run_loadtest.sh and docs/load-testing.md.\n"
                "\n"
                "Options:\n"
                "  --clients N            Number of synthetic clients (default: 32)\n"
                "  --duration S           Soak duration in seconds (default: 30)\n"
                "  --rate HZ              MsgClientInput send rate per client (default: 60)\n"
                "  --ramp-ms MS           Delay between successive connects (default: 20)\n"
                "  --threads N            Worker threads (default: auto)\n"
                "  --pattern NAME         weave|level|aggressive|idle|random|trace:<file> (default: weave)\n"
                "  --pattern-mix SPEC     Weighted heterogeneous swarm, e.g. \"weave:80,aggressive:20\"\n"
                "                         (supersedes --pattern; deterministic per-client assignment)\n"
                "  --json PATH            Write a JSON report to PATH\n"
                "  --server-metrics PATH  Read fl-server --metrics-json file; embed authoritative server_tick block\n"
                "  --assert-min-tick-hz X Exit nonzero if observed server tick-Hz min < X\n"
                "  --assert-max-kbs Y     Exit nonzero if downstream KB/s per client max > Y\n"
                "  --assert-max-tick-ms X Exit nonzero if authoritative server tick p99 (ms) > X\n"
                "  --assert-min-entities N Exit nonzero if authoritative server_tick.entities < N\n"
                "  --assert-max-rss-growth-kb N  Exit nonzero if server RSS growth (rss_kb - rss_startup_kb) > N\n"
                "  --assert-max-load-factor X  Exit nonzero if server_tick.load_factor > X (governor engaged? <0=off)\n"
                "  --assert-max-dropped-ticks N  Exit nonzero if server_tick.dropped_ticks > N (<0=off)\n"
                "  --help, --version\n"
                "\n"
                "Environment:\n"
                "  FL_HOST    Server host (default: 127.0.0.1)\n"
                "  FL_PORT    Server port (default: 4778)\n");
}

// Raise the open-file soft limit toward the hard limit (each client is a UDP socket).
void raiseFdLimit(int clients) {
#ifndef _WIN32
    rlimit rl{};
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        if (rl.rlim_cur < rl.rlim_max) {
            rl.rlim_cur = rl.rlim_max;
            setrlimit(RLIMIT_NOFILE, &rl);
            getrlimit(RLIMIT_NOFILE, &rl);
        }
        const rlim_t need = static_cast<rlim_t>(clients) + 16u; // sockets + stdio + slack
        if (rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur < need)
            std::printf("[WARN ] open-file limit is %llu; %d clients may exhaust it (raise ulimit -n)\n",
                        static_cast<unsigned long long>(rl.rlim_cur), clients);
    }
#else
    (void)clients;
#endif
}

void runWorker(int threadIdx, int startIdx, int count, const SwarmConfig cfg, std::string host, SwarmPatternPlan plan) {
    using namespace std::chrono;
    std::vector<std::unique_ptr<BotClient>> bots;
    bots.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        const auto idx = static_cast<uint32_t>(startIdx + i);
        bots.push_back(std::make_unique<BotClient>(idx, plan.make(idx), cfg.rateHz));
    }

    // ---- Ramp connect ----
    for (int i = 0; i < count && !g_quit; ++i) {
        bots[static_cast<size_t>(i)]->connect(nowS(), host.c_str(), cfg.port);
        const auto rampEnd = steady_clock::now() + milliseconds(cfg.rampMs);
        do {
            const double n = nowS();
            for (int j = 0; j <= i; ++j) {
                bots[static_cast<size_t>(j)]->setNow(n);
                bots[static_cast<size_t>(j)]->service();
            }
            std::this_thread::sleep_for(milliseconds(1));
        } while (steady_clock::now() < rampEnd && !g_quit);
    }

    // ---- Wait for handshakes (up to 5 s) ----
    const auto connectDeadline = steady_clock::now() + seconds(5);
    for (;;) {
        int connected = 0;
        const double n = nowS();
        for (auto& b : bots) {
            b->setNow(n);
            b->service();
            if (b->connected())
                ++connected;
        }
        if (connected == count || steady_clock::now() >= connectDeadline || g_quit)
            break;
        std::this_thread::sleep_for(milliseconds(2));
    }

    // ---- Steady measurement window ----
    std::vector<uint64_t> byteBaseline(static_cast<size_t>(count), 0);
    const double windowStart = nowS();
    for (int i = 0; i < count; ++i)
        byteBaseline[static_cast<size_t>(i)] = bots[static_cast<size_t>(i)]->metrics().snapshotBytes;

    const auto interval = duration_cast<steady_clock::duration>(duration<double>(1.0 / cfg.rateHz));
    auto nextWake = steady_clock::now();
    double rttSampleAt = windowStart;
    auto& dts = g_loopDt[static_cast<size_t>(threadIdx)];
    while (!g_quit) {
        const double n = nowS();
        if (n - windowStart >= static_cast<double>(cfg.durationS))
            break;
        const auto t0 = steady_clock::now();
        for (auto& b : bots) {
            b->setNow(n);
            b->service();
        }
        for (auto& b : bots)
            b->sendInputIfDue(n);
        if (n - rttSampleAt >= 1.0) {
            for (auto& b : bots)
                b->sampleRtt();
            rttSampleAt = n;
        }
        dts.push_back(duration<double, std::milli>(steady_clock::now() - t0).count());

        nextWake += interval;
        const auto now = steady_clock::now();
        if (nextWake > now)
            std::this_thread::sleep_until(nextWake);
        else
            nextWake = now; // fell behind — don't spiral
    }
    const double windowEnd = nowS();

    // ---- Final RTT + graceful disconnect drain ----
    for (auto& b : bots)
        b->sampleRtt();
    for (auto& b : bots)
        b->beginDisconnect();
    const auto drainEnd = steady_clock::now() + milliseconds(200);
    while (steady_clock::now() < drainEnd) {
        const double n = nowS();
        for (auto& b : bots) {
            b->setNow(n);
            b->service();
        }
        std::this_thread::sleep_for(milliseconds(5));
    }

    // ---- Export windowed metrics ----
    for (int i = 0; i < count; ++i) {
        ClientMetrics m = bots[static_cast<size_t>(i)]->metrics();
        const uint64_t base = byteBaseline[static_cast<size_t>(i)];
        m.snapshotBytes = m.snapshotBytes >= base ? m.snapshotBytes - base : 0;
        g_metrics[static_cast<size_t>(startIdx + i)] = m;
    }
    g_windowS[static_cast<size_t>(threadIdx)] = windowEnd - windowStart;

    for (auto& b : bots)
        b->shutdown();
}

} // namespace

int main(int argc, char** argv) {
    SwarmParseResult pr = parseSwarmArgs(argc, argv);
    if (pr.status == ParseStatus::Help) {
        printHelp();
        return 0;
    }
    if (pr.status == ParseStatus::Version) {
        std::printf("bot_swarm %s (%s)\n", kVersion, enetLibraryVersion());
        return 0;
    }
    if (pr.status == ParseStatus::Error) {
        std::printf("[ERROR] %s\n", pr.error.c_str());
        return 2;
    }
    SwarmConfig cfg = pr.cfg;

    // Env fallback only when not given positionally.
    if (!pr.hostSet) {
        if (const char* e = std::getenv("FL_HOST"))
            cfg.host = e;
    }
    if (!pr.portSet) {
        if (const char* e = std::getenv("FL_PORT"))
            cfg.port = static_cast<uint16_t>(std::atoi(e));
    }

    // Worker thread count.
    int threads = cfg.threads;
    if (threads <= 0) {
        unsigned hw = std::thread::hardware_concurrency();
        if (hw == 0)
            hw = 4;
        const int byLoad = (cfg.clients + 31) / 32; // ceil(clients/32)
        threads = std::min(static_cast<int>(hw), std::max(1, byLoad));
    }
    threads = std::min(threads, cfg.clients);
    threads = std::max(threads, 1);

    raiseFdLimit(cfg.clients);

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
#ifdef _WIN32
    timeBeginPeriod(1);
#endif

    // Resolve the flight pattern once for the whole swarm: a shared trace, a weighted mix, or a
    // single built-in. A trace file is loaded (and shared read-only) here so N clients don't each
    // re-read it. The CLI already validated the mix spec / pattern shape.
    SwarmPatternPlan plan;
    plan.totalClients = cfg.clients;
    if (!cfg.patternMix.empty()) {
        std::string err;
        if (!parsePatternMix(cfg.patternMix, plan.mix, err)) {
            std::printf("[ERROR] --pattern-mix: %s\n", err.c_str());
            return 2;
        }
    } else if (isTracePattern(cfg.pattern)) {
        const std::string path(tracePatternPath(cfg.pattern));
        InputTrace tr;
        std::string err;
        if (!loadInputTraceFile(path, tr, err)) {
            std::printf("[ERROR] %s\n", err.c_str());
            return 2;
        }
        if (tr.records.empty()) {
            std::printf("[ERROR] trace %s has no input records\n", path.c_str());
            return 2;
        }
        std::printf("[INFO ] loaded trace %s: %zu records @ %u Hz\n", path.c_str(), tr.records.size(), tr.tickRate);
        plan.trace = std::make_shared<const InputTrace>(std::move(tr));
    } else {
        plan.single = cfg.pattern;
    }

    const std::string patternLabel = cfg.patternMix.empty() ? cfg.pattern : cfg.patternMix;
    std::printf("[INFO ] bot_swarm: %d clients, %d threads, pattern=%s, rate=%dHz, duration=%ds -> %s:%u\n",
                cfg.clients, threads, patternLabel.c_str(), cfg.rateHz, cfg.durationS, cfg.host.c_str(), cfg.port);

    g_metrics.assign(static_cast<size_t>(cfg.clients), ClientMetrics{});
    g_loopDt.assign(static_cast<size_t>(threads), {});
    g_windowS.assign(static_cast<size_t>(threads), 0.0);
    g_start = std::chrono::steady_clock::now();

    // Partition clients across workers.
    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(threads));
    const int base = cfg.clients / threads;
    const int rem = cfg.clients % threads;
    int next = 0;
    for (int t = 0; t < threads; ++t) {
        const int count = base + (t < rem ? 1 : 0);
        const int startIdx = next;
        next += count;
        workers.emplace_back(runWorker, t, startIdx, count, cfg, cfg.host, plan);
    }
    for (auto& w : workers)
        w.join();

#ifdef _WIN32
    timeEndPeriod(1);
#endif

    // Aggregate.
    double elapsed = 0.0;
    int nz = 0;
    for (double w : g_windowS)
        if (w > 0.0) {
            elapsed += w;
            ++nz;
        }
    elapsed = nz > 0 ? elapsed / nz : static_cast<double>(cfg.durationS);

    std::vector<double> allDt;
    for (auto& v : g_loopDt)
        allDt.insert(allDt.end(), v.begin(), v.end());

    // Read the authoritative server-side tick budget (if fl-server was given --metrics-json and
    // the path was passed via --server-metrics). Missing/empty/malformed -> no server block.
    std::optional<ServerTickReport> serverMetrics;
    if (!cfg.serverMetricsPath.empty()) {
        serverMetrics = loadServerMetrics(cfg.serverMetricsPath);
        if (!serverMetrics)
            std::printf("[WARN ] could not read server metrics from %s\n", cfg.serverMetricsPath.c_str());
    }

    const SwarmReport report = buildReport(cfg, g_metrics, elapsed, std::move(allDt), threads, serverMetrics);
    printReport(report);

    if (!cfg.jsonPath.empty()) {
        const std::string json = reportToJson(report);
        if (FILE* f = std::fopen(cfg.jsonPath.c_str(), "wb")) {
            std::fwrite(json.data(), 1, json.size(), f);
            std::fclose(f);
            std::printf("[INFO ] wrote %s\n", cfg.jsonPath.c_str());
        } else {
            std::printf("[WARN ] could not write %s\n", cfg.jsonPath.c_str());
        }
    }

    // Exit code: all requested clients connected, none dropped unexpectedly, asserts pass.
    const bool ok = report.clientsConnected == report.clientsRequested && report.clientsDisconnected == 0 &&
                    report.assertsPassed && g_quit == 0;
    return ok ? 0 : 1;
}
