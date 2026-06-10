// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "RenderTypes.h"
#include <array>
#include <cstddef>
#include <span>

namespace fl {

// Screen-space windshield precipitation overlay for the cockpit HUD.
// Produces animated semi-transparent Line elements simulating rain streaks
// or snow smears on the windshield glass.
//
// Active only when cloudCoverage >= kRainThreshold; callers must pass
// isSnow=true (altitude above kSnowAltitudeThresholdM) to switch visual mode.
// Caller is responsible for only appending elements() when in Cockpit mode.
class WindshieldRain {
  public:
    WindshieldRain(); // deterministic LCG seeds drop positions

    // Advance animation and rebuild elements.
    // cloudCoverage < kRainThreshold clears all elements.
    // isSnow: true when camera altitude exceeds the snow altitude threshold.
    void update(float dt, const EnvironmentState& env, float rollRad = 0.f, bool isSnow = false);

    [[nodiscard]] std::span<const HudElement> elements() const;

  private:
    static constexpr int kDropCount = 48;
    static constexpr float kRainThreshold = 0.75f;

    // Rain visual parameters
    static constexpr float kRainBaseFallSpeed = 0.5f;
    static constexpr float kRainWindSpeedScale = 0.015f;
    static constexpr float kRainWindXTiltScale = 0.0006f;
    static constexpr float kRainBaseAlpha = 0.15f;
    static constexpr float kRainAlphaRange = 0.25f;
    static constexpr float kRainBaseStreakLen = 0.025f;
    static constexpr float kRainStreakLenRange = 0.015f;

    // Snow visual parameters
    static constexpr float kSnowBaseFallSpeed = 0.20f;
    static constexpr float kSnowWindSpeedScale = 0.008f;
    static constexpr float kSnowWindXTiltScale = 0.0003f;
    static constexpr float kSnowBaseAlpha = 0.20f;
    static constexpr float kSnowAlphaRange = 0.20f;
    static constexpr float kSnowBaseStreakLen = 0.008f;
    static constexpr float kSnowStreakLenRange = 0.006f;

    struct Drop {
        float x;
        float phase;
    };

    std::array<Drop, kDropCount> m_drops{};
    std::array<HudElement, kDropCount> m_elements{};
    std::size_t m_elementCount{0};
    float m_timeAccum{0.f};
};

} // namespace fl
