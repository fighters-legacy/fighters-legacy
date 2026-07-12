// SPDX-License-Identifier: GPL-3.0-or-later
#include "perf/ServerTickReport.h"
#include "perf/TickProfiler.h"
#include <net/WireRateSampler.h>

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

TEST_CASE("TickProfiler lastTotalMs reflects the most recent tick", "[tickprofiler]") {
    ManualClock clk;
    TickProfiler prof(100);
    prof.setClock(clk);

    // Zero before the first tick.
    CHECK(prof.lastTotalMs() == 0.0);

    prof.beginTick();
    clk.advance(milliseconds(20));
    prof.endTick();
    CHECK(prof.lastTotalMs() == Approx(20.0));

    prof.beginTick();
    clk.advance(milliseconds(5));
    prof.endTick();
    CHECK(prof.lastTotalMs() == Approx(5.0)); // overwritten by the latest tick, not accumulated
}

// ---- Wire-byte accounting (#772) ----------------------------------------------------------------

TEST_CASE("WireRateSampler primes on the first call and returns rates thereafter", "[wire][net]") {
    fl::WireRateSampler s;
    // First call has no interval to divide by: report zero rather than invent a spike.
    const fl::WireStats first = s.sample(1000u, 500u, 10u, 5u, 100.0);
    CHECK(first.outBytesPerSec == Catch::Approx(0.0));
    CHECK(first.outPacketsPerSec == Catch::Approx(0.0));

    // 2000 bytes and 20 packets more, 2 s later.
    const fl::WireStats r = s.sample(3000u, 1500u, 30u, 15u, 102.0);
    CHECK(r.outBytesPerSec == Catch::Approx(1000.0));
    CHECK(r.inBytesPerSec == Catch::Approx(500.0));
    CHECK(r.outPacketsPerSec == Catch::Approx(10.0));
    CHECK(r.inPacketsPerSec == Catch::Approx(5.0));
}

TEST_CASE("WireRateSampler is exact across a uint32 counter wrap (#772)", "[wire][net]") {
    // ENet's totals are uint32 and wrap ("user should reset to 0 as needed" - enet.h). At 128
    // clients they wrap roughly every 8 minutes, so a soak run wraps repeatedly; unsigned
    // subtraction must carry us across it or the rate reads as a huge negative-turned-positive spike.
    fl::WireRateSampler s;
    const uint32_t nearMax = 0xFFFFFFFFu - 100u;
    s.sample(nearMax, nearMax, 0u, 0u, 10.0);
    // 300 bytes later the counter has wrapped past zero.
    const uint32_t wrapped = nearMax + 300u; // wraps to 199
    const fl::WireStats r = s.sample(wrapped, wrapped, 0u, 0u, 11.0);
    CHECK(r.outBytesPerSec == Catch::Approx(300.0));
    CHECK(r.inBytesPerSec == Catch::Approx(300.0));
}

TEST_CASE("WireRateSampler does not divide by a non-advancing clock", "[wire][net]") {
    fl::WireRateSampler s;
    s.sample(0u, 0u, 0u, 0u, 5.0);
    const fl::WireStats r1 = s.sample(1000u, 0u, 0u, 0u, 6.0);
    CHECK(r1.outBytesPerSec == Catch::Approx(1000.0));
    // Same timestamp: repeat the last rate rather than divide by zero.
    const fl::WireStats r2 = s.sample(2000u, 0u, 0u, 0u, 6.0);
    CHECK(r2.outBytesPerSec == Catch::Approx(1000.0));
}

TEST_CASE("ServerTickReport carries wire traffic and divides by the sampled peer count (#772)", "[wire][servertick]") {
    fl::TickBudget b;
    // 800 KB/s egress produced by 16 peers -> 50 KB/s/client, regardless of who is connected NOW.
    fl::ServerTickReport r = fl::makeServerTickReport(b, /*peers=*/0, /*entities=*/0, 1.0, 0, 0, 0, 1.0, 60.0, 60.0,
                                                      0.0, /*wireOutKbs=*/800.0, /*wireInKbs=*/100.0,
                                                      /*wireOutPacketsPerSec=*/960.0, /*wirePeers=*/16);
    CHECK(r.schemaVersion == 6);
    // `peers` is 0 (the load clients have drained away by the time the file is written) but the wire
    // figure must still divide by the 16 peers that PRODUCED the traffic, not by 0.
    CHECK(r.wireOutKbsPerClient() == Catch::Approx(50.0));

    const std::string json = fl::toJson(r);
    CHECK(json.find("\"wire_out_kbs\": 800.0000") != std::string::npos);
    CHECK(json.find("\"wire_peers\": 16") != std::string::npos);
    CHECK(json.find("\"wire_out_kbs_per_client\": 50.0000") != std::string::npos);

    fl::ServerTickReport rt;
    REQUIRE(fl::fromJson(json, rt));
    CHECK(rt.wireOutKbs == Catch::Approx(800.0));
    CHECK(rt.wirePeers == 16);
    CHECK(rt.wireOutKbsPerClient() == Catch::Approx(50.0));
}
