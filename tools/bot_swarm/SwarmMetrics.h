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
// is retained for comparison (extend, don't replace). schema_version = 3 adds "transport"
// (#649) — the backend the swarm ACTUALLY spoke, so a GNS gate can prove it measured GNS
// rather than an enet6 fallback. schema_version = 4 adds "rss_series" (a periodic RSS time
// series) + the slope-based leak assert (#789) — one end-of-run RSS sample cannot tell a
// 128-client working set held flat from a slow leak that stayed under the bound; the trend can.
// schema_version = 5 adds "rss_step_max_kb"/"rss_step_at_s" and makes the tail slope robust to a
// step (#1095) — see rssSlopeKbPerMin below for why a plain least-squares fit is not.

#include "NetStats.h"
#include "SwarmConfig.h"
#include "perf/ServerTickReport.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace fl {

constexpr int kSwarmReportSchemaVersion = 5;

// One periodic RSS reading over the run: wall-seconds from measurement start + server RSS in KB.
struct RssSample {
    double tS{0.0};
    int64_t rssKb{0};
};

// Fraction of the run (measured by time, from the tail) the leak slope is fitted over. The head of
// the run is the connect ramp + working-set fill, where RSS legitimately climbs to its 128-client
// plateau; only the tail is the "still growing?" signal. Default: the final half.
constexpr double kRssSlopeTailFraction = 0.5;

// Buckets the tail is split into for the robust slope below, and the sample density a tail needs
// before bucketing is used at all. A single step can spoil at most ONE bucket, so a median over
// >= 3 survives it; 6 keeps each bucket long enough to average out RSS jitter while leaving a
// comfortable majority of buckets clean.
constexpr int kRssSlopeBuckets = 6;
constexpr int kRssSlopeMinSamplesPerBucket = 4;

// Least-squares RSS growth rate (KB/min) over [tailStart, end] of the series. Returns nullopt when
// that window has fewer than two distinct-time points.
inline std::optional<double> rssOlsKbPerMin(const std::vector<RssSample>& series, double tailStart) {
    double n = 0.0, sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
    for (const auto& s : series) {
        if (s.tS < tailStart)
            continue;
        const double x = s.tS;
        const double y = static_cast<double>(s.rssKb);
        n += 1.0;
        sx += x;
        sy += y;
        sxx += x * x;
        sxy += x * y;
    }
    if (n < 2.0)
        return std::nullopt;
    const double denom = n * sxx - sx * sx;
    if (denom == 0.0)
        return std::nullopt;                                // all tail samples share one timestamp
    const double slopePerSec = (n * sxy - sx * sy) / denom; // KB/s
    return slopePerSec * 60.0;                              // KB/min
}

