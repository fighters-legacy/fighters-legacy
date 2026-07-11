// SPDX-License-Identifier: GPL-3.0-or-later
#include "net/TickGovernor.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>

namespace {
using fl::makeTickGovernorParams;
using fl::TickGovernor;
using fl::TickGovernorParams;

constexpr double kBudgetMs = 1000.0 / 60.0; // ~16.667 ms fixed-step budget at 60 Hz

// Default test params: evaluate every tick so a single update() step takes effect immediately, and a
// near-instant EWMA (alpha 1.0) so the control signal tracks the fed value exactly in the simple cases.
TickGovernorParams baseParams() {
    TickGovernorParams p;
    p.evalIntervalTicks = 1u;
    p.ewmaAlpha = 1.0f;
    return p;
}

// Drive the governor for n ticks at a fixed per-tick wall-time, returning the final loadFactor.
float run(TickGovernor& g, const TickGovernorParams& p, double tickMs, int n, uint64_t startTick = 0) {
    g.configure(p);
    for (int i = 0; i < n; ++i)
        g.update(startTick + static_cast<uint64_t>(i), tickMs, kBudgetMs);
    return g.loadFactor();
}

} // namespace

TEST_CASE("TickGovernor: a healthy under-budget server holds loadFactor at 1", "[tick_governor]") {
    TickGovernor g;
    auto p = baseParams();
    // 4 ms/tick is well under the 0.60 low-watermark (~10 ms) -> always healthy.
    CHECK(run(g, p, 4.0, 30) == Catch::Approx(1.0f));
    CHECK_FALSE(g.degraded());
    CHECK(g.snapshotIntervalTicks() == 1u);
    CHECK(g.aiSampleStride() == 1u);
    CHECK(g.effectiveBudget(1200u) == 1200u);
    CHECK(g.interestScale() == Catch::Approx(1.0f)); // full interest radius when healthy
}

TEST_CASE("TickGovernor: sustained over-budget ticks drive loadFactor to the floor", "[tick_governor]") {
    TickGovernor g;
    auto p = baseParams();
    // 30 ms/tick is well above the 0.90 high-watermark (~15 ms) -> overrun every eval.
    const float lf = run(g, p, 30.0, 200);
    CHECK(g.degraded());
    CHECK(lf == Catch::Approx(p.floor));
    // At the floor the levers bottom out at their configured caps.
    CHECK(g.snapshotIntervalTicks() == p.maxSnapshotIntervalTicks);
    CHECK(g.aiSampleStride() == p.maxAiStride);
    // Interest scale clamps at its own fraction floor (default 0.5 > the 0.25 loadFactor floor).
    CHECK(g.interestScale() == Catch::Approx(p.minInterestFraction));
}

TEST_CASE("TickGovernor: multiplicative decrease then additive recovery", "[tick_governor]") {
    TickGovernor g;
    auto p = baseParams();
    g.configure(p);
    // One overrun eval from full: loadFactor = 1 * decreaseFactor.
    g.update(0, 30.0, kBudgetMs);
    CHECK(g.loadFactor() == Catch::Approx(1.0f * p.decreaseFactor));
    // One healthy eval: additive increase.
    g.update(1, 2.0, kBudgetMs);
    CHECK(g.loadFactor() == Catch::Approx(p.decreaseFactor + p.increaseStep));
}

TEST_CASE("TickGovernor: dead-band between watermarks holds loadFactor", "[tick_governor]") {
    TickGovernor g;
    auto p = baseParams();
    g.configure(p);
    g.update(0, 30.0, kBudgetMs); // shed once -> below 1
    const float held = g.loadFactor();
    // 12 ms is between low (~10 ms) and high (~15 ms) watermarks -> neither shed nor recover.
    for (uint64_t t = 1; t < 10; ++t)
        g.update(t, 12.0, kBudgetMs);
    CHECK(g.loadFactor() == Catch::Approx(held));
}

TEST_CASE("TickGovernor: evalIntervalTicks gates AIMD stepping (hysteresis)", "[tick_governor]") {
    TickGovernor g;
    auto p = baseParams();
    p.evalIntervalTicks = 6u;
    g.configure(p);
    g.update(0, 30.0, kBudgetMs); // first call always steps
    const float afterFirst = g.loadFactor();
    CHECK(afterFirst < 1.0f);
    // Ticks 1..5 are within the eval interval -> no further step.
    for (uint64_t t = 1; t < 6; ++t)
        g.update(t, 30.0, kBudgetMs);
    CHECK(g.loadFactor() == Catch::Approx(afterFirst));
    // Tick 6 crosses the interval -> steps again.
    g.update(6, 30.0, kBudgetMs);
    CHECK(g.loadFactor() < afterFirst);
}

TEST_CASE("TickGovernor: disabled pins all levers to no-op", "[tick_governor]") {
    TickGovernor g;
    auto p = baseParams();
    p.enabled = false;
    run(g, p, 50.0, 50); // hammer with huge over-budget ticks
    CHECK(g.loadFactor() == Catch::Approx(1.0f));
    CHECK_FALSE(g.degraded());
    CHECK(g.snapshotIntervalTicks() == 1u);
    CHECK(g.aiSampleStride() == 1u);
    CHECK(g.effectiveBudget(1200u) == 1200u);
    CHECK(g.interestScale() == Catch::Approx(1.0f));
}

