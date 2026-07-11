// SPDX-License-Identifier: GPL-3.0-or-later
#include "TickGovernor.h"

#include <algorithm>
#include <cmath>

namespace fl {

void TickGovernor::configure(const TickGovernorParams& params) noexcept {
    m_params = params;
}

void TickGovernor::update(uint64_t tick, double tickMs, double budgetMs) noexcept {
    if (!m_params.enabled) {
        m_loadFactor = 1.f; // pinned to full rate + full budget + every-tick AI while disabled
        return;
    }

    // Smooth the measured per-tick wall-time. The EWMA updates every tick so the control signal tracks
    // sustained load, not a single GC/scheduler spike. alpha ~0.1 => ~10-tick reaction window.
    const float ms = static_cast<float>(tickMs);
    if (!m_ewmaSeeded) {
        m_ewmaMs = ms;
        m_ewmaSeeded = true;
    } else {
        m_ewmaMs += m_params.ewmaAlpha * (ms - m_ewmaMs);
    }

    // AIMD cadence gate (hysteresis): only step every evalIntervalTicks. The first call always steps.
    const uint32_t evalInterval = m_params.evalIntervalTicks == 0u ? 1u : m_params.evalIntervalTicks;
    if (m_haveEval && tick - m_lastEvalTick < evalInterval)
        return;
    m_haveEval = true;
    m_lastEvalTick = tick;

    const float budget = static_cast<float>(budgetMs);
    const bool overrun = budget > 0.f && m_ewmaMs > budget * m_params.highWatermark;
    const bool healthy = budget <= 0.f || m_ewmaMs < budget * m_params.lowWatermark;

    if (overrun) {
        m_loadFactor = std::max(m_params.floor, m_loadFactor * m_params.decreaseFactor);
    } else if (healthy) {
        m_loadFactor = std::min(1.f, m_loadFactor + m_params.increaseStep);
    }
    // Between high and low watermarks: hold (dead-band).
}

uint32_t TickGovernor::snapshotIntervalTicks() const noexcept {
    if (!m_params.enabled)
        return 1u;
    const uint32_t maxInterval = m_params.maxSnapshotIntervalTicks == 0u ? 1u : m_params.maxSnapshotIntervalTicks;
    // loadFactor is clamped to [floor, 1] with floor > 0, so 1/loadFactor is finite; clamp the float
    // result to [1, maxInterval] BEFORE the cast so no inf/NaN ever reaches an int (UBSan-safe).
    const float interval = std::clamp(std::ceil(1.f / m_loadFactor), 1.f, static_cast<float>(maxInterval));
    return static_cast<uint32_t>(interval);
}

uint32_t TickGovernor::effectiveBudget(uint32_t staticBudget) const noexcept {
    if (!m_params.enabled || staticBudget == 0u)
        return staticBudget; // disabled => unchanged; 0 (unlimited) => still unlimited (rate lever only)
    const uint32_t scaled = static_cast<uint32_t>(std::round(static_cast<float>(staticBudget) * m_loadFactor));
    const uint32_t floor = std::min(staticBudget, m_params.budgetFloorBytes);
    return std::max(floor, scaled);
}

uint32_t TickGovernor::aiSampleStride() const noexcept {
    if (!m_params.enabled)
        return 1u;
    const uint32_t maxStride = m_params.maxAiStride == 0u ? 1u : m_params.maxAiStride;
    const float stride = std::clamp(std::round(1.f / m_loadFactor), 1.f, static_cast<float>(maxStride));
    return static_cast<uint32_t>(stride);
}

float TickGovernor::interestScale() const noexcept {
    if (!m_params.enabled)
        return 1.f;
    // loadFactor is already clamped to [floor, 1]; the fraction floor clamps the shed depth. A
    // misconfigured fraction outside [0, 1] is clamped defensively (config validation should have
    // caught it) so the scale can never grow the radius or go non-positive.
    const float minFrac = std::clamp(m_params.minInterestFraction, 0.f, 1.f);
    return std::max(minFrac, m_loadFactor);
}

} // namespace fl
