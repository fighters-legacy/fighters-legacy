// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// TickProfiler — per-phase wall-time instrumentation for the authoritative server tick.
//
// WorldBroadcaster::onTick decomposes into phases (maintenance / integrate / ai / collision /
// serialize). The profiler accumulates each phase's wall-time per tick and keeps a rolling
// ring of the last kWindow ticks, then computes min/mean/p95/p99/max (and an actual tick-Hz
// from the ring's wall span) on demand. This is the measurement foundation for Epic A
// (which phase to parallelise) and Epic G (Prometheus export reuses ServerTickReport).
//
// Threading: beginTick()/endTick()/addPhaseSample() and the TickPhaseScope RAII helper run on
// the sim thread only. endTick() (rolls the per-tick accumulators into the rings) and
// snapshot() (reader: admin command + ~1 Hz file writer) take the same mutex — one lock/tick.
// The per-tick scratch accumulators are sim-thread-only and lock-free.

#include "IClock.h"
#include "Stats.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <vector>

namespace fl {

enum class TickPhase : int { Maintenance = 0, Integrate, Ai, Collision, Serialize, Count };

inline constexpr int kTickPhaseCount = static_cast<int>(TickPhase::Count);

inline const char* tickPhaseName(TickPhase p) {
    switch (p) {
    case TickPhase::Maintenance:
        return "maintenance";
    case TickPhase::Integrate:
        return "integrate";
    case TickPhase::Ai:
        return "ai";
    case TickPhase::Collision:
        return "collision";
    case TickPhase::Serialize:
        return "serialize";
    case TickPhase::Count:
        break;
    }
    return "?";
}

// Read-only snapshot of the rolling window. All durations in milliseconds.
struct TickBudget {
    std::array<Stats, kTickPhaseCount> phases{}; // indexed by TickPhase
    Stats total{};
    Stats other{};            // total - sum(phases) per tick (untimed remainder); >= 0
    uint64_t ticksSampled{0}; // ticks represented in the window (<= kWindow)
    uint64_t ticksTotal{0};   // monotonic all-time tick count
    double windowSeconds{0.0};
    double tickHz{0.0}; // actual recent tick rate from the ring's wall span
};

class TickProfiler {
  public:
    // Default window ~60 s at 60 Hz. Kept small enough to bound memory (~6 * 3600 doubles).
    explicit TickProfiler(std::size_t window = 3600) : m_window(window == 0 ? 1 : window) {
        m_endTimes.assign(m_window, {});
        for (auto& r : m_rings)
            r.reserve(m_window);
        m_totalRing.reserve(m_window);
        m_otherRing.reserve(m_window);
    }

    void setClock(const IClock& clock) noexcept {
        m_clock = &clock;
    }

    // --- sim-thread per-tick API ---

    void beginTick() noexcept {
        m_cur.fill(0.0);
        m_tickStart = m_clock->now();
    }

    // Add a measured duration (ms) to the current tick's accumulator for `phase`.
    void addPhaseSample(TickPhase phase, double ms) noexcept {
        m_cur[static_cast<int>(phase)] += ms;
    }

    void endTick() {
        const auto now = m_clock->now();
        const double totalMs = std::chrono::duration<double, std::milli>(now - m_tickStart).count();

        std::lock_guard<std::mutex> lk(m_mutex);
        for (int i = 0; i < kTickPhaseCount; ++i)
            push(m_rings[i], m_cur[i]);
        push(m_totalRing, totalMs);

        double sum = 0.0;
        for (int i = 0; i < kTickPhaseCount; ++i)
            sum += m_cur[i];
        push(m_otherRing, totalMs > sum ? totalMs - sum : 0.0);

        m_endTimes[m_head] = now;
        m_head = (m_head + 1) % m_window;
        if (m_count < m_window)
            ++m_count;
        ++m_ticksTotal;
    }

    // --- reader-thread API (any thread) ---

    TickBudget snapshot() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        TickBudget b;
        b.ticksTotal = m_ticksTotal;
        b.ticksSampled = m_count;
        for (int i = 0; i < kTickPhaseCount; ++i) {
            std::vector<double> v = m_rings[i];
            b.phases[i] = computeStats(v);
        }
        {
            std::vector<double> v = m_totalRing;
            b.total = computeStats(v);
        }
        {
            std::vector<double> v = m_otherRing;
            b.other = computeStats(v);
        }
        // Actual recent tick rate = (samples - 1) / wall span across the ring.
        if (m_count >= 2) {
            const std::size_t oldest = (m_head + m_window - m_count) % m_window;
            const std::size_t newest = (m_head + m_window - 1) % m_window;
            const double span = std::chrono::duration<double>(m_endTimes[newest] - m_endTimes[oldest]).count();
            b.windowSeconds = span;
            if (span > 0.0)
                b.tickHz = static_cast<double>(m_count - 1) / span;
        }
        return b;
    }

  private:
    void push(std::vector<double>& ring, double v) {
        if (ring.size() < m_window)
            ring.push_back(v);
        else
            ring[m_head] = v;
    }

    const IClock* m_clock{&SystemClock::instance()};
    std::size_t m_window;

    // Sim-thread scratch for the in-progress tick (no lock).
    std::array<double, kTickPhaseCount> m_cur{};
    std::chrono::steady_clock::time_point m_tickStart{};

    // Rolling rings, guarded by m_mutex.
    mutable std::mutex m_mutex;
    std::array<std::vector<double>, kTickPhaseCount> m_rings{};
    std::vector<double> m_totalRing;
    std::vector<double> m_otherRing;
    std::vector<std::chrono::steady_clock::time_point> m_endTimes; // sized to m_window in ctor
    std::size_t m_head{0};
    std::size_t m_count{0};
    uint64_t m_ticksTotal{0};
};

// RAII phase timer: records wall-time between construction and destruction into the profiler's
// current-tick accumulator for `phase`. Sim-thread only.
class TickPhaseScope {
  public:
    TickPhaseScope(TickProfiler& prof, TickPhase phase, const IClock& clock) noexcept
        : m_prof(prof), m_phase(phase), m_clock(clock), m_start(clock.now()) {}
    ~TickPhaseScope() {
        const double ms = std::chrono::duration<double, std::milli>(m_clock.now() - m_start).count();
        m_prof.addPhaseSample(m_phase, ms);
    }
    TickPhaseScope(const TickPhaseScope&) = delete;
    TickPhaseScope& operator=(const TickPhaseScope&) = delete;

  private:
    TickProfiler& m_prof;
    TickPhase m_phase;
    const IClock& m_clock;
    std::chrono::steady_clock::time_point m_start;
};

} // namespace fl
