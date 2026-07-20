// SPDX-License-Identifier: GPL-3.0-or-later
#include "loop/GameLoop.h"

#include "ILogger.h"
#include "loop/ISimUpdate.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

static_assert(std::atomic<int64_t>::is_always_lock_free, "int64_t atomic must be lock-free on this platform");
static_assert(std::atomic<uint64_t>::is_always_lock_free, "uint64_t atomic must be lock-free on this platform");

namespace fl {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using ns = std::chrono::nanoseconds;

int clampCatchupTicks(int rawTicks, int maxCatchup, uint64_t& dropped) noexcept {
    if (maxCatchup < 1)
        maxCatchup = 1;
    if (rawTicks > maxCatchup) {
        dropped = static_cast<uint64_t>(rawTicks - maxCatchup);
        return maxCatchup;
    }
    dropped = 0;
    return rawTicks;
}

GameLoop::GameLoop(ISimUpdate& sim, ILogger& logger, double tickRate, int maxCatchupTicks)
    : m_sim(sim), m_logger(logger), m_tickRate(tickRate), m_maxCatchupTicks(std::clamp(maxCatchupTicks, 1, 64)) {}

GameLoop::~GameLoop() {
    stop();
}

void GameLoop::start() {
    if (m_running.load(std::memory_order_relaxed))
        return;

    m_running.store(true, std::memory_order_release);
    m_simThread = std::thread(&GameLoop::simThreadFunc, this);
    m_logger.log(LogLevel::Info, __FILE__, __LINE__, "game loop started");
}

void GameLoop::stop() {
    if (!m_running.load(std::memory_order_relaxed))
        return;

    m_running.store(false, std::memory_order_release);
    if (m_simThread.joinable())
        m_simThread.join();

    char buf[64];
    std::snprintf(buf, sizeof(buf), "game loop stopped; total ticks: %llu",
                  static_cast<unsigned long long>(m_totalTicksSnap.load(std::memory_order_relaxed)));
    m_logger.log(LogLevel::Info, __FILE__, __LINE__, buf);
}

float GameLoop::shellTick() noexcept {
    int64_t lastNs = m_lastTickNs.load(std::memory_order_acquire);
    if (lastNs == 0)
        return 0.0f;

    auto lastTp = TimePoint{ns{lastNs}};
    auto now = Clock::now();

    double fixedStep = m_tickRate > 0.0 ? 1.0 / m_tickRate : TimeController::kDefaultFixedStep;
    double elapsed = std::chrono::duration<double>(now - lastTp).count();
    double alpha = elapsed / fixedStep;
    return static_cast<float>(std::clamp(alpha, 0.0, 1.0));
}

void GameLoop::setRate(TimeRate rate) {
    std::lock_guard<std::mutex> lk(m_rateMutex);
    m_pendingRate = rate;
    m_rateDirty = true;
}

TimeRate GameLoop::rate() const {
    std::lock_guard<std::mutex> lk(m_rateMutex);
    return m_pendingRate;
}

uint64_t GameLoop::totalTicks() const noexcept {
    return m_totalTicksSnap.load(std::memory_order_relaxed);
}

uint64_t GameLoop::totalDroppedTicks() const noexcept {
    return m_droppedTicks.load(std::memory_order_relaxed);
}

void GameLoop::enqueueSimCallback(std::function<void()> fn) {
    std::lock_guard<std::mutex> lk(m_callbackMutex);
    m_pendingCallbacks.push_back(std::move(fn));
}

void GameLoop::drainSimCallbacks() {
    std::vector<std::function<void()>> callbacks;
    {
        std::lock_guard<std::mutex> lk(m_callbackMutex);
        callbacks.swap(m_pendingCallbacks);
    }
    for (auto& fn : callbacks)
        fn();
}

void GameLoop::simThreadFunc() {
    TimeController tc{1.0 / m_tickRate};

    auto fixedStepDur = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(tc.fixedStep()));

    while (m_running.load(std::memory_order_relaxed)) {
        // Apply any pending rate change from the main thread.
        {
            std::lock_guard<std::mutex> lk(m_rateMutex);
            if (m_rateDirty) {
                tc.setRate(m_pendingRate);
                m_rateDirty = false;
            }
        }

        auto now = Clock::now();
        int ticks = tc.advance(now);

        if (ticks == 0) {
            // Frame gating: no sim time has elapsed — sleep until the next tick deadline.
            std::this_thread::sleep_until(tc.lastTickWallTime() + fixedStepDur);
            continue;
        }

        // Spiral-of-death backstop: drain at most m_maxCatchupTicks ticks per iteration; discard the
        // excess (the accumulator was already fully drained by advance(), so the sim time skips forward
        // rather than spiralling). Record the drop count for overrun observability (#514).
        uint64_t droppedThisIter = 0;
        ticks = clampCatchupTicks(ticks, m_maxCatchupTicks, droppedThisIter);
        if (droppedThisIter > 0)
            m_droppedTicks.fetch_add(droppedThisIter, std::memory_order_relaxed);

        // Drain one-shot callbacks queued by external threads (e.g. debug console).
        drainSimCallbacks();

        for (int i = 0; i < ticks; ++i) {
            auto tickNow = Clock::now();
            tc.consumeTick(tickNow);

            int64_t tickNs = std::chrono::duration_cast<ns>(tickNow.time_since_epoch()).count();
            m_lastTickNs.store(tickNs, std::memory_order_release);
            m_totalTicksSnap.fetch_add(1, std::memory_order_relaxed);

            m_sim.onTick(tc.fixedStep(), tc.totalTicks());
        }
    }
}

} // namespace fl
