// SPDX-License-Identifier: GPL-3.0-or-later
#include "loop/GameLoop.h"
#include "loop/ISimUpdate.h"
#include "loop/TimeController.h"
#include "loop/TimeRate.h"
#include "mock_hal.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>

using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Time-point helpers for deterministic TimeController tests.
//
// Synthetic time points share a single static base so all calls in one test
// process see the same origin. Values are passed as integer nanoseconds to
// avoid duration_cast truncation errors when converting floating-point seconds
// to nanoseconds.
//
// kNs60: nanoseconds per tick at 60 Hz, rounded UP so the value is always
//        strictly greater than 1/60 s (= 16 666 666.7 ns).  Using a value
//        slightly above the fixedStep boundary guarantees advance() returns the
//        correct tick count even with floating-point rounding.
// kNs30: same for 30 Hz.
// ---------------------------------------------------------------------------
static std::chrono::steady_clock::time_point tp(long long nanos) {
    static const auto base = std::chrono::steady_clock::now();
    return base + std::chrono::nanoseconds(nanos);
}

static constexpr long long kNs60 = 16'666'667LL; // > 1/60 s
static constexpr long long kNs30 = 33'333'334LL; // > 1/30 s

// ============================================================================
// Section A: TimeController — deterministic, no threads
// ============================================================================

TEST_CASE("TimeController: default construction", "[tc]") {
    TimeController tc;
    REQUIRE(tc.accumulator() == 0.0);
    REQUIRE(tc.totalTicks() == 0);
    REQUIRE(tc.rate() == TimeRate::Normal);
    REQUIRE(tc.fixedStep() == Catch::Approx(1.0 / 60.0));
}

TEST_CASE("TimeController: advance with same TimePoint returns 0 (frame gating)", "[tc]") {
    TimeController tc;
    auto t = tp(0);
    tc.advance(t);
    REQUIRE(tc.advance(t) == 0);
}

TEST_CASE("TimeController: advance by exactly one fixedStep returns 1", "[tc]") {
    TimeController tc;
    tc.advance(tp(0));
    REQUIRE(tc.advance(tp(kNs60)) == 1);
}

TEST_CASE("TimeController: advance by N fixedSteps returns N ticks", "[tc]") {
    for (int n : {1, 2, 5, 8}) {
        TimeController tc;
        tc.advance(tp(0));
        REQUIRE(tc.advance(tp(n * kNs60)) == n);
    }
}

TEST_CASE("TimeController: accumulator is non-negative after consumeTick", "[tc]") {
    TimeController tc;
    tc.advance(tp(0));
    tc.advance(tp(kNs60));
    tc.consumeTick(tp(kNs60));
    REQUIRE(tc.accumulator() >= 0.0);
}

TEST_CASE("TimeController: totalTicks increments once per consumeTick", "[tc]") {
    TimeController tc;
    tc.advance(tp(0));
    for (int i = 0; i < 3; ++i) {
        tc.advance(tp((i + 1) * kNs60));
        tc.consumeTick(tp((i + 1) * kNs60));
    }
    REQUIRE(tc.totalTicks() == 3);
}

TEST_CASE("TimeController: Normal rate accumulator grows at 1x wall rate", "[tc]") {
    TimeController tc;
    tc.advance(tp(0));
    tc.advance(tp(kNs60 / 2)); // half a tick — no tick fires
    // kNs60/2 = 8 333 333 ns; 0.5/60 s = 8 333 333.3 ns — within 1 ns
    REQUIRE(tc.accumulator() == Catch::Approx(0.5 / 60.0).margin(2e-9));
}