TEST_CASE("TickGovernor: effectiveBudget scaling and floor", "[tick_governor]") {
    TickGovernor g;
    auto p = baseParams();
    run(g, p, 30.0, 200); // drive to floor
    const float lf = g.loadFactor();
    // Unlimited budget (0) always passes through — only the rate lever applies.
    CHECK(g.effectiveBudget(0u) == 0u);
    // A set budget is scaled by loadFactor, clamped up to budgetFloorBytes.
    const uint32_t expected = std::max(p.budgetFloorBytes, static_cast<uint32_t>(std::round(1200.0f * lf)));
    CHECK(g.effectiveBudget(1200u) == expected);
    // A budget already below the floor is never raised.
    CHECK(g.effectiveBudget(100u) == 100u);
}

TEST_CASE("TickGovernor: lever clamps are UBSan-safe at the floor", "[tick_governor]") {
    TickGovernor g;
    auto p = baseParams();
    p.floor = 0.01f; // tiny floor -> 1/loadFactor large, but clamped before the cast
    p.maxSnapshotIntervalTicks = 8u;
    p.maxAiStride = 6u;
    run(g, p, 100.0, 300);
    CHECK(g.snapshotIntervalTicks() == 8u);
    CHECK(g.aiSampleStride() == 6u);
}

TEST_CASE("TickGovernor: interestScale tracks loadFactor and clamps at the fraction floor", "[tick_governor]") {
    TickGovernor g;
    auto p = baseParams();
    p.minInterestFraction = 0.5f;
    g.configure(p);
    // One overrun eval: loadFactor 0.7 is above the fraction floor -> scale tracks it exactly.
    g.update(0, 30.0, kBudgetMs);
    CHECK(g.loadFactor() == Catch::Approx(0.7f));
    CHECK(g.interestScale() == Catch::Approx(0.7f));
    // Drive to the loadFactor floor (0.25): the scale clamps at the 0.5 fraction floor instead.
    run(g, p, 30.0, 200, /*startTick=*/1);
    CHECK(g.loadFactor() == Catch::Approx(p.floor));
    CHECK(g.interestScale() == Catch::Approx(0.5f));
}

TEST_CASE("TickGovernor: minInterestFraction of 1 pins the interest lever off", "[tick_governor]") {
    TickGovernor g;
    auto p = baseParams();
    p.minInterestFraction = 1.0f;
    run(g, p, 30.0, 200);                            // drive to the loadFactor floor
    CHECK(g.degraded());                             // the other levers still shed...
    CHECK(g.interestScale() == Catch::Approx(1.0f)); // ...but the interest radius never shrinks
}

TEST_CASE("TickGovernor: interestScale never drops below a loadFactor floor above the fraction", "[tick_governor]") {
    TickGovernor g;
    auto p = baseParams();
    p.floor = 0.8f;               // loadFactor cannot fall below 0.8...
    p.minInterestFraction = 0.5f; // ...so the scale bottoms out at 0.8, not 0.5
    run(g, p, 30.0, 200);
    CHECK(g.loadFactor() == Catch::Approx(0.8f));
    CHECK(g.interestScale() == Catch::Approx(0.8f));
}

TEST_CASE("TickGovernor: zero or negative budget treated as healthy", "[tick_governor]") {
    TickGovernor g;
    auto p = baseParams();
    g.configure(p);
    for (uint64_t t = 0; t < 20; ++t)
        g.update(t, 100.0, 0.0); // budget 0 -> never overrun
    CHECK(g.loadFactor() == Catch::Approx(1.0f));
}

TEST_CASE("TickGovernor: makeTickGovernorParams maps Hz to floor and interval", "[tick_governor]") {
    // 15 Hz floor at 60 Hz -> floor 0.25, maxSnapshotIntervalTicks 4.
    auto p = makeTickGovernorParams(true, 0.90f, 0.60f, 15.0f, 4u, 400u);
    CHECK(p.enabled);
    CHECK(p.floor == Catch::Approx(0.25f));
    CHECK(p.maxSnapshotIntervalTicks == 4u);
    CHECK(p.maxAiStride == 4u);
    CHECK(p.budgetFloorBytes == 400u);
    // 10 Hz floor -> interval 6.
    auto p2 = makeTickGovernorParams(true, 0.90f, 0.60f, 10.0f, 4u, 400u);
    CHECK(p2.floor == Catch::Approx(1.0f / 6.0f));
    CHECK(p2.maxSnapshotIntervalTicks == 6u);
    // maxAiStride floored at 1.
    auto p3 = makeTickGovernorParams(true, 0.90f, 0.60f, 15.0f, 0u, 400u);
    CHECK(p3.maxAiStride == 1u);
    // minInterestFraction (#726): default 0.5, explicit value passed through, clamped to [0, 1].
    CHECK(p.minInterestFraction == Catch::Approx(0.5f));
    auto p4 = makeTickGovernorParams(true, 0.90f, 0.60f, 15.0f, 4u, 400u, 0.75f);
    CHECK(p4.minInterestFraction == Catch::Approx(0.75f));
    auto p5 = makeTickGovernorParams(true, 0.90f, 0.60f, 15.0f, 4u, 400u, 2.0f);
    CHECK(p5.minInterestFraction == Catch::Approx(1.0f));
}
