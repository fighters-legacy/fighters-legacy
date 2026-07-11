// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// SwarmConfig — bot_swarm CLI parsing + validated configuration. Pure logic (no I/O, no
// sockets) so it unit-tests directly. Mirrors net_check's positional host/port + flags;
// env fallback (FL_HOST/FL_PORT) is applied by the caller using `hostSet`/`portSet`.

#include "IFlightPattern.h"
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

namespace fl {

struct SwarmConfig {
    std::string host{"127.0.0.1"};
    uint16_t port{4778};
    int clients{32};
    int durationS{30};
    int rateHz{60};
    int rampMs{20};
    int threads{0};                  // 0 = auto (min(hw_concurrency, ceil(clients/32)))
    std::string pattern{"weave"};    // built-in name or "trace:<file>"; ignored when patternMix is set
    std::string patternMix;          // weighted mix spec e.g. "weave:80,aggressive:20"; empty = single pattern
    std::string jsonPath;            // empty = no JSON output
    std::string serverMetricsPath;   // empty = no server-side tick block; fl-server --metrics-json file
    double assertMinTickHz{0.0};     // 0 = disabled
    double assertMaxKbs{0.0};        // 0 = disabled
    double assertMaxTickMs{0.0};     // 0 = disabled; fails if server tick_ms.p99 > this (#520 gate hook)
    int assertMinEntities{0};        // 0 = disabled; fails if server_tick.entities < this (#573 gate hook)
    int64_t assertMaxRssGrowthKb{0}; // 0 = disabled; fails if server_tick.rss_kb - rss_startup_kb > this (#707)
    // Overrun-governor gate (#574). Both use a NEGATIVE-disabled sentinel because 0 is a real value
    // for each (load_factor 0 = fully shed; dropped_ticks 0 = no drops, the healthy target).
    double assertMaxLoadFactor{-1.0}; // <0 = disabled; fails if server_tick.load_factor > this (governor engaged?)
    int64_t assertMaxDroppedTicks{
        -1}; // <0 = disabled; fails if server_tick.dropped_ticks > this (graceful, not spiral)
    // Lossy-proxy link degradation (#714): clients connect through a local UDP relay that drops
    // degradeLoss of datagrams and adds degradeDelayMs one-way delay inside the window
    // [degradeStartS, degradeStartS + degradeDurationS) measured from proxy start (i.e. before the
    // connect ramp). degradeDurationS 0 = proxy disabled entirely (clients connect direct).
    double degradeStartS{10.0};
    double degradeDurationS{0.0};
    double degradeLoss{0.0}; // drop fraction while degraded [0, 1]
    int degradeDelayMs{0};   // added one-way delay while degraded
    // Congestion-controller gate (#714); 0 = disabled (a 0 Hz threshold is meaningless for both, so
    // no negative sentinel is needed here, unlike the #574 governor asserts).
    double assertCongestionEngagedHz{0.0};   // fails if server_tick.congestion_min_send_hz > this
    double assertCongestionRecoveredHz{0.0}; // fails if server_tick.congestion_recovered_send_hz < this
};

enum class ParseStatus { Ok, Help, Version, Error };

struct SwarmParseResult {
    ParseStatus status{ParseStatus::Ok};
    SwarmConfig cfg;
    std::string error;
    bool hostSet{false};
    bool portSet{false};
};

namespace detail {

inline bool needValue(int i, int argc, const char* flag, SwarmParseResult& r) {
    if (i + 1 >= argc) {
        r.status = ParseStatus::Error;
        r.error = std::string("missing value for ") + flag;
        return false;
    }
    return true;
}

} // namespace detail

// Parses argv into a SwarmConfig. Does not read the environment or touch I/O.
inline SwarmParseResult parseSwarmArgs(int argc, char** argv) {
    SwarmParseResult r;
    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-h") == 0) {
            r.status = ParseStatus::Help;
            return r;
        }
        if (std::strcmp(a, "--version") == 0 || std::strcmp(a, "-v") == 0) {
            r.status = ParseStatus::Version;
            return r;
        }
        if (std::strcmp(a, "--clients") == 0) {
            if (!detail::needValue(i, argc, a, r))
                return r;
            r.cfg.clients = std::atoi(argv[++i]);
        } else if (std::strcmp(a, "--duration") == 0) {
            if (!detail::needValue(i, argc, a, r))
                return r;
            r.cfg.durationS = std::atoi(argv[++i]);
        } else if (std::strcmp(a, "--rate") == 0) {
            if (!detail::needValue(i, argc, a, r))
                return r;
            r.cfg.rateHz = std::atoi(argv[++i]);
        } else if (std::strcmp(a, "--ramp-ms") == 0) {
            if (!detail::needValue(i, argc, a, r))
                return r;
            r.cfg.rampMs = std::atoi(argv[++i]);
        } else if (std::strcmp(a, "--threads") == 0) {
            if (!detail::needValue(i, argc, a, r))
                return r;
            r.cfg.threads = std::atoi(argv[++i]);
        } else if (std::strcmp(a, "--pattern") == 0) {
            if (!detail::needValue(i, argc, a, r))
                return r;
            r.cfg.pattern = argv[++i];
        } else if (std::strcmp(a, "--pattern-mix") == 0) {
            if (!detail::needValue(i, argc, a, r))
                return r;
            r.cfg.patternMix = argv[++i];
        } else if (std::strcmp(a, "--json") == 0) {
            if (!detail::needValue(i, argc, a, r))
                return r;
            r.cfg.jsonPath = argv[++i];
        } else if (std::strcmp(a, "--server-metrics") == 0) {
            if (!detail::needValue(i, argc, a, r))
                return r;
            r.cfg.serverMetricsPath = argv[++i];
        } else if (std::strcmp(a, "--assert-min-tick-hz") == 0) {
            if (!detail::needValue(i, argc, a, r))
                return r;
            r.cfg.assertMinTickHz = std::strtod(argv[++i], nullptr);
        } else if (std::strcmp(a, "--assert-max-kbs") == 0) {
            if (!detail::needValue(i, argc, a, r))
                return r;
            r.cfg.assertMaxKbs = std::strtod(argv[++i], nullptr);
        } else if (std::strcmp(a, "--assert-max-tick-ms") == 0) {
            if (!detail::needValue(i, argc, a, r))
                return r;
            r.cfg.assertMaxTickMs = std::strtod(argv[++i], nullptr);
        } else if (std::strcmp(a, "--assert-min-entities") == 0) {
            if (!detail::needValue(i, argc, a, r))
                return r;
            r.cfg.assertMinEntities = std::atoi(argv[++i]);
        } else if (std::strcmp(a, "--assert-max-rss-growth-kb") == 0) {
            if (!detail::needValue(i, argc, a, r))
                return r;
            r.cfg.assertMaxRssGrowthKb = std::strtoll(argv[++i], nullptr, 10);
        } else if (std::strcmp(a, "--assert-max-load-factor") == 0) {
            if (!detail::needValue(i, argc, a, r))
                return r;
            r.cfg.assertMaxLoadFactor = std::strtod(argv[++i], nullptr);
        } else if (std::strcmp(a, "--assert-max-dropped-ticks") == 0) {
            if (!detail::needValue(i, argc, a, r))
                return r;
            r.cfg.assertMaxDroppedTicks = std::strtoll(argv[++i], nullptr, 10);
        } else if (std::strcmp(a, "--degrade-start") == 0) {
            if (!detail::needValue(i, argc, a, r))
                return r;
            r.cfg.degradeStartS = std::strtod(argv[++i], nullptr);
        } else if (std::strcmp(a, "--degrade-duration") == 0) {
            if (!detail::needValue(i, argc, a, r))
                return r;
            r.cfg.degradeDurationS = std::strtod(argv[++i], nullptr);
        } else if (std::strcmp(a, "--degrade-loss") == 0) {
            if (!detail::needValue(i, argc, a, r))
                return r;
            r.cfg.degradeLoss = std::strtod(argv[++i], nullptr);
        } else if (std::strcmp(a, "--degrade-delay-ms") == 0) {
            if (!detail::needValue(i, argc, a, r))
                return r;
            r.cfg.degradeDelayMs = std::atoi(argv[++i]);
        } else if (std::strcmp(a, "--assert-congestion-engaged-hz") == 0) {
            if (!detail::needValue(i, argc, a, r))
                return r;
            r.cfg.assertCongestionEngagedHz = std::strtod(argv[++i], nullptr);
        } else if (std::strcmp(a, "--assert-congestion-recovered-hz") == 0) {
            if (!detail::needValue(i, argc, a, r))
                return r;
            r.cfg.assertCongestionRecoveredHz = std::strtod(argv[++i], nullptr);
        } else if (a[0] == '-' && a[1] != '\0') {
            r.status = ParseStatus::Error;
            r.error = std::string("unknown flag: ") + a;
            return r;
        } else if (positional == 0) {
            r.cfg.host = a;
            r.hostSet = true;
            ++positional;
        } else if (positional == 1) {
            r.cfg.port = static_cast<uint16_t>(std::atoi(a));
            r.portSet = true;
            ++positional;
        } else {
            r.status = ParseStatus::Error;
            r.error = std::string("unexpected argument: ") + a;
            return r;
        }
    }

    // ---- Validation ----
    auto fail = [&r](std::string msg) {
        r.status = ParseStatus::Error;
        r.error = std::move(msg);
    };
    if (r.cfg.clients < 1)
        fail("--clients must be >= 1");
    else if (r.cfg.durationS < 1)
        fail("--duration must be >= 1");
    else if (r.cfg.rateHz < 1 || r.cfg.rateHz > 1000)
        fail("--rate must be in [1, 1000]");
    else if (r.cfg.rampMs < 0)
        fail("--ramp-ms must be >= 0");
    else if (r.cfg.threads < 0)
        fail("--threads must be >= 0");
    else if (!r.cfg.patternMix.empty()) {
        // --pattern-mix supersedes --pattern; validate the mix spec (names + weights) up front.
        std::vector<PatternMixEntry> mix;
        std::string mixErr;
        if (!parsePatternMix(r.cfg.patternMix, mix, mixErr))
            fail("--pattern-mix: " + mixErr);
    } else if (!isKnownPattern(r.cfg.pattern) && !isTracePattern(r.cfg.pattern))
        fail("--pattern must be one of: weave, level, aggressive, idle, random, or trace:<file>");
    else if (r.cfg.assertMinTickHz < 0.0)
        fail("--assert-min-tick-hz must be >= 0");
    else if (r.cfg.assertMaxKbs < 0.0)
        fail("--assert-max-kbs must be >= 0");
    else if (r.cfg.assertMaxTickMs < 0.0)
        fail("--assert-max-tick-ms must be >= 0");
    else if (r.cfg.assertMinEntities < 0)
        fail("--assert-min-entities must be >= 0");
    else if (r.cfg.assertMaxRssGrowthKb < 0)
        fail("--assert-max-rss-growth-kb must be >= 0");
    else if (r.cfg.degradeStartS < 0.0)
        fail("--degrade-start must be >= 0");
    else if (r.cfg.degradeDurationS < 0.0)
        fail("--degrade-duration must be >= 0");
    else if (r.cfg.degradeLoss < 0.0 || r.cfg.degradeLoss > 1.0)
        fail("--degrade-loss must be in [0, 1]");
    else if (r.cfg.degradeDelayMs < 0 || r.cfg.degradeDelayMs > 10000)
        fail("--degrade-delay-ms must be in [0, 10000]");
    else if (r.cfg.degradeDurationS > 0.0 && r.cfg.degradeLoss == 0.0 && r.cfg.degradeDelayMs == 0)
        fail("--degrade-duration needs --degrade-loss and/or --degrade-delay-ms");
    else if (r.cfg.assertCongestionEngagedHz < 0.0 || r.cfg.assertCongestionEngagedHz > 60.0)
        fail("--assert-congestion-engaged-hz must be in [0, 60]");
    else if (r.cfg.assertCongestionRecoveredHz < 0.0 || r.cfg.assertCongestionRecoveredHz > 60.0)
        fail("--assert-congestion-recovered-hz must be in [0, 60]");
    return r;
}

} // namespace fl
