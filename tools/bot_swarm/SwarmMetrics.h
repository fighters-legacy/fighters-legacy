// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// SwarmMetrics — per-client counters, swarm aggregation, and the JSON report.
//
// Each synthetic client owns a ClientMetrics written only by its owning worker thread, then
// read on the main thread after the run (no locks). buildReport()/printReport()/reportToJson()
// are pure functions over the collected metrics — unit-tested without sockets.
//
// The JSON shape is the #520/#513 contract. schema_version = 2 adds the authoritative
// "server_tick" sibling block (per-phase server tick budget) read from the fl-server
// --metrics-json file via --server-metrics; the client-side "observed_server_tick_hz" proxy
// is retained for comparison (extend, don't replace).

#include "NetStats.h"
#include "SwarmConfig.h"
#include "perf/ServerTickReport.h"
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace fl {

constexpr int kSwarmReportSchemaVersion = 2;

// Written by one worker thread; read after the run.
struct ClientMetrics {
    bool connected{false};
    bool disconnectedDuringRun{false};
    double connectMs{0.0};
    uint64_t snapshotBytes{0}; // WorldSnapshot payload bytes (downstream bandwidth)
    uint64_t snapshotCount{0};
    uint64_t inputsSent{0};
    uint64_t firstSnapshotTick{0};
    uint64_t lastSnapshotTick{0};
    double firstSnapshotWall{0.0}; // steady seconds
    double lastSnapshotWall{0.0};
    double maxSnapshotGapMs{0.0};
    uint32_t rttMs{0};
    bool rttValid{false};

    double observedTickHz() const {
        const double dt = lastSnapshotWall - firstSnapshotWall;
        if (dt <= 0.0 || lastSnapshotTick <= firstSnapshotTick)
            return 0.0;
        return static_cast<double>(lastSnapshotTick - firstSnapshotTick) / dt;
    }
    double downstreamKbs(double elapsedS) const {
        if (elapsedS <= 0.0)
            return 0.0;
        return static_cast<double>(snapshotBytes) / elapsedS / 1024.0;
    }
};

struct SwarmReport {
    std::string host;
    uint16_t port{0};
    int clientsRequested{0};
    int clientsConnected{0};
    int clientsRefused{0};
    int clientsDisconnected{0};
    double durationS{0.0};
    int rateHz{0};
    std::string pattern;
    int threads{0};
    Stats tickHz;
    Stats downstreamKbs;
    Stats rttMs;
    Stats connectMs;
    Stats workerLoopDtMs;
    int workerLoopDtSamples{0};
    double aggregateDownstreamMbs{0.0};
    double maxSnapshotGapMs{0.0};
    bool assertsPassed{true};
    double assertMinTickHz{0.0};
    double assertMaxKbs{0.0};
    double assertMaxTickMs{0.0};

    // Authoritative server-side tick budget (from fl-server --metrics-json), when available.
    bool hasServer{false};
    ServerTickReport server{};
};

// Aggregates per-client metrics into a report. `elapsedS` is the steady measurement window;
// `workerDtMs` are the worker-loop iteration times (harness-overrun signal). Mutates the input
// vectors (computeStats sorts) — callers pass throwaway copies.
inline SwarmReport buildReport(const SwarmConfig& cfg, const std::vector<ClientMetrics>& clients, double elapsedS,
                               std::vector<double> workerDtMs, int threadsUsed,
                               std::optional<ServerTickReport> server = std::nullopt) {
    SwarmReport r;
    r.host = cfg.host;
    r.port = cfg.port;
    r.clientsRequested = cfg.clients;
    r.durationS = elapsedS;
    r.rateHz = cfg.rateHz;
    r.pattern = cfg.pattern;
    r.threads = threadsUsed;
    r.assertMinTickHz = cfg.assertMinTickHz;
    r.assertMaxKbs = cfg.assertMaxKbs;
    r.assertMaxTickMs = cfg.assertMaxTickMs;
    if (server) {
        r.hasServer = true;
        r.server = *server;
    }

    std::vector<double> kbs, rtt, connect, tick;
    uint64_t totalSnapshotBytes = 0;
    for (const auto& c : clients) {
        if (c.connected)
            ++r.clientsConnected;
        if (c.disconnectedDuringRun)
            ++r.clientsDisconnected;
        if (!c.connected)
            continue;
        connect.push_back(c.connectMs);
        kbs.push_back(c.downstreamKbs(elapsedS));
        totalSnapshotBytes += c.snapshotBytes;
        if (c.rttValid)
            rtt.push_back(static_cast<double>(c.rttMs));
        const double hz = c.observedTickHz();
        if (hz > 0.0)
            tick.push_back(hz);
        if (c.maxSnapshotGapMs > r.maxSnapshotGapMs)
            r.maxSnapshotGapMs = c.maxSnapshotGapMs;
    }
    r.clientsRefused = r.clientsRequested - r.clientsConnected - r.clientsDisconnected;
    if (r.clientsRefused < 0)
        r.clientsRefused = 0;

    r.connectMs = computeStats(connect);
    r.downstreamKbs = computeStats(kbs);
    r.rttMs = computeStats(rtt);
    r.tickHz = computeStats(tick);
    r.workerLoopDtSamples = static_cast<int>(workerDtMs.size());
    r.workerLoopDtMs = computeStats(workerDtMs);
    if (elapsedS > 0.0)
        r.aggregateDownstreamMbs = static_cast<double>(totalSnapshotBytes) / elapsedS / (1024.0 * 1024.0);

    bool pass = true;
    if (cfg.assertMinTickHz > 0.0 && r.tickHz.min < cfg.assertMinTickHz)
        pass = false;
    if (cfg.assertMaxKbs > 0.0 && r.downstreamKbs.max > cfg.assertMaxKbs)
        pass = false;
    // Authoritative server tick budget gate (#520 hook): fail if the server's p99 tick time
    // exceeds the cap. Missing server data while the assert is enabled is itself a failure
    // (the gate cannot be evaluated -> treat as not-passing rather than silently passing).
    if (cfg.assertMaxTickMs > 0.0 && (!r.hasServer || r.server.total.p99 > cfg.assertMaxTickMs))
        pass = false;
    r.assertsPassed = pass;
    return r;
}

inline void printReport(const SwarmReport& r) {
    std::printf("\n--- bot_swarm results (%s:%u, pattern=%s, %d threads) ---\n", r.host.c_str(), r.port,
                r.pattern.c_str(), r.threads);
    std::printf("clients: requested=%d connected=%d refused=%d disconnected=%d  duration=%.1fs\n", r.clientsRequested,
                r.clientsConnected, r.clientsRefused, r.clientsDisconnected, r.durationS);
    std::printf("observed server tick-Hz: min=%.1f mean=%.1f\n", r.tickHz.min, r.tickHz.mean);
    printStats("dn KB/s/cl", r.downstreamKbs, r.clientsConnected, "");
    printStats("RTT", r.rttMs, static_cast<int>(r.rttMs.max > 0 ? r.clientsConnected : 0), "ms");
    printStats("connect", r.connectMs, r.clientsConnected, "ms");
    printStats("loop dt", r.workerLoopDtMs, r.workerLoopDtSamples, "ms");
    std::printf("aggregate downstream: %.2f MB/s   max snapshot gap: %.1f ms\n", r.aggregateDownstreamMbs,
                r.maxSnapshotGapMs);
    if (r.hasServer)
        std::printf("server tick (authoritative): %.1f Hz  total %.2f/%.2f ms mean/p99  "
                    "(integ %.2f ai %.2f coll %.2f ser %.2f mean)\n",
                    r.server.tickHz, r.server.total.mean, r.server.total.p99,
                    r.server.phases[static_cast<int>(TickPhase::Integrate)].mean,
                    r.server.phases[static_cast<int>(TickPhase::Ai)].mean,
                    r.server.phases[static_cast<int>(TickPhase::Collision)].mean,
                    r.server.phases[static_cast<int>(TickPhase::Serialize)].mean);
    if (r.assertMinTickHz > 0.0 || r.assertMaxKbs > 0.0 || r.assertMaxTickMs > 0.0)
        std::printf("asserts: %s\n", r.assertsPassed ? "PASS" : "FAIL");
    std::printf("---\n");
}

namespace detail {
inline std::string jStat(const char* name, const Stats& s) {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "  \"%s\": { \"min\": %.3f, \"mean\": %.3f, \"max\": %.3f, \"p95\": %.3f, \"p99\": %.3f }", name,
                  s.min, s.mean, s.max, s.p95, s.p99);
    return buf;
}
} // namespace detail

