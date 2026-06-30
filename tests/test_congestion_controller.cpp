// SPDX-License-Identifier: GPL-3.0-or-later
#include "net/CongestionController.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>

namespace {
using fl::CongestionController;
using fl::CongestionParams;
using fl::CongestionSample;

// Default test params: 10 Hz floor at 60 Hz sim (throttleFloor 1/6, maxIntervalTicks 6), every-tick
// eval cadence so a single update() step takes effect immediately in the simple cases.
CongestionParams baseParams() {
    CongestionParams p;
    p.evalIntervalTicks = 1u;
    return p;
}

CongestionSample healthy() {
    return CongestionSample{}; // zero loss / rtt / backlog
}

CongestionSample lossy() {
    CongestionSample s;
    s.packetLoss = 0.5f; // well above the 0.02 threshold
    return s;
}

// Drive `ticks` evals of the same sample, one tick apart.
void drive(CongestionController& c, const CongestionSample& s, uint64_t ticks, uint64_t startTick = 1) {
    for (uint64_t t = 0; t < ticks; ++t)
        c.update(startTick + t, s);
}
} // namespace

TEST_CASE("CongestionController: healthy peer stays at full rate and full budget", "[congestion]") {
    CongestionController c;
    c.configure(baseParams());
    drive(c, healthy(), 20);
    CHECK(c.throttle() == Catch::Approx(1.0f));
    CHECK(c.sendIntervalTicks() == 1u);
    CHECK(c.effectiveBudget(1200u) == 1200u);
}

TEST_CASE("CongestionController: increase clamps at throttle 1.0", "[congestion]") {
    CongestionController c;
    c.configure(baseParams());
    drive(c, healthy(), 100); // many ramp-up steps must not push throttle past 1
    CHECK(c.throttle() == Catch::Approx(1.0f));
}

TEST_CASE("CongestionController: sustained loss backs off to the floor", "[congestion]") {
    CongestionParams p = baseParams();
    CongestionController c;
    c.configure(p);
    drive(c, lossy(), 60); // many back-off steps
    CHECK(c.throttle() == Catch::Approx(p.throttleFloor));
    CHECK(c.sendIntervalTicks() == p.maxIntervalTicks); // 6 => 10 Hz
    CHECK(c.effectiveBudget(1200u) == p.budgetFloorBytes);
}

TEST_CASE("CongestionController: RTT gradient triggers congestion without loss", "[congestion]") {
    CongestionParams p = baseParams();
    CongestionController c;
    c.configure(p);
    // Seed the baseline low, then sustain RTT well above baseline + delayMargin with zero loss.
    CongestionSample lowRtt;
    lowRtt.rttMs = 10u;
    c.update(1, lowRtt); // seeds baseline at 10
    CongestionSample highRtt;
    highRtt.rttMs = 10u + p.delayMarginMs + 20u; // gradient exceeds the margin
    drive(c, highRtt, 30, /*startTick=*/2);
    CHECK(c.throttle() < 1.0f);
    CHECK(c.sendIntervalTicks() > 1u);
}

TEST_CASE("CongestionController: reliable backlog triggers congestion", "[congestion]") {
    CongestionParams p = baseParams();
    CongestionController c;
    c.configure(p);
    CongestionSample backlog;
    backlog.reliableBytesInFlight = p.backlogThresholdBytes + 1u;
    drive(c, backlog, 30);
    CHECK(c.throttle() < 1.0f);
}

TEST_CASE("CongestionController: interval mapping is monotone in throttle", "[congestion]") {
    CongestionParams p = baseParams();
    CongestionController c;
    c.configure(p);
    // Fresh controller (throttle 1.0) maps to interval 1.
    CHECK(c.sendIntervalTicks() == 1u);
    // One loss step => throttle 0.7 => ceil(1/0.7) = 2.
    c.update(1, lossy());
    CHECK(c.throttle() == Catch::Approx(0.7f));
    CHECK(c.sendIntervalTicks() == 2u);
}

TEST_CASE("CongestionController: budget scaling, floor clamp, and unlimited passthrough", "[congestion]") {
    CongestionParams p = baseParams();
    p.budgetFloorBytes = 400u;
    CongestionController c;
    c.configure(p);
    // At full throttle the static budget is unchanged; unlimited (0) stays unlimited.
    CHECK(c.effectiveBudget(1200u) == 1200u);
    CHECK(c.effectiveBudget(0u) == 0u);
    // Back off to the floor; a small static budget clamps to budgetFloorBytes, 0 still passes through.
    drive(c, lossy(), 60);
    CHECK(c.effectiveBudget(1000u) == p.budgetFloorBytes); // 1000*1/6 ≈ 167 < 400 => floor
    CHECK(c.effectiveBudget(0u) == 0u);
}

TEST_CASE("CongestionController: hysteresis holds throttle between eval boundaries", "[congestion]") {
    CongestionParams p = baseParams();
    p.evalIntervalTicks = 10u; // step only every 10 ticks
    CongestionController c;
    c.configure(p);
    c.update(100, lossy()); // first call always steps => throttle 0.7
    const float after_first = c.throttle();
    CHECK(after_first == Catch::Approx(0.7f));
    // Ticks 101..109 are within the eval window: throttle must not change.
    for (uint64_t t = 101; t < 110; ++t)
        c.update(t, lossy());
    CHECK(c.throttle() == Catch::Approx(after_first));
    // Tick 110 crosses the boundary => another back-off step.
    c.update(110, lossy());
    CHECK(c.throttle() < after_first);
}

TEST_CASE("CongestionController: recovers to full rate after congestion clears", "[congestion]") {
    CongestionController c;
    c.configure(baseParams());
    drive(c, lossy(), 60); // collapse to the floor
    CHECK(c.sendIntervalTicks() > 1u);
    drive(c, healthy(), 200, /*startTick=*/100); // sustained healthy
    CHECK(c.throttle() == Catch::Approx(1.0f));
    CHECK(c.sendIntervalTicks() == 1u);
}

TEST_CASE("CongestionController: disabled is a no-op regardless of samples", "[congestion]") {
    CongestionParams p = baseParams();
    p.enabled = false;
    CongestionController c;
    c.configure(p);
    drive(c, lossy(), 60);
    CHECK(c.throttle() == Catch::Approx(1.0f));
    CHECK(c.sendIntervalTicks() == 1u);
    CHECK(c.effectiveBudget(1200u) == 1200u);
    CHECK(c.effectiveBudget(0u) == 0u);
}

TEST_CASE("CongestionController: makeCongestionParams maps min send Hz to interval/floor", "[congestion]") {
    // 10 Hz floor at 60 Hz sim => throttleFloor 1/6, maxIntervalTicks 6.
    const CongestionParams p = fl::makeCongestionParams(/*enabled=*/true, /*minSendHz=*/10.f,
                                                        /*lossThreshold=*/0.05f, /*budgetFloorBytes=*/500u);
    CHECK(p.enabled);
    CHECK(p.throttleFloor == Catch::Approx(10.f / 60.f));
    CHECK(p.maxIntervalTicks == 6u);
    CHECK(p.lossThreshold == Catch::Approx(0.05f));
    CHECK(p.budgetFloorBytes == 500u);
    // 30 Hz floor => interval 2.
    CHECK(fl::makeCongestionParams(true, 30.f, 0.02f, 400u).maxIntervalTicks == 2u);
}
