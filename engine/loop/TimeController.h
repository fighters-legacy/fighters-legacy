// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "loop/TimeRate.h"

#include <chrono>
#include <cstdint>

// Pure fixed-timestep timing math. No threads, no HAL.
// All methods may be called from any thread provided the caller handles external locking.
// GameLoop uses TimeController exclusively on the sim thread after start(), so no locking
// is required there.
//
// Accumulator design:
//   accumulator += wallDt * timeRateMultiplier(rate)
//   ticksThisFrame = floor(accumulator / m_fixedStep)
//   accumulator   -= ticksThisFrame * m_fixedStep
//
// simDt (the value passed to ISimUpdate::onTick) is always m_fixedStep — a constant per
// instance. This keeps the physics integrator stable regardless of time compression; the
// compression factor only controls how many ticks fire per real second.
class TimeController {
  public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Duration = Clock::duration;

    static constexpr double kDefaultFixedStep = 1.0 / 60.0;

    explicit TimeController(double fixedStep = kDefaultFixedStep) noexcept;

    // Feed the current wall-clock time. Returns the number of sim ticks to execute
    // this frame (>= 0). On the first call, initialises the internal last-wall-time
    // anchor so the very first wallDt is not astronomically large.
    int advance(TimePoint wallNow) noexcept;

    // Call after dispatching each tick. Decrements accumulator by one fixedStep,
    // stamps lastTickWallTime, and increments totalTicks.
    void consumeTick(TimePoint wallNow) noexcept;

    // Render interpolation alpha in [0.0, 1.0]: how far between the last sim tick
    // and the next tick we currently are. Used by the renderer to interpolate
    // object positions for smooth visuals at uncapped frame rates.
    // Returns 0.0 before the first tick has been consumed.
    [[nodiscard]] double renderAlpha(TimePoint wallNow) const noexcept;

    void setRate(TimeRate rate) noexcept;
    [[nodiscard]] TimeRate rate() const noexcept;
    [[nodiscard]] double fixedStep() const noexcept;

    // Reset to a fresh state anchored at wallNow (zeroes accumulator and tick count).
    void reset(TimePoint wallNow) noexcept;

    // Accessors used by tests and by GameLoop for sleep_until deadline calculation.
    [[nodiscard]] double accumulator() const noexcept;
    [[nodiscard]] uint64_t totalTicks() const noexcept;
    [[nodiscard]] TimePoint lastTickWallTime() const noexcept;

  private:
    double m_fixedStep;
    TimeRate m_rate{TimeRate::Normal};
    double m_accumulator{0.0};
    TimePoint m_lastWallTime{};
    TimePoint m_lastTickWallTime{};
    uint64_t m_totalTicks{0};
    bool m_firstAdvance{true};
};