// RSS growth rate (KB/min) over the final `tailFraction` of the series. Returns nullopt when the
// tail cannot be fitted at all — the caller treats that as "not evaluable", distinct from a genuine
// flat (0 KB/min) fit. Positive = growing.
//
// ⚠ This is deliberately NOT a plain least-squares fit over the tail. A server that plateaus, takes
// ONE discrete allocation (an arena block, a buffer that doubles), then plateaus again is not
// leaking — but a straight line through that step reports the step height smeared over the window as
// though it were a sustained rate. That is not hypothetical: the 2 h soak on #1095 held flat at
// 85.7 MB, stepped +6.1 MB once, held flat at 93.3 MB for the final 29 min (0.4 MB of drift), and
// the old fit called it 190.7 KB/min against a 128 KB/min gate — a fabricated leak. Theil-Sen does
// not help either: with the step near mid-window the straddling pairs are the MAJORITY, so the
// median lands on one of them.
//
// So: split the tail into `kRssSlopeBuckets` equal time buckets, take each bucket's AVERAGE rate
// from its boundary samples, and report the MEDIAN of those. One step lands wholly inside one
// bucket and the median ignores it, while a genuine leak — or a RECURRING staircase, which IS a
// leak — moves every bucket and is reported at its true rate.
//
// The rate is deliberately taken from the bucket's endpoints rather than a per-bucket least-squares
// fit. A within-bucket fit aliases: a staircase whose period matches the bucket width and whose
// steps land on the boundaries is FLAT inside every bucket, so a median of fitted slopes calls a
// real 205 KB/min leak 0.0 KB/min. Endpoint deltas cannot lose a step — whatever happens between
// two boundaries is charged to that bucket. Checked against the real #1095 series plus synthetic
// flat / one-step / two-step / sustained / staircase (aligned, offset and half-period) / jittered
// cases: endpoint-median is correct on all of them, fitted-median is not.
//
// Short runs whose tail cannot give every bucket `kRssSlopeMinSamplesPerBucket` samples fall back to
// the plain whole-tail fit, which is what they have always done.
inline std::optional<double> rssSlopeKbPerMin(const std::vector<RssSample>& series, double tailFraction) {
    if (series.size() < 2)
        return std::nullopt;
    const double firstT = series.front().tS;
    const double lastT = series.back().tS;
    if (lastT <= firstT)
        return std::nullopt;
    const double tailStart = lastT - (lastT - firstT) * tailFraction;

    std::size_t tailCount = 0;
    for (const auto& s : series)
        if (s.tS >= tailStart)
            ++tailCount;
    if (tailCount < static_cast<std::size_t>(kRssSlopeBuckets) * kRssSlopeMinSamplesPerBucket)
        return rssOlsKbPerMin(series, tailStart);

    const double bucketS = (lastT - tailStart) / kRssSlopeBuckets;
    if (!(bucketS > 0.0))
        return rssOlsKbPerMin(series, tailStart);

    // The tail's first sample is the opening boundary; each bucket then closes on its last sample.
    const RssSample* prev = nullptr;
    for (const auto& s : series) {
        if (s.tS >= tailStart) {
            prev = &s;
            break;
        }
    }
    if (prev == nullptr)
        return rssOlsKbPerMin(series, tailStart);

    std::vector<double> rates;
    rates.reserve(kRssSlopeBuckets);
    for (int b = 0; b < kRssSlopeBuckets; ++b) {
        const double lo = tailStart + bucketS * b;
        // The last bucket takes the final sample; earlier ones are half-open so none is counted twice.
        const double hi = (b == kRssSlopeBuckets - 1) ? lastT : lo + bucketS;

        const RssSample* last = nullptr;
        for (const auto& s : series) {
            if (s.tS < lo)
                continue;
            if (b == kRssSlopeBuckets - 1 ? s.tS > hi : s.tS >= hi)
                break;
            last = &s;
        }
        if (last == nullptr || last->tS <= prev->tS)
            continue; // empty bucket — its growth is charged to the next one that closes
        rates.push_back(static_cast<double>(last->rssKb - prev->rssKb) / ((last->tS - prev->tS) / 60.0));
        prev = last;
    }

    // Too few closed buckets to take a meaningful median — fall back rather than invent a number.
    if (rates.size() < 3)
        return rssOlsKbPerMin(series, tailStart);

    std::sort(rates.begin(), rates.end());
    const std::size_t mid = rates.size() / 2;
    return (rates.size() % 2 == 1) ? rates[mid] : (rates[mid - 1] + rates[mid]) / 2.0;
}

// The largest single-sample RSS JUMP within the same tail window, and when it happened. The robust
// slope above deliberately ignores an isolated step; reporting it here keeps it visible instead of
// silently eaten, so a 6 MB one-off is still something a human sees and can go attribute.
struct RssStep {
    int64_t deltaKb{0};
    double atS{0.0};
};

