// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// NetStats — shared distribution statistics for the networking tools (net_check,
// bot_swarm). Header-only. Computes min/mean/max/p95/p99/stddev over a sample set
// and prints a one-line summary. Extracted from net_check so the load-test harness
// reuses the exact same percentile math.

#include <algorithm>
#include <cmath>
#include <cstdio>
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

// Computes summary statistics. Sorts the input vector in place (nearest-rank
// percentiles). Returns a zeroed Stats for an empty input.
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

inline void printStats(const char* label, const Stats& s, int samples, const char* unit) {
    std::printf("%-10s  samples=%-4d  min=%.2f%s  mean=%.2f%s  max=%.2f%s"
                "  p95=%.2f%s  p99=%.2f%s  stddev=%.2f%s\n",
                label, samples, s.min, unit, s.mean, unit, s.max, unit, s.p95, unit, s.p99, unit, s.stddev, unit);
}

} // namespace fl