inline std::string reportToJson(const SwarmReport& r) {
    char head[512];
    std::snprintf(head, sizeof(head),
                  "{\n"
                  "  \"schema_version\": %d,\n"
                  "  \"host\": \"%s\", \"port\": %u,\n"
                  "  \"clients_requested\": %d, \"clients_connected\": %d,\n"
                  "  \"clients_refused\": %d, \"clients_disconnected\": %d,\n"
                  "  \"duration_s\": %.3f, \"rate_hz\": %d, \"pattern\": \"%s\", \"threads\": %d,\n"
                  "  \"observed_server_tick_hz\": { \"min\": %.3f, \"mean\": %.3f },\n",
                  kSwarmReportSchemaVersion, r.host.c_str(), r.port, r.clientsRequested, r.clientsConnected,
                  r.clientsRefused, r.clientsDisconnected, r.durationS, r.rateHz, r.pattern.c_str(), r.threads,
                  r.tickHz.min, r.tickHz.mean);
    std::string out = head;
    out += detail::jStat("downstream_kbs_per_client", r.downstreamKbs) + ",\n";
    out += detail::jStat("rtt_ms", r.rttMs) + ",\n";
    out += detail::jStat("connect_ms", r.connectMs) + ",\n";
    out += detail::jStat("worker_loop_dt_ms", r.workerLoopDtMs) + ",\n";
    // Authoritative server-side block (same shape fl-server writes; substr(2) trims the leading
    // pad on toJson's opening brace so it sits after the key on one line).
    if (r.hasServer) {
        const std::string sj = toJson(r.server, 2);
        out += "  \"server_tick\": " + sj.substr(2) + ",\n";
    }
    char tail[512];
    std::snprintf(tail, sizeof(tail),
                  "  \"aggregate_downstream_mbs\": %.3f, \"max_snapshot_gap_ms\": %.3f,\n"
                  "  \"asserts\": { \"min_tick_hz\": %.3f, \"max_kbs\": %.3f, \"max_tick_ms\": %.3f, "
                  "\"passed\": %s }\n"
                  "}\n",
                  r.aggregateDownstreamMbs, r.maxSnapshotGapMs, r.assertMinTickHz, r.assertMaxKbs, r.assertMaxTickMs,
                  r.assertsPassed ? "true" : "false");
    out += tail;
    return out;
}

} // namespace fl
