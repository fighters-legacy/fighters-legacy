// SPDX-License-Identifier: GPL-3.0-or-later
#include "loop/TimeController.h"

#include <algorithm>

namespace fl {

TimeController::TimeController(double fixedStep) noexcept : m_fixedStep(fixedStep) {}

int TimeController::advance(TimePoint wallNow) noexcept {
    if (m_firstAdvance) {
        m_lastWallTime = wallNow;
        m_lastTickWallTime = wallNow;
        m_firstAdvance = false;
        return 0;
    }

    using namespace std::chrono;
    double wallDt = duration<double>(wallNow - m_lastWallTime).count();
    m_lastWallTime = wallNow;

    m_accumulator += wallDt * timeRateMultiplier(m_rate);

    int ticks = static_cast<int>(m_accumulator / m_fixedStep);
    m_accumulator -= ticks * m_fixedStep;
    // Guard against floating-point drift below zero.
    if (m_accumulator < 0.0)
        m_accumulator = 0.0;

    return ticks;
}

void TimeController::consumeTick(TimePoint wallNow) noexcept {
    m_lastTickWallTime = wallNow;
    ++m_totalTicks;
}

double TimeController::renderAlpha(TimePoint wallNow) const noexcept {
    if (m_totalTicks == 0)
        return 0.0;

    using namespace std::chrono;
    double elapsed = duration<double>(wallNow - m_lastTickWallTime).count();
    double alpha = elapsed / m_fixedStep;
    return std::clamp(alpha, 0.0, 1.0);
}

void TimeController::setRate(TimeRate rate) noexcept {
    m_rate = rate;
}

TimeRate TimeController::rate() const noexcept {
    return m_rate;
}

double TimeController::fixedStep() const noexcept {
    return m_fixedStep;
}

void TimeController::reset(TimePoint wallNow) noexcept {
    m_accumulator = 0.0;
    m_totalTicks = 0;
    m_lastWallTime = wallNow;
    m_lastTickWallTime = wallNow;
    m_firstAdvance = false;
}

double TimeController::accumulator() const noexcept {
    return m_accumulator;
}

uint64_t TimeController::totalTicks() const noexcept {
    return m_totalTicks;
}

TimeController::TimePoint TimeController::lastTickWallTime() const noexcept {
    return m_lastTickWallTime;
}

} // namespace fl
