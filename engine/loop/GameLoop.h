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
// Drives an ORDERED LIST of ISimUpdate systems (#1078): registration order IS execution order, so the
// tick order is data the loop holds rather than the body of a lambda somewhere else. It took exactly
// one system before, which is why the server hand-sequenced its five sim systems inside a single
// setMissionTickHook lambda across three different step signatures -- and why AlertSystem implemented
// ISimUpdate while carrying a comment explaining that it was not registered with the loop it
// implements the interface for.
//
// Threading model:
//   Main thread   — calls start(), stop(), shellTick(), setRate(), addSimUpdate(). Owns all HAL.
//   Sim thread    — owned by GameLoop; calls ISimUpdate::onTick() on each registered system, in order,
//                   at fixed rate. Must never call any HAL method except ILogger::log().
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
    // The first system registered. Additional ones go through addSimUpdate() in the order they must
    // run; a loop with one system is the common case (the client) and reads as it always did.
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

    // Register a system to run after the ones already registered. Main thread, BEFORE start() —
    // asserted, because the sim thread iterates the list without a lock, and because a system added
    // mid-run would begin at an arbitrary point in a tick.
    void addSimUpdate(ISimUpdate& sim);

    [[nodiscard]] std::size_t simUpdateCount() const noexcept {
        return m_systems.size();
    }

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
    // once at the top of each iteration; a harness that drives ticks itself (e.g. the deterministic
    // --mission-report loop) must call it before each stepOnce() so admin-dispatched trigger effects
    // (detonate / atc_scramble / spawn) actually run instead of piling up unexecuted.
    void drainSimCallbacks();

    // Advance every registered system, in order, on the CALLING thread. This is the body of one sim
    // tick with the timing removed, for a harness that owns its own tick loop — the deterministic
    // --mission-report run and the replay/determinism gate.
    //
    // It exists so there is exactly ONE definition of tick order (#1078). A harness that walked the
    // systems itself would be a second copy of that order, and a determinism harness whose tick order
    // can differ from production's is measuring the wrong thing. Do not call while the loop is running.
    void stepOnce(double simDt, uint64_t tickIndex);

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
    // The ordered walk, shared by the sim thread and stepOnce() so there is one definition of it.
    void stepSystems(double simDt, uint64_t tickIndex);

    // Ordered, and frozen by start(): the sim thread walks it every tick without a lock.
    std::vector<ISimUpdate*> m_systems;
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
