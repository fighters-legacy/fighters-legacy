// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "loop/TimeController.h"
#include "loop/TimeRate.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

class ILogger;
class ISimUpdate;

// Manages the fixed-timestep sim thread and coordinates with the main (render) thread.
//
// Threading model:
//   Main thread   — calls start(), stop(), shellTick(), setRate(). Owns all HAL.
//   Sim thread    — owned by GameLoop; calls ISimUpdate::onTick() at fixed rate.
//                   Must never call any HAL method except ILogger::log().
//
// Shared state between threads:
//   m_running          atomic<bool>     stop signal (release/relaxed)
//   m_lastTickNs       atomic<int64_t>  wall-time of last tick in ns (release/acquire)
//   m_totalTicksSnap   atomic<uint64_t> snapshot of tick count (relaxed)
//   m_pendingRate      guarded by m_rateMutex; sim thread polls m_rateDirty each tick
//   TimeController     touched only by the sim thread after start()
class GameLoop {
  public:
    // tickRate: desired sim ticks per real second at Normal compression (default 60).
    GameLoop(ISimUpdate& sim, ILogger& logger, double tickRate = 60.0);

    // Destructor calls stop() as a safety net; prefer an explicit stop() before
    // any HAL teardown so the sim thread exits while the logger is still alive.
    ~GameLoop();

    GameLoop(const GameLoop&) = delete;
    GameLoop& operator=(const GameLoop&) = delete;
    GameLoop(GameLoop&&) = delete;
    GameLoop& operator=(GameLoop&&) = delete;

    // -----------------------------------------------------------------------
    // Lifecycle — main thread only.
    // -----------------------------------------------------------------------

    // Starts the sim thread. Log: "game loop started".
    void start();

    // Signals the sim thread to stop and joins it. Idempotent.
    // Log: "game loop stopped; total ticks: N".
    void stop();

    // -----------------------------------------------------------------------
    // Per-frame main-thread API.
    // -----------------------------------------------------------------------

    // Call once per rendered frame. Returns render-interpolation alpha in [0.0, 1.0].
    // Lock-free: reads m_lastTickNs with acquire semantics.
    [[nodiscard]] float shellTick() noexcept;

    // -----------------------------------------------------------------------
    // Enqueue a callback to run on the sim thread at the top of the next tick,
    // before ISimUpdate::onTick(). Thread-safe; may be called from any thread.
    void enqueueSimCallback(std::function<void()> fn);

    // -----------------------------------------------------------------------
    // Time compression — main thread only.
    // -----------------------------------------------------------------------

    void setRate(TimeRate rate);
    [[nodiscard]] TimeRate rate() const;

    // Approximate snapshot of total ticks fired (atomic load, relaxed).
    [[nodiscard]] uint64_t totalTicks() const noexcept;

  private:
    void simThreadFunc();

    ISimUpdate& m_sim;
    ILogger& m_logger;
    double m_tickRate;

    std::atomic<bool> m_running{false};
    std::atomic<int64_t> m_lastTickNs{0};
    std::atomic<uint64_t> m_totalTicksSnap{0};

    mutable std::mutex m_rateMutex;
    TimeRate m_pendingRate{TimeRate::Normal};
    bool m_rateDirty{false};

    std::mutex m_callbackMutex;
    std::vector<std::function<void()>> m_pendingCallbacks;

    std::thread m_simThread;
};
