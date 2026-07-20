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

namespace fl {

class ILogger;
class ISimUpdate;

// Caps the per-iteration catch-up tick count (the spiral-of-death backstop) and reports how many ticks
// were shed. Pure so it is unit-testable without spinning the sim thread. Returns min(rawTicks,
// maxCatchup) (maxCatchup floored at 1) and sets `dropped` to the number of ticks discarded (>= 0).
[[nodiscard]] int clampCatchupTicks(int rawTicks, int maxCatchup, uint64_t& dropped) noexcept;

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
    // maxCatchupTicks: max sim ticks drained per loop iteration — the spiral-of-death backstop. When a
    // single iteration falls more than this many ticks behind (e.g. a CPU spike under 128-player load),
    // the excess is discarded (sim time dilates) rather than spiralling; the count is exposed via
    // totalDroppedTicks(). Range [1, 64]; default 8.
    GameLoop(ISimUpdate& sim, ILogger& logger, double tickRate = 60.0, int maxCatchupTicks = 8);

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

    // Run and clear all queued sim callbacks now, on the calling thread. The sim thread calls this
    // once at the top of each iteration; a harness that steps ISimUpdate::onTick() directly (e.g. the
    // deterministic --mission-report loop) must call it before each onTick() so admin-dispatched
    // trigger effects (detonate / atc_scramble / spawn) actually run instead of piling up unexecuted.
    void drainSimCallbacks();

    // -----------------------------------------------------------------------
    // Time compression — main thread only.
    // -----------------------------------------------------------------------

    void setRate(TimeRate rate);
    [[nodiscard]] TimeRate rate() const;

    // Approximate snapshot of total ticks fired (atomic load, relaxed).
    [[nodiscard]] uint64_t totalTicks() const noexcept;

    // All-time count of sim ticks discarded by the catch-up cap (sim overrun / time dilation). 0 on a
    // healthy server; a rising value means the sim cannot keep up even after the governor sheds work.
    // Atomic load (relaxed); safe from any thread. Surfaced by fl-server's metrics loop + --metrics-json.
    [[nodiscard]] uint64_t totalDroppedTicks() const noexcept;

  private:
    void simThreadFunc();

    ISimUpdate& m_sim;
    ILogger& m_logger;
    double m_tickRate;
    int m_maxCatchupTicks;

    std::atomic<bool> m_running{false};
    std::atomic<int64_t> m_lastTickNs{0};
    std::atomic<uint64_t> m_totalTicksSnap{0};
    std::atomic<uint64_t> m_droppedTicks{0};

    mutable std::mutex m_rateMutex;
    TimeRate m_pendingRate{TimeRate::Normal};
    bool m_rateDirty{false};

    std::mutex m_callbackMutex;
    std::vector<std::function<void()>> m_pendingCallbacks;

    std::thread m_simThread;
};

} // namespace fl