inline std::optional<RssStep> rssMaxStep(const std::vector<RssSample>& series, double tailFraction) {
    if (series.size() < 2)
        return std::nullopt;
    const double firstT = series.front().tS;
    const double lastT = series.back().tS;
    if (lastT <= firstT)
        return std::nullopt;
    const double tailStart = lastT - (lastT - firstT) * tailFraction;

    std::optional<RssStep> best;
    for (std::size_t i = 1; i < series.size(); ++i) {
        if (series[i].tS < tailStart)
            continue;
        const int64_t delta = series[i].rssKb - series[i - 1].rssKb;
        if (delta > 0 && (!best || delta > best->deltaKb))
            best = RssStep{delta, series[i].tS};
    }
    return best;
}

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
    std::string transport; // backend actually spoken ("enet"/"gns") — #649
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
    int assertMinEntities{0};
    int64_t assertMaxRssGrowthKb{0};
    double assertMaxRssSlopeKbPerMin{0.0};   // 0 = disabled (#789)
    double assertMaxLoadFactor{-1.0};        // <0 = disabled (#574)
    int64_t assertMaxDroppedTicks{-1};       // <0 = disabled (#574)
    double assertCongestionEngagedHz{0.0};   // 0 = disabled (#714)
    double assertCongestionRecoveredHz{0.0}; // 0 = disabled (#714)

    // Periodic RSS trend (#789): the sampled series and the fitted tail slope (KB/min). The slope is
    // present only when the tail had enough points to fit; nullopt = not evaluable (too few samples).
    std::vector<RssSample> rssSeries;
    std::optional<double> rssSlopeKbPerMin;
    // The largest single-sample jump in the same tail window (#1095). The slope ignores an isolated
    // step by design, so this is what keeps it visible; nullopt = no upward step in the tail.
    std::optional<RssStep> rssMaxStep;

    // Authoritative server-side tick budget (from fl-server --metrics-json), when available.
    bool hasServer{false};
    ServerTickReport server{};
};

