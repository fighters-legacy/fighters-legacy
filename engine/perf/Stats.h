// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Stats — distribution summary statistics shared across the engine and the networking tools.
// Header-only. Computes min/mean/max/p95/p99/stddev over a sample set. Promoted out of
// tools/common/NetStats.h so the server tick profiler (engine/perf/TickProfiler.h) and the
// load-test tools (net_check, bot_swarm via NetStats.h) use one percentile implementation.
//
// Layering: lives in engine/ because both server/ and tools/ depend on engine/. engine/ must
// not depend on tools/, so the canonical copy moved here.

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

namespace fl {

struct Stats {
    double min{0};
    double mean{0};
    double max{0};
    double p95{0};
    double p99{0};
    double stddev{0};
};

// Computes summary statistics. Sorts the input vector in place (nearest-rank percentiles).
// Returns a zeroed Stats for an empty input.
inline Stats computeStats(std::vector<double>& v) {
    if (v.empty())
        return {};
    std::sort(v.begin(), v.end());
    Stats s;
    s.min = v.front();
    s.max = v.back();
    s.mean = std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size());
    auto idx95 = static_cast<std::size_t>(0.95 * static_cast<double>(v.size() - 1));
    auto idx99 = static_cast<std::size_t>(0.99 * static_cast<double>(v.size() - 1));
    s.p95 = v[idx95];
    s.p99 = v[idx99];
    double var = 0.0;
    for (double x : v)
        var += (x - s.mean) * (x - s.mean);
    s.stddev = std::sqrt(var / static_cast<double>(v.size()));
    return s;
}

} // namespace fl