TEST_CASE("TimeController: Paused rate - accumulator stays 0, advance returns 0", "[tc]") {
    TimeController tc;
    tc.setRate(TimeRate::Paused);
    tc.advance(tp(0));
    REQUIRE(tc.advance(tp(1'000'000'000LL)) == 0);
    REQUIRE(tc.accumulator() == 0.0);
}

TEST_CASE("TimeController: Half rate - 1 fixedStep wall time gives 0 ticks", "[tc]") {
    TimeController tc;
    tc.setRate(TimeRate::Half);
    tc.advance(tp(0));
    REQUIRE(tc.advance(tp(kNs60)) == 0);
    REQUIRE(tc.accumulator() == Catch::Approx(0.5 / 60.0).margin(2e-9));
}

TEST_CASE("TimeController: Double rate - 1 fixedStep wall time gives 2 ticks", "[tc]") {
    TimeController tc;
    tc.setRate(TimeRate::Double);
    tc.advance(tp(0));
    REQUIRE(tc.advance(tp(kNs60)) == 2);
}

TEST_CASE("TimeController: Quad rate - 1 fixedStep wall time gives 4 ticks", "[tc]") {
    TimeController tc;
    tc.setRate(TimeRate::Quad);
    tc.advance(tp(0));
    REQUIRE(tc.advance(tp(kNs60)) == 4);
}

TEST_CASE("TimeController: Octa rate - 1 fixedStep wall time gives 8 ticks", "[tc]") {
    TimeController tc;
    tc.setRate(TimeRate::Octa);
    tc.advance(tp(0));
    REQUIRE(tc.advance(tp(kNs60)) == 8);
}

TEST_CASE("TimeController: setRate changes multiplier mid-session", "[tc]") {
    TimeController tc;
    tc.advance(tp(0));
    tc.advance(tp(kNs60)); // 1 tick at Normal; m_lastWallTime = tp(kNs60)
    tc.setRate(TimeRate::Double);
    // wallDt for next advance = kNs60; at 2× that gives 2 ticks
    REQUIRE(tc.advance(tp(2 * kNs60)) == 2);
}

TEST_CASE("TimeController: reset clears accumulator and totalTicks", "[tc]") {
    TimeController tc;
    tc.advance(tp(0));
    tc.advance(tp(5 * kNs60));
    for (int i = 0; i < 5; ++i)
        tc.consumeTick(tp((i + 1) * kNs60));
    tc.reset(tp(10 * kNs60));
    REQUIRE(tc.accumulator() == 0.0);
    REQUIRE(tc.totalTicks() == 0);
}

TEST_CASE("TimeController: renderAlpha is 0.0 immediately after consumeTick", "[tc]") {
    TimeController tc;
    tc.advance(tp(0));
    tc.advance(tp(kNs60));
    tc.consumeTick(tp(kNs60));
    REQUIRE(tc.renderAlpha(tp(kNs60)) == Catch::Approx(0.0).margin(1e-9));
}

TEST_CASE("TimeController: renderAlpha is 0.5 at half fixedStep after last tick", "[tc]") {
    TimeController tc;
    tc.advance(tp(0));
    tc.advance(tp(kNs60));
    tc.consumeTick(tp(kNs60));
    REQUIRE(tc.renderAlpha(tp(kNs60 + kNs60 / 2)) == Catch::Approx(0.5).epsilon(1e-6));
}

TEST_CASE("TimeController: renderAlpha clamps to 1.0", "[tc]") {
    TimeController tc;
    tc.advance(tp(0));
    tc.advance(tp(kNs60));
    tc.consumeTick(tp(kNs60));
    REQUIRE(tc.renderAlpha(tp(kNs60 + 2 * kNs60)) == Catch::Approx(1.0).margin(1e-9));
}

TEST_CASE("TimeController: renderAlpha is 0.0 before first tick", "[tc]") {
    TimeController tc;
    REQUIRE(tc.renderAlpha(std::chrono::steady_clock::now()) == 0.0);
}

TEST_CASE("TimeController: custom fixedStep is honoured", "[tc]") {
    TimeController tc{1.0 / 30.0};
    REQUIRE(tc.fixedStep() == Catch::Approx(1.0 / 30.0));
    tc.advance(tp(0));
    REQUIRE(tc.advance(tp(kNs30)) == 1);
}

TEST_CASE("TimeController: timeRateMultiplier returns correct values", "[tc]") {
    REQUIRE(timeRateMultiplier(TimeRate::Paused) == 0.0);
    REQUIRE(timeRateMultiplier(TimeRate::Half) == 0.5);
    REQUIRE(timeRateMultiplier(TimeRate::Normal) == 1.0);
    REQUIRE(timeRateMultiplier(TimeRate::Double) == 2.0);
    REQUIRE(timeRateMultiplier(TimeRate::Quad) == 4.0);
    REQUIRE(timeRateMultiplier(TimeRate::Octa) == 8.0);
}

// ============================================================================
// Section B: GameLoop lifecycle — short real-time sleeps
// ============================================================================

struct MockSim : ISimUpdate {
    std::atomic<int> tickCount{0};
    void onTick(double /*dt*/, uint64_t) override {
        ++tickCount;
    }
};

TEST_CASE("GameLoop: start/stop without sleep logs started and stopped", "[gl]") {
    MockSim sim;
    MockLogger logger;
    {
        GameLoop gl(sim, logger);
        gl.start();
        gl.stop();
    }
    REQUIRE(logger.hasMessage(LogLevel::Info, "game loop started"));
    REQUIRE(logger.hasMessage(LogLevel::Info, "game loop stopped"));
}

TEST_CASE("GameLoop: onTick fires at least once after 50ms at Normal rate", "[gl]") {
    MockSim sim;
    MockLogger logger;
    GameLoop gl(sim, logger);
    gl.start();
    std::this_thread::sleep_for(200ms); // 200ms gives 12 tick opportunities at 60 Hz; robust on slow CI runners
    gl.stop();
    REQUIRE(sim.tickCount.load() >= 1);
}

TEST_CASE("GameLoop: Paused rate fires no ticks", "[gl]") {
    MockSim sim;
    MockLogger logger;
    GameLoop gl(sim, logger);
    gl.setRate(TimeRate::Paused);
    gl.start();
    std::this_thread::sleep_for(100ms);
    gl.stop();
    REQUIRE(sim.tickCount.load() == 0);
}

TEST_CASE("GameLoop: shellTick returns alpha in [0, 1]", "[gl]") {
    MockSim sim;
    MockLogger logger;
    GameLoop gl(sim, logger);
    gl.start();
    std::this_thread::sleep_for(20ms);
    float alpha = gl.shellTick();
    gl.stop();
    REQUIRE(alpha >= 0.0f);
    REQUIRE(alpha <= 1.0f);
}

TEST_CASE("GameLoop: stop is idempotent", "[gl]") {
    MockSim sim;
    MockLogger logger;
    GameLoop gl(sim, logger);
    gl.start();
    gl.stop();
    REQUIRE_NOTHROW(gl.stop());
}

TEST_CASE("GameLoop: totalTicks matches sim callback count after stop", "[gl]") {
    MockSim sim;
    MockLogger logger;
    GameLoop gl(sim, logger);
    gl.start();
    std::this_thread::sleep_for(50ms);
    gl.stop();
    // After stop() the sim thread is joined — both counts are stable.
    REQUIRE(gl.totalTicks() == static_cast<uint64_t>(sim.tickCount.load()));
}

TEST_CASE("GameLoop: setRate to Paused while running stops new ticks", "[gl]") {
    MockSim sim;
    MockLogger logger;
    GameLoop gl(sim, logger);
    gl.start();
    std::this_thread::sleep_for(50ms);
    gl.setRate(TimeRate::Paused);
    std::this_thread::sleep_for(20ms);
    int countAfterPause = sim.tickCount.load();
    std::this_thread::sleep_for(50ms);
    gl.stop();
    REQUIRE(sim.tickCount.load() == countAfterPause);
}

TEST_CASE("GameLoop: destructor stops sim thread without explicit stop", "[gl]") {
    MockSim sim;
    MockLogger logger;
    {
        GameLoop gl(sim, logger);
        gl.start();
        std::this_thread::sleep_for(20ms);
        // destructor called here — must not invoke std::terminate()
    }
    REQUIRE(logger.hasMessage(LogLevel::Info, "game loop stopped"));
}

TEST_CASE("GameLoop: enqueueSimCallback executes on next tick", "[gl]") {
    MockSim sim;
    MockLogger logger;
    GameLoop gl(sim, logger);
    gl.start();

    std::atomic<bool> fired{false};
    gl.enqueueSimCallback([&fired] { fired.store(true, std::memory_order_release); });

    // Wait up to 500 ms (30 tick opportunities at 60 Hz) — robust against macOS CI scheduler jitter.
    auto deadline = std::chrono::steady_clock::now() + 500ms;
    while (!fired.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(5ms);

    gl.stop();
    REQUIRE(fired.load());
}

TEST_CASE("GameLoop: enqueueSimCallback multiple callbacks all run", "[gl]") {
    MockSim sim;
    MockLogger logger;
    GameLoop gl(sim, logger);
    gl.start();

    std::atomic<int> counter{0};
    for (int i = 0; i < 3; ++i)
        gl.enqueueSimCallback([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });

    auto deadline = std::chrono::steady_clock::now() + 500ms;
    while (counter.load(std::memory_order_relaxed) < 3 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(5ms);

    gl.stop();
    REQUIRE(counter.load() == 3);
}

TEST_CASE("GameLoop: fires approximately 60 ticks per second at Normal rate", "[gl]") {
    MockSim sim;
    MockLogger logger;
    GameLoop gl(sim, logger);
    gl.start();
    std::this_thread::sleep_for(1000ms);
    gl.stop();
    int count = sim.tickCount.load();
    // ±15% tolerance for CI scheduler jitter (Windows timer quantum ≈ 15 ms).
    REQUIRE(count >= 51);
    REQUIRE(count <= 69);
}
