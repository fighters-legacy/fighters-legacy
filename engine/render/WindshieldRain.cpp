// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/WindshieldRain.h"
#include <cmath>
#include <cstdint>

namespace fl {

WindshieldRain::WindshieldRain() {
    // Deterministic LCG (Numerical Recipes parameters) — same sequence every run.
    uint32_t seed = 0xDEADBEEFu;
    for (int i = 0; i < kDropCount; ++i) {
        seed = seed * 1664525u + 1013904223u;
        m_drops[i].x = static_cast<float>(seed >> 8) / static_cast<float>(1u << 24);
        seed = seed * 1664525u + 1013904223u;
        m_drops[i].phase = static_cast<float>(seed >> 8) / static_cast<float>(1u << 24);
    }
}

void WindshieldRain::update(float dt, const EnvironmentState& env, float rollRad, bool isSnow) {
    m_elementCount = 0;
    if (env.cloudCoverage < kRainThreshold)
        return;

    const float intensity = (env.cloudCoverage - kRainThreshold) / (1.0f - kRainThreshold);
    const float intClamped = intensity < 0.f ? 0.f : (intensity > 1.f ? 1.f : intensity);

    // Total wind speed drives fall rate; windZ is depth-axis from cockpit view
    // so it does not contribute screen-space horizontal lean.
    const float windSpeed = std::sqrt(env.windX * env.windX + env.windZ * env.windZ);

    const float baseFallSpeed = isSnow ? kSnowBaseFallSpeed : kRainBaseFallSpeed;
    const float windSpeedScale = isSnow ? kSnowWindSpeedScale : kRainWindSpeedScale;
    const float windXTiltScale = isSnow ? kSnowWindXTiltScale : kRainWindXTiltScale;
    const float baseAlpha = isSnow ? kSnowBaseAlpha : kRainBaseAlpha;
    const float alphaRange = isSnow ? kSnowAlphaRange : kRainAlphaRange;
    const float baseStreakLen = isSnow ? kSnowBaseStreakLen : kRainBaseStreakLen;
    const float streakLenRange = isSnow ? kSnowStreakLenRange : kRainStreakLenRange;
    const float strokeWidth = isSnow ? 2.0f : 1.0f;

    const float fallSpeed = baseFallSpeed + windSpeedScale * windSpeed;
    m_timeAccum += dt * fallSpeed;
    // Do not fmod m_timeAccum here: reducing it to [0,1) each frame would make
    // all 48 drops jump simultaneously. Per-drop fmod in the loop handles wrap.
    // Float32 precision is adequate for >7000 simulated hours before noticeable drift.

    const float alpha = baseAlpha + alphaRange * intClamped;
    const float streakLen = baseStreakLen + streakLenRange * intClamped;
    // windX (crosswind) drives screen-space lateral lean.
    const float windDx = env.windX * windXTiltScale;

    // Rotate the streak direction by the aircraft roll angle so rain tracks world-down
    // rather than screen-down. Right-hand rotation in screen space (Y positive downward):
    //   rdx = dx*cos(r) + dy*sin(r),  rdy = -dx*sin(r) + dy*cos(r)
    const float cr = std::cos(rollRad);
    const float sr = std::sin(rollRad);
    const float rdx = windDx * cr + streakLen * sr;
    const float rdy = -windDx * sr + streakLen * cr;

    for (int i = 0; i < kDropCount; ++i) {
        const float y = std::fmod(m_drops[i].phase + m_timeAccum, 1.0f);
        HudElement& el = m_elements[m_elementCount++];
        el.type = HudElement::Type::Line;
        el.x = m_drops[i].x;
        el.y = y;
        el.x2 = m_drops[i].x + rdx;
        el.y2 = y + rdy;
        el.strokeWidth = strokeWidth;
        if (isSnow) {
            el.r = 1.0f;
            el.g = 1.0f;
            el.b = 1.0f;
        } else {
            el.r = 0.7f;
            el.g = 0.8f;
            el.b = 1.0f;
        }
        el.a = alpha;
    }
}

std::span<const HudElement> WindshieldRain::elements() const {
    return {m_elements.data(), m_elementCount};
}

} // namespace fl