// Aggregates per-client metrics into a report. `elapsedS` is the steady measurement window;
// `workerDtMs` are the worker-loop iteration times (harness-overrun signal). Mutates the input
// vectors (computeStats sorts) — callers pass throwaway copies.
inline SwarmReport buildReport(const SwarmConfig& cfg, const std::vector<ClientMetrics>& clients, double elapsedS,
                               std::vector<double> workerDtMs, int threadsUsed,
                               std::optional<ServerTickReport> server = std::nullopt,
                               std::vector<RssSample> rssSeries = {}) {
    SwarmReport r;
    r.host = cfg.host;
    r.port = cfg.port;
    r.clientsRequested = cfg.clients;
    r.durationS = elapsedS;
    r.rateHz = cfg.rateHz;
    // Normalize the alias so the report (and the gate's cross-check) always reads "enet" or "gns".
    r.transport = (cfg.transport == "enet6") ? "enet" : cfg.transport;
    // The report carries the mix spec when a weighted mix is used, else the single pattern label.
    r.pattern = cfg.patternMix.empty() ? cfg.pattern : cfg.patternMix;
    r.threads = threadsUsed;
    r.assertMinTickHz = cfg.assertMinTickHz;
    r.assertMaxKbs = cfg.assertMaxKbs;
    r.assertMaxTickMs = cfg.assertMaxTickMs;
    r.assertMinEntities = cfg.assertMinEntities;
    r.assertMaxRssGrowthKb = cfg.assertMaxRssGrowthKb;
    r.assertMaxRssSlopeKbPerMin = cfg.assertMaxRssSlopeKbPerMin;
    r.assertMaxLoadFactor = cfg.assertMaxLoadFactor;
    r.assertMaxDroppedTicks = cfg.assertMaxDroppedTicks;
    r.assertCongestionEngagedHz = cfg.assertCongestionEngagedHz;
    r.assertCongestionRecoveredHz = cfg.assertCongestionRecoveredHz;
    if (server) {
        r.hasServer = true;
        r.server = *server;
    }
    r.rssSeries = std::move(rssSeries);
    r.rssSlopeKbPerMin = rssSlopeKbPerMin(r.rssSeries, kRssSlopeTailFraction);
    r.rssMaxStep = rssMaxStep(r.rssSeries, kRssSlopeTailFraction);

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
    // Entity-scale gate (#573 hook): confirm the server actually reached the requested live entity
    // count (e.g. the [world] test_spawn_ai_count load-spawn took). Missing server data while the
    // assert is enabled is a failure, like assert-max-tick-ms.
    if (cfg.assertMinEntities > 0 && (!r.hasServer || r.server.entities < static_cast<uint32_t>(cfg.assertMinEntities)))
        pass = false;
    // Soak leak gate (#707 hook): fail if the server's RSS grew more than the cap over the run.
    // Missing server data while the assert is enabled is a failure, like the other server gates.
    if (cfg.assertMaxRssGrowthKb > 0 &&
        (!r.hasServer || (static_cast<int64_t>(r.server.rssKb) - static_cast<int64_t>(r.server.rssStartupKb)) >
                             cfg.assertMaxRssGrowthKb))
        pass = false;
    // Soak leak TREND gate (#789 hook): fit RSS over the tail of the run and fail if the sustained
    // growth rate exceeds the cap. A flat tail is the real "no leak" signal — the endpoint bound
    // above stays as the coarse backstop. Not evaluable (too few samples) while the assert is enabled
    // is a failure, like the other server gates: the gate could not be checked, so do not pass it.
    if (cfg.assertMaxRssSlopeKbPerMin > 0.0 &&
        (!r.rssSlopeKbPerMin.has_value() || *r.rssSlopeKbPerMin > cfg.assertMaxRssSlopeKbPerMin))
        pass = false;
    // Overrun-governor gate (#574 hook). load_factor: the governor shed under load iff it dropped below
    // the threshold (< 1) — so this fails when the governor never engaged (load_factor stays 1.0).
    // dropped_ticks: the graceful-not-spiral property (the governor should keep GameLoop drops at ~0).
    // Both use a negative-disabled sentinel; missing server data while enabled is a failure, as with the
    // other server gates (the gate cannot be evaluated -> not-passing rather than silently passing).
    if (cfg.assertMaxLoadFactor >= 0.0 && (!r.hasServer || r.server.loadFactor > cfg.assertMaxLoadFactor))
        pass = false;
    if (cfg.assertMaxDroppedTicks >= 0 &&
        (!r.hasServer || static_cast<int64_t>(r.server.droppedTicks) > cfg.assertMaxDroppedTicks))
        pass = false;
    // Congestion-controller gate (#714 hook), over the run-long server watermarks (schema v5).
    // engaged: the controller must have throttled the slowest peer's send rate to <= the threshold
    // at some point (congestion_min_send_hz stuck at 60 = it never responded to the degraded link).
    // recovered: after the min was set, the send rate must have climbed back to >= the threshold
    // (proves recovery once the link cleared). Missing server data while enabled = failure.
    if (cfg.assertCongestionEngagedHz > 0.0 &&
        (!r.hasServer || r.server.congestionMinSendHz > cfg.assertCongestionEngagedHz))
        pass = false;
    if (cfg.assertCongestionRecoveredHz > 0.0 &&
        (!r.hasServer || r.server.congestionRecoveredSendHz < cfg.assertCongestionRecoveredHz))
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
    if (r.hasServer && (r.assertCongestionEngagedHz > 0.0 || r.assertCongestionRecoveredHz > 0.0))
        std::printf("congestion: min send %.1f Hz, recovered to %.1f Hz, max loss %.3f\n", r.server.congestionMinSendHz,
                    r.server.congestionRecoveredSendHz, r.server.congestionMaxLoss);
    if (!r.rssSeries.empty()) {
        std::printf("RSS series: %zu samples, tail slope %s KB/min (median of %d buckets over the final %.0f%%)\n",
                    r.rssSeries.size(), r.rssSlopeKbPerMin ? std::to_string(*r.rssSlopeKbPerMin).c_str() : "n/a",
                    kRssSlopeBuckets, kRssSlopeTailFraction * 100.0);
        if (r.rssMaxStep)
            std::printf("RSS largest tail step: %+.1f MB at t=%.0fs (excluded from the slope by design)\n",
                        static_cast<double>(r.rssMaxStep->deltaKb) / 1024.0, r.rssMaxStep->atS);
    }
    if (r.assertMinTickHz > 0.0 || r.assertMaxKbs > 0.0 || r.assertMaxTickMs > 0.0 || r.assertMinEntities > 0 ||
        r.assertMaxRssGrowthKb > 0 || r.assertMaxRssSlopeKbPerMin > 0.0 || r.assertMaxLoadFactor >= 0.0 ||
        r.assertMaxDroppedTicks >= 0 || r.assertCongestionEngagedHz > 0.0 || r.assertCongestionRecoveredHz > 0.0)
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
    char head[640];
    std::snprintf(head, sizeof(head),
                  "{\n"
                  "  \"schema_version\": %d,\n"
                  "  \"host\": \"%s\", \"port\": %u,\n"
                  "  \"clients_requested\": %d, \"clients_connected\": %d,\n"
                  "  \"clients_refused\": %d, \"clients_disconnected\": %d,\n"
                  "  \"duration_s\": %.3f, \"rate_hz\": %d, \"pattern\": \"%s\", \"threads\": %d,\n"
                  "  \"transport\": \"%s\",\n"
                  "  \"observed_server_tick_hz\": { \"min\": %.3f, \"mean\": %.3f },\n",
                  kSwarmReportSchemaVersion, r.host.c_str(), r.port, r.clientsRequested, r.clientsConnected,
                  r.clientsRefused, r.clientsDisconnected, r.durationS, r.rateHz, r.pattern.c_str(), r.threads,
                  r.transport.c_str(), r.tickHz.min, r.tickHz.mean);
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
    // RSS trend (#789): the sampled series + the fitted tail slope, so a soak can plot growth and
    // gate on the trend, not just the endpoint.
    if (!r.rssSeries.empty()) {
        out += "  \"rss_series\": [";
        for (std::size_t i = 0; i < r.rssSeries.size(); ++i) {
            char b[96];
            std::snprintf(b, sizeof(b), "%s{ \"t_s\": %.1f, \"rss_kb\": %lld }", i ? ", " : "", r.rssSeries[i].tS,
                          static_cast<long long>(r.rssSeries[i].rssKb));
            out += b;
        }
        out += "],\n";
        char sb[128];
        if (r.rssSlopeKbPerMin)
            std::snprintf(sb, sizeof(sb), "  \"rss_slope_kb_per_min\": %.3f,\n", *r.rssSlopeKbPerMin);
        else
            std::snprintf(sb, sizeof(sb), "  \"rss_slope_kb_per_min\": null,\n");
        out += sb;
        // The step the slope deliberately ignores (#1095) — reported so it is never silently eaten.
        if (r.rssMaxStep)
            std::snprintf(sb, sizeof(sb), "  \"rss_step_max_kb\": %lld, \"rss_step_at_s\": %.1f,\n",
                          static_cast<long long>(r.rssMaxStep->deltaKb), r.rssMaxStep->atS);
        else
            std::snprintf(sb, sizeof(sb), "  \"rss_step_max_kb\": null, \"rss_step_at_s\": null,\n");
        out += sb;
    }
    char tail[1024];
    std::snprintf(tail, sizeof(tail),
                  "  \"aggregate_downstream_mbs\": %.3f, \"max_snapshot_gap_ms\": %.3f,\n"
                  "  \"asserts\": { \"min_tick_hz\": %.3f, \"max_kbs\": %.3f, \"max_tick_ms\": %.3f, "
                  "\"min_entities\": %d, \"max_rss_growth_kb\": %lld, \"max_rss_slope_kb_per_min\": %.3f, "
                  "\"max_load_factor\": %.3f, "
                  "\"max_dropped_ticks\": %lld, \"congestion_engaged_hz\": %.3f, "
                  "\"congestion_recovered_hz\": %.3f, \"passed\": %s }\n"
                  "}\n",
                  r.aggregateDownstreamMbs, r.maxSnapshotGapMs, r.assertMinTickHz, r.assertMaxKbs, r.assertMaxTickMs,
                  r.assertMinEntities, static_cast<long long>(r.assertMaxRssGrowthKb), r.assertMaxRssSlopeKbPerMin,
                  r.assertMaxLoadFactor, static_cast<long long>(r.assertMaxDroppedTicks), r.assertCongestionEngagedHz,
                  r.assertCongestionRecoveredHz, r.assertsPassed ? "true" : "false");
    out += tail;
    return out;
}

} // namespace fl
