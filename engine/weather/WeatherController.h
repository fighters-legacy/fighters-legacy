// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "weather/WeatherTypes.h"

#include <RenderTypes.h>

#include <cstdint>

namespace fl {

struct WeatherControllerParams {
    // Game seconds per real second. 10 = 1 real minute → 10 game minutes;
    // a full 24-hour day/night cycle takes ~2.4 real hours at the default.
    float timeScaleRatio{10.f};
    // Real-seconds dwell range before an autonomous preset transition fires.
    float transitionMinSeconds{120.f};
    float transitionMaxSeconds{300.f};
};

static constexpr float kDefaultTimeScaleRatio = 10.f;

class WeatherController {
  public:
    explicit WeatherController(const WeatherControllerParams& params = {});

    // Advance the time-of-day clock and gust oscillator by simDt real seconds.
    // Also decrements the autonomous-transition dwell timer.
    // Call once per sim tick from the sim thread.
    void advance(double simDt);

    // Instant overrides — call via GameLoop::enqueueSimCallback() from non-sim threads.
    void setPreset(WeatherPreset p);
    void setTimeOfDay(float hours);                // [0, 24); wraps if out of range
    void setWind(float headingDeg, float speedMs); // meteorological: FROM direction

    [[nodiscard]] WeatherPreset preset() const noexcept {
        return m_preset;
    }
    [[nodiscard]] float timeOfDay() const noexcept {
        return m_timeOfDay;
    }

    // Instantaneous world-frame wind including the current gust component (m/s).
    [[nodiscard]] float windX() const noexcept;
    [[nodiscard]] float windZ() const noexcept;

    // Per-tick stochastic turbulence amplitude (m/s); 0 for Clear.
    [[nodiscard]] float turbulenceAmplitude() const noexcept {
        return m_turbulenceAmp;
    }

    // Derive a full EnvironmentState from current weather and time. Pure / no side-effects.
    [[nodiscard]] EnvironmentState computeEnvironment() const;

    // Static helpers — safe to call on the main thread using only wire-received values.
    [[nodiscard]] static glm::vec3 sunDirectionFromTime(float timeOfDay);
    static void applyPresetToEnv(WeatherPreset p, float timeOfDay, EnvironmentState& env);

  private:
    WeatherControllerParams m_params{};

    WeatherPreset m_preset{WeatherPreset::PartlyCloudy}; // sandbox default
    float m_timeOfDay{9.f};                              // sandbox default: 09:00
    float m_dwellRemaining{120.f};                       // real seconds until next auto-transition

    // Steady wind (meteorological FROM direction)
    float m_windHeadingDeg{270.f}; // direction wind blows FROM; default westerly
    float m_windSpeedMs{5.f};      // default PartlyCloudy speed
    bool m_windMissionSet{false};  // true = setWind() was called; preset changes don't override

    // Gust oscillator — advances in real time (unscaled by timeScaleRatio)
    float m_gustPhase{0.f};       // radians; wraps mod 2π
    float m_gustFrequency{0.12f}; // rad/s; ~52-second period; randomised on transitions
    float m_gustAmplitude{2.0f};  // m/s; default PartlyCloudy

    // Turbulence amplitude (m/s) — set from preset, used by WorldBroadcaster
    float m_turbulenceAmp{0.0f};

    uint32_t m_rng{0xDEADBEEF}; // LCG state for transition randomisation
    uint32_t lcg() noexcept;    // returns next LCG value and advances m_rng

    // Apply preset-keyed defaults (gust amplitude, turbulence, wind speed if not mission-set).
    void applyPresetDefaults();
};

} // namespace fl
