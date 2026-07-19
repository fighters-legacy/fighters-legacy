// SPDX-License-Identifier: GPL-3.0-or-later
#include "weather/WeatherController.h"

#include "flight/LocalFrame.h"      // enuBasis for the geographic sun (#481)
#include "weather/CelestialFrame.h" // sidereal time + equatorial->world (#484)
#include "weather/LunarPosition.h"  // Moon ephemeris (#484)

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

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

static constexpr PresetDefaults kPresetDefaults[7] = {
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
    // Snow
    {5.0f, 1.5f, 8.f, 0.0002f, 20000.f, 0.85f},
    // Blizzard
    {10.0f, 4.0f, 14.f, 0.0006f, 5000.f, 0.95f},
};

inline const PresetDefaults& defaults(WeatherPreset p) {
    // p can originate from an untrusted MsgWeatherState.preset (any uint8_t value): clamp an
    // out-of-range preset to Clear so a malicious/garbage value can never index past
    // kPresetDefaults. This is the single indexing site, so the guard covers every caller.
    std::size_t idx = static_cast<std::size_t>(p);
    if (idx >= sizeof(kPresetDefaults) / sizeof(kPresetDefaults[0]))
        idx = 0; // Clear
    return kPresetDefaults[idx];
}

// Wrap hours to [0, 24)
inline float wrapHours(float h) {
    h = std::fmod(h, 24.f);
    if (h < 0.f)
        h += 24.f;
    return h;
}

