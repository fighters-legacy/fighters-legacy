// SPDX-License-Identifier: GPL-3.0-or-later
#include "weather/WeatherController.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <cmath>

namespace fl {

namespace {

// Per-preset constants --------------------------------------------------------

struct PresetDefaults {
    float gustAmplitude;    // m/s
    float turbulenceAmp;    // m/s
    float windSpeedDefault; // m/s (used when wind is not mission-set)
    float fogDensity;       // exponential coefficient
    float fogStartDist;     // metres
    float cloudCoverage;    // [0, 1]
};

static constexpr PresetDefaults kPresetDefaults[5] = {
    // Clear
    {0.5f, 0.0f, 2.f, 0.f, 50000.f, 0.00f},
    // PartlyCloudy
    {2.0f, 0.3f, 5.f, 0.f, 50000.f, 0.35f},
    // Overcast
    {4.0f, 0.8f, 8.f, 0.0001f, 40000.f, 0.75f},
    // Rain
    {7.0f, 2.5f, 12.f, 0.0003f, 10000.f, 0.85f},
    // Storm
    {12.0f, 6.0f, 18.f, 0.0008f, 3000.f, 0.95f},
};

inline const PresetDefaults& defaults(WeatherPreset p) {
    return kPresetDefaults[static_cast<uint8_t>(p)];
}

// Wrap hours to [0, 24)
inline float wrapHours(float h) {
    h = std::fmod(h, 24.f);
    if (h < 0.f)
        h += 24.f;
    return h;
}

} // anonymous namespace

// ---------------------------------------------------------------------------

WeatherController::WeatherController(const WeatherControllerParams& params) : m_params(params) {
    applyPresetDefaults();
}

uint32_t WeatherController::lcg() noexcept {
    m_rng = m_rng * 1664525u + 1013904223u;
    return m_rng;
}

void WeatherController::applyPresetDefaults() {
    const auto& d = defaults(m_preset);
    m_gustAmplitude = d.gustAmplitude;
    m_turbulenceAmp = d.turbulenceAmp;
    if (!m_windMissionSet)
        m_windSpeedMs = d.windSpeedDefault;
}

void WeatherController::setPreset(WeatherPreset p) {
    m_preset = p;
    m_dwellRemaining = m_params.transitionMinSeconds;
    applyPresetDefaults();
    if (!m_windMissionSet) {
        // Randomise wind heading slightly on preset change
        uint32_t r = lcg();
        m_windHeadingDeg = static_cast<float>(r % 360u);
    }
    // Randomise gust frequency [0.08, 0.20] rad/s
    uint32_t r2 = lcg();
    m_gustFrequency = 0.08f + 0.12f * static_cast<float>(r2 & 0xFFu) / 255.f;
}

void WeatherController::setTimeOfDay(float hours) {
    m_timeOfDay = wrapHours(hours);
}

void WeatherController::setWind(float headingDeg, float speedMs) {
    m_windHeadingDeg = std::fmod(headingDeg, 360.f);
    if (m_windHeadingDeg < 0.f)
        m_windHeadingDeg += 360.f;
    m_windSpeedMs = speedMs;
    m_windMissionSet = true;
}

void WeatherController::advance(double simDt) {
    const float dt = static_cast<float>(simDt);

    // Game clock — scaled
    m_timeOfDay = wrapHours(m_timeOfDay + static_cast<float>(simDt * m_params.timeScaleRatio / 3600.0));

    // Gust oscillator — real time (unscaled)
    m_gustPhase = std::fmod(m_gustPhase + dt * m_gustFrequency, glm::two_pi<float>());

    // Autonomous-transition dwell timer — real time
    m_dwellRemaining -= dt;
    if (m_dwellRemaining <= 0.f) {
        // Cycle through presets: Clear→PartlyCloudy→Overcast→Rain→Storm→Clear
        uint8_t next = (static_cast<uint8_t>(m_preset) + 1u) % 5u;
        setPreset(static_cast<WeatherPreset>(next));
        // Reset dwell to a random duration in [min, max]
        uint32_t r = lcg();
        float range = m_params.transitionMaxSeconds - m_params.transitionMinSeconds;
        m_dwellRemaining = m_params.transitionMinSeconds + range * static_cast<float>(r & 0xFFFFu) / 65535.f;
    }
}

float WeatherController::windX() const noexcept {
    // Meteorological: heading is FROM direction. Wind vector = -heading direction.
    // FROM 270° (west) → blows east (+X).
    float rad = glm::radians(m_windHeadingDeg);
    float steadyX = -std::sin(rad) * m_windSpeedMs; // negate: FROM direction → blowing direction
    float gust = m_gustAmplitude * std::sin(m_gustPhase);
    return steadyX + gust * (-std::sin(rad));
}

float WeatherController::windZ() const noexcept {
    float rad = glm::radians(m_windHeadingDeg);
    float steadyZ = -std::cos(rad) * m_windSpeedMs;
    float gust = m_gustAmplitude * std::sin(m_gustPhase);
    return steadyZ + gust * (-std::cos(rad));
}

// ---------------------------------------------------------------------------
// sunDirectionFromTime
// ---------------------------------------------------------------------------

glm::vec3 WeatherController::sunDirectionFromTime(float timeOfDay) {
    // Simple circular orbit: elevation = sin(π*(t-6)/12) for one 24h cycle.
    // At t=6: elevation=0 (sunrise, +X azimuth)
    // At t=12: elevation=1 (noon, Y-up)
    // At t=18: elevation=0 (sunset, -X azimuth)
    // At t=0/24: elevation=-1 (midnight, below horizon)
    float angle = glm::pi<float>() * (timeOfDay - 6.f) / 12.f;
    float elevation = std::sin(angle);
    float horiz = std::cos(angle); // horizontal extent (+X dawn, -X dusk)
    // Keep Y slightly negative when below horizon so shadow cascades still compute.
    glm::vec3 dir{horiz, elevation, 0.f};
    return glm::normalize(dir);
}

// ---------------------------------------------------------------------------
// applyPresetToEnv — static, called on main thread from received wire data
// ---------------------------------------------------------------------------

void WeatherController::applyPresetToEnv(WeatherPreset p, float timeOfDay, EnvironmentState& env) {
    const auto& d = defaults(p);
    env.fogDensity = d.fogDensity;
    env.fogStartDist = d.fogStartDist;
    env.cloudCoverage = d.cloudCoverage;

    // Sun direction from time
    env.sunDirection = sunDirectionFromTime(timeOfDay);

    // Sun color: warm orange at dawn/dusk, white at noon, dark at night
    float elevation = env.sunDirection.y;
    if (elevation > 0.f) {
        float t = glm::clamp(elevation, 0.f, 1.f);
        // Low elevation → warm orange; high elevation → near white
        glm::vec3 sunLow{1.0f, 0.55f, 0.20f};
        glm::vec3 sunHigh{1.0f, 0.97f, 0.88f};
        glm::vec3 sunColor = glm::mix(sunLow, sunHigh, t);
        // Under heavy cloud cover, dim the sun
        float cloudDim = 1.f - d.cloudCoverage * 0.7f;
        env.sunColor = sunColor * cloudDim;
    } else {
        // Night: very dim blue tint
        env.sunColor = glm::vec3{0.02f, 0.02f, 0.08f};
    }

    // Ambient: overcast lifts ambient (diffuse sky fill), storm darkens it
    float nightFactor = glm::clamp(1.f - elevation * 2.f, 0.f, 1.f); // 0=day, 1=night
    glm::vec3 ambientDay{0.10f, 0.12f, 0.15f};
    glm::vec3 ambientOvercast{0.22f, 0.22f, 0.24f};
    glm::vec3 ambientStorm{0.06f, 0.07f, 0.09f};
    glm::vec3 ambientNight{0.02f, 0.02f, 0.05f};

    glm::vec3 ambient;
    if (d.cloudCoverage < 0.5f) {
        ambient = glm::mix(ambientDay, ambientNight, nightFactor);
    } else if (d.cloudCoverage < 0.85f) {
        ambient = glm::mix(ambientOvercast, ambientNight, nightFactor * 0.5f);
    } else {
        ambient = glm::mix(ambientStorm, ambientNight, nightFactor * 0.5f);
    }
    env.ambientColor = ambient;
    env.timeOfDay = timeOfDay;
}

// ---------------------------------------------------------------------------
// computeEnvironment
// ---------------------------------------------------------------------------

EnvironmentState WeatherController::computeEnvironment() const {
    EnvironmentState env{};
    applyPresetToEnv(m_preset, m_timeOfDay, env);
    env.windX = windX();
    env.windZ = windZ();
    return env;
}

} // namespace fl
