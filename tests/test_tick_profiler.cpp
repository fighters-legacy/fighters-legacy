// SPDX-License-Identifier: GPL-3.0-or-later
#include "perf/TickProfiler.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>

using namespace fl;
using Catch::Approx;
using namespace std::chrono;

TEST_CASE("TickProfiler aggregates per-phase samples across ticks", "[tickprofiler]") {
    ManualClock clk;
    TickProfiler prof(100);
    prof.setClock(clk);

    for (int i = 0; i < 10; ++i) {
        prof.beginTick();
        prof.addPhaseSample(TickPhase::Integrate, 5.0);
        prof.addPhaseSample(TickPhase::Ai, 2.0);
        clk.advance(milliseconds(16)); // total wall for this tick = 16 ms
        prof.endTick();
    }

    const TickBudget b = prof.snapshot();
    CHECK(b.ticksSampled == 10);
    CHECK(b.ticksTotal == 10);
    CHECK(b.phases[static_cast<int>(TickPhase::Integrate)].mean == Approx(5.0));
    CHECK(b.phases[static_cast<int>(TickPhase::Integrate)].max == Approx(5.0));
    CHECK(b.phases[static_cast<int>(TickPhase::Ai)].mean == Approx(2.0));
    CHECK(b.total.mean == Approx(16.0));
    // other = total - sum(phases) = 16 - 7 = 9.
    CHECK(b.other.mean == Approx(9.0));
    // 10 ticks 16 ms apart -> 9 intervals over 144 ms -> 62.5 Hz.
    CHECK(b.tickHz == Approx(62.5));
}

TEST_CASE("TickProfiler empty snapshot is zeroed with no division by zero", "[tickprofiler]") {
    TickProfiler prof;
    const TickBudget b = prof.snapshot();
    CHECK(b.ticksSampled == 0);
    CHECK(b.ticksTotal == 0);
    CHECK(b.tickHz == 0.0);
    CHECK(b.windowSeconds == 0.0);
    CHECK(b.total.mean == 0.0);
    CHECK(b.phases[static_cast<int>(TickPhase::Serialize)].p99 == 0.0);
}

TEST_CASE("TickProfiler ring buffer wraps to the last N ticks", "[tickprofiler]") {
    ManualClock clk;
    TickProfiler prof(4); // window = 4
    prof.setClock(clk);

    for (int i = 0; i < 10; ++i) {
        prof.beginTick();
        prof.addPhaseSample(TickPhase::Integrate, static_cast<double>(i));
        clk.advance(milliseconds(10));
        prof.endTick();
    }

    const TickBudget b = prof.snapshot();
    CHECK(b.ticksSampled == 4); // window cap
    CHECK(b.ticksTotal == 10);  // monotonic all-time
    // Last 4 integrate values are 6,7,8,9.
    CHECK(b.phases[static_cast<int>(TickPhase::Integrate)].min == Approx(6.0));
    CHECK(b.phases[static_cast<int>(TickPhase::Integrate)].max == Approx(9.0));
    CHECK(b.phases[static_cast<int>(TickPhase::Integrate)].mean == Approx(7.5));
}

TEST_CASE("TickProfiler sums multiple scopes for the same phase within one tick", "[tickprofiler]") {
    ManualClock clk;
    TickProfiler prof;
    prof.setClock(clk);

    prof.beginTick();
    prof.addPhaseSample(TickPhase::Integrate, 3.0);
    prof.addPhaseSample(TickPhase::Integrate, 4.0);
    clk.advance(milliseconds(10));
    prof.endTick();

    const TickBudget b = prof.snapshot();
    CHECK(b.phases[static_cast<int>(TickPhase::Integrate)].mean == Approx(7.0));
}

TEST_CASE("TickProfiler clamps other to zero when phases exceed measured total", "[tickprofiler]") {
    ManualClock clk;
    TickProfiler prof;
    prof.setClock(clk);

    prof.beginTick();
    prof.addPhaseSample(TickPhase::Integrate, 100.0);
    // No clock advance -> measured total wall is 0, less than the phase sum.
    prof.endTick();

    const TickBudget b = prof.snapshot();
    CHECK(b.total.mean == Approx(0.0));
    CHECK(b.other.mean == Approx(0.0));
}

TEST_CASE("TickPhaseScope records wall-time into the current tick", "[tickprofiler]") {
    ManualClock clk;
    TickProfiler prof;
    prof.setClock(clk);

    prof.beginTick();
    {
        TickPhaseScope sc(prof, TickPhase::Collision, clk);
        clk.advance(milliseconds(7));
    }
    clk.advance(milliseconds(3)); // untimed remainder -> 'other'
    prof.endTick();

    const TickBudget b = prof.snapshot();
    CHECK(b.phases[static_cast<int>(TickPhase::Collision)].mean == Approx(7.0));
    CHECK(b.total.mean == Approx(10.0));
    CHECK(b.other.mean == Approx(3.0));
}
