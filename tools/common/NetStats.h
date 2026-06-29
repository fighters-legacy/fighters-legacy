// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// NetStats — one-line distribution summary printer for the networking tools (net_check,
// bot_swarm). Header-only. The `Stats` type + `computeStats` percentile math now live in
// engine/perf/Stats.h (promoted so the server tick profiler shares one implementation);
// this header re-exports them via the include below and keeps the tools-only printer.

#include "perf/Stats.h"

#include <cstdio>

namespace fl {

inline void printStats(const char* label, const Stats& s, int samples, const char* unit) {
    std::printf("%-10s  samples=%-4d  min=%.2f%s  mean=%.2f%s  max=%.2f%s"
                "  p95=%.2f%s  p99=%.2f%s  stddev=%.2f%s\n",
                label, samples, s.min, unit, s.mean, unit, s.max, unit, s.p95, unit, s.p99, unit, s.stddev, unit);
}

} // namespace fl
