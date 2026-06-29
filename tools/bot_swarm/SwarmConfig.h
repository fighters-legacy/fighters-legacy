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
    int threads{0}; // 0 = auto (min(hw_concurrency, ceil(clients/32)))
    std::string pattern{"weave"};
    std::string jsonPath;          // empty = no JSON output
    std::string serverMetricsPath; // empty = no server-side tick block; fl-server --metrics-json file
    double assertMinTickHz{0.0};   // 0 = disabled
    double assertMaxKbs{0.0};      // 0 = disabled
    double assertMaxTickMs{0.0};   // 0 = disabled; fails if server tick_ms.p99 > this (#520 gate hook)
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
    else if (!isKnownPattern(r.cfg.pattern))
        fail("--pattern must be one of: weave, level, aggressive, idle, random");
    else if (r.cfg.assertMinTickHz < 0.0)
        fail("--assert-min-tick-hz must be >= 0");
    else if (r.cfg.assertMaxKbs < 0.0)
        fail("--assert-max-kbs must be >= 0");
    else if (r.cfg.assertMaxTickMs < 0.0)
        fail("--assert-max-tick-ms must be >= 0");
    return r;
}

} // namespace fl