// Sun colour + ambient from the solar elevation, expressed as `elev` = sin(elevation angle) ∈ [−1,1]
// (so +1 = zenith, 0 = horizon, negative = below). Reads env.cloudCoverage; sets env.sunColor and
// env.ambientColor. Shared by the legacy planar path (applyPresetToEnv) and the geographic sun
// (applyGeographicSun) so both light the scene identically for a given elevation (#481).
void applySunLighting(EnvironmentState& env, float elev) {
    if (elev > 0.f) {
        float t = glm::clamp(elev, 0.f, 1.f);
        // Low elevation → warm orange; high elevation → near white
        glm::vec3 sunLow{1.0f, 0.55f, 0.20f};
        glm::vec3 sunHigh{1.0f, 0.97f, 0.88f};
        glm::vec3 sunColor = glm::mix(sunLow, sunHigh, t);
        float cloudDim = 1.f - env.cloudCoverage * 0.7f;
        env.sunColor = sunColor * cloudDim;
    } else {
        // Night: very dim blue tint
        env.sunColor = glm::vec3{0.02f, 0.02f, 0.08f};
    }

    // Ambient: overcast lifts ambient (diffuse sky fill), storm darkens it
    float nightFactor = glm::clamp(1.f - elev * 2.f, 0.f, 1.f); // 0=day, 1=night
    glm::vec3 ambientDay{0.10f, 0.12f, 0.15f};
    glm::vec3 ambientOvercast{0.22f, 0.22f, 0.24f};
    glm::vec3 ambientStorm{0.06f, 0.07f, 0.09f};
    glm::vec3 ambientNight{0.02f, 0.02f, 0.05f};

    glm::vec3 ambient;
    if (env.cloudCoverage < 0.5f) {
        ambient = glm::mix(ambientDay, ambientNight, nightFactor);
    } else if (env.cloudCoverage < 0.85f) {
        ambient = glm::mix(ambientOvercast, ambientNight, nightFactor * 0.5f);
    } else {
        ambient = glm::mix(ambientStorm, ambientNight, nightFactor * 0.5f);
    }
    env.ambientColor = ambient;
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

void WeatherController::setTimeScaleRatio(float ratio) noexcept {
    // Ignore non-positive ratios: a zero/negative clock would freeze or reverse time-of-day, which is a
    // content bug rather than an intent. The gust oscillator is deliberately unaffected (it runs in real
    // time regardless of the day/night scale).
    if (ratio > 0.f)
        m_params.timeScaleRatio = ratio;
}

void WeatherController::setWind(float headingDeg, float speedMs) {
    m_windHeadingDeg = std::fmod(headingDeg, 360.f);
    if (m_windHeadingDeg < 0.f)
        m_windHeadingDeg += 360.f;
    m_windSpeedMs = speedMs;
    m_windMissionSet = true;
    clearWindProfile(); // a flat mission wind overrides any altitude profile
}

void WeatherController::clearWindProfile() {
    m_windProfileCount = 0;
}

void WeatherController::setWindProfile(const std::vector<WindProfileMetKnot>& knots) {
    // Convert each met knot (FROM heading) to a steady world-frame vector once, sort by altitude,
    // and cap at the wire/POD limit. Empty -> clear.
    std::vector<WindProfileStored> tmp;
    tmp.reserve(knots.size());
    for (const auto& k : knots) {
        const float rad = glm::radians(k.headingDeg);
        // Blowing direction = negated FROM direction (matches windX()/windZ()).
        tmp.push_back({k.altM, -std::sin(rad) * k.speedMs, -std::cos(rad) * k.speedMs});
    }
    std::sort(tmp.begin(), tmp.end(),
              [](const WindProfileStored& a, const WindProfileStored& b) { return a.altM < b.altM; });
    m_windProfileCount = static_cast<int>(std::min<std::size_t>(tmp.size(), EnvironmentState::kWindProfileMaxKnots));
    for (int i = 0; i < m_windProfileCount; ++i)
        m_windProfile[i] = tmp[static_cast<std::size_t>(i)];
    if (m_windProfileCount > 0)
        m_windMissionSet = true; // like setWind: preset changes don't clobber a set profile
}

glm::vec2 WeatherController::windAtAltitude(float altM) const noexcept {
    if (m_windProfileCount <= 0)
        return glm::vec2(windX(), windZ());
    // Gust magnitude (m/s) added along each knot's own wind direction — the same oscillator the datum
    // scalar folds in, so the surface knot tracks windX()/windZ() when authored to match.
    const float gust = m_gustAmplitude * std::sin(m_gustPhase);
    auto fold = [&](const WindProfileStored& k) -> glm::vec2 {
        const float mag = std::sqrt(k.windX * k.windX + k.windZ * k.windZ);
        if (mag < 1e-4f)
            return glm::vec2(k.windX, k.windZ);
        const float ux = k.windX / mag, uz = k.windZ / mag;
        return glm::vec2(k.windX + gust * ux, k.windZ + gust * uz);
    };
    if (altM <= m_windProfile[0].altM)
        return fold(m_windProfile[0]);
    const int last = m_windProfileCount - 1;
    if (altM >= m_windProfile[last].altM)
        return fold(m_windProfile[last]);
    for (int i = 0; i < last; ++i) {
        const auto& a = m_windProfile[i];
        const auto& b = m_windProfile[i + 1];
        if (altM >= a.altM && altM <= b.altM) {
            const float span = b.altM - a.altM;
            const float t = (span > 1e-6f) ? (altM - a.altM) / span : 0.0f;
            const glm::vec2 fa = fold(a), fb = fold(b);
            return fa + (fb - fa) * t;
        }
    }
    return fold(m_windProfile[last]);
}

void WeatherController::advance(double simDt) {
    const float dt = static_cast<float>(simDt);

    // Game clock — scaled. Track whole-day rollovers so the UTC date (and thus the solar declination
    // feeding the geographic sun, #481) advances across midnight.
    const float rawTod = m_timeOfDay + static_cast<float>(simDt * m_params.timeScaleRatio / 3600.0);
    if (rawTod >= 24.f)
        m_utcDayStartJd += std::floor(rawTod / 24.f);
    else if (rawTod < 0.f)
        m_utcDayStartJd += std::floor(rawTod / 24.f); // supports a reversed clock, if ever set
    m_timeOfDay = wrapHours(rawTod);

    // Gust oscillator — real time (unscaled)
    m_gustPhase = std::fmod(m_gustPhase + dt * m_gustFrequency, glm::two_pi<float>());

    // Autonomous-transition dwell timer — real time
    m_dwellRemaining -= dt;
    if (m_dwellRemaining <= 0.f) {
        // Cycle through presets: Clear→PartlyCloudy→Overcast→Rain→Storm→Clear.
        // Snow and Blizzard are operator-set only — do not auto-transition away from them.
        if (m_preset <= WeatherPreset::Storm) {
            uint8_t next = (static_cast<uint8_t>(m_preset) + 1u) % 5u;
            setPreset(static_cast<WeatherPreset>(next));
        }
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

    // Legacy planar sun direction from time-of-day (the server/nominal path). The CLIENT overwrites
    // this with the geographic sun (applyGeographicSun) once it knows its camera lat/lon (#481).
    env.sunDirection = sunDirectionFromTime(timeOfDay);
    // Colour + ambient from the planar sun's elevation scalar (its world-Y = sin(elevation) here).
    applySunLighting(env, env.sunDirection.y);
    env.timeOfDay = timeOfDay;
    env.isSnowPrecipitation = (p == WeatherPreset::Snow || p == WeatherPreset::Blizzard);
}

// ---------------------------------------------------------------------------
// computeEnvironment
// ---------------------------------------------------------------------------

EnvironmentState WeatherController::computeEnvironment() const {
    EnvironmentState env{};
    applyPresetToEnv(m_preset, m_timeOfDay, env);
    env.windX = windX();
    env.windZ = windZ();
    env.turbulenceAmp = m_turbulenceAmp; // #426: broadcast so the client reproduces turbulence exactly
    // Altitude wind profile (#489): fill the gust-folded per-knot vectors so the client interpolates
    // the same wind by altitude. The surface knot also becomes the datum windX/windZ (what an old
    // client without the TLV sees), so the two representations agree at ground level.
    env.windProfileCount = static_cast<uint8_t>(m_windProfileCount);
    for (int i = 0; i < m_windProfileCount; ++i) {
        const glm::vec2 w = windAtAltitude(m_windProfile[i].altM);
        env.windProfile[i] = {m_windProfile[i].altM, w.x, w.y};
    }
    if (m_windProfileCount > 0) {
        const glm::vec2 surface = windAtAltitude(m_windProfile[0].altM);
        env.windX = surface.x;
        env.windZ = surface.y;
    }
    return env;
}

// ---------------------------------------------------------------------------
// UTC clock + geographic sun (#481)
// ---------------------------------------------------------------------------

void WeatherController::setDate(int year, int month, int day) {
    m_utcDayStartJd = julianDay(year, month, day, 0.0);
}

double WeatherController::utcJulianDay() const noexcept {
    return m_utcDayStartJd + static_cast<double>(m_timeOfDay) / 24.0;
}

glm::vec3 WeatherController::geographicSunDirection(double jd, glm::dvec3 observerPos, double R) {
    const LatLonAlt lla = worldToGeodetic(observerPos.x, observerPos.y, observerPos.z, R);
    const SolarAngles a = solarAngles(lla.lat_rad, lla.lon_rad, jd);
    const glm::mat3 basis = enuBasis(observerPos, R); // columns East, North, Up
    return glm::normalize(basis * sunDirectionEnu(a));
}

void WeatherController::applyGeographicSun(EnvironmentState& env, double jd, glm::dvec3 observerPos, double R) {
    const LatLonAlt lla = worldToGeodetic(observerPos.x, observerPos.y, observerPos.z, R);
    const SolarAngles a = solarAngles(lla.lat_rad, lla.lon_rad, jd);
    const glm::mat3 basis = enuBasis(observerPos, R);
    env.sunDirection = glm::normalize(basis * sunDirectionEnu(a));
    // Light the scene from the TRUE local solar elevation, not the planar world-Y proxy.
    applySunLighting(env, static_cast<float>(std::sin(a.elevationRad)));
}

void WeatherController::applyGeographicCelestial(EnvironmentState& env, double jd, glm::dvec3 observerPos, double R) {
    const LatLonAlt lla = worldToGeodetic(observerPos.x, observerPos.y, observerPos.z, R);
    const double lst = localSiderealTimeRad(jd, lla.lon_rad);

    const MoonEquatorial moon = moonEquatorial(jd);
    env.moonDirection = equatorialToWorld(moon.raRad, moon.decRad, lla.lat_rad, lst, observerPos, R);
    env.moonAngularRadius = static_cast<float>(moonAngularRadiusRad(moon.distanceKm));
    env.moonIllumination = static_cast<float>(moon.illuminatedFraction);
    env.worldToCelestial = worldToCelestial(lla.lat_rad, lst, observerPos, R);
    env.celestialValid = true;
}

} // namespace fl
