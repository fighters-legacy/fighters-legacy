// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "flight/Geodetic.h"       // kEarthRadiusM, worldToGeodetic (geographic sun, #481)
#include "weather/SolarPosition.h" // julianDay, solarAngles (geographic sun, #481)
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
    void setWind(float headingDeg, float speedMs); // meteorological: FROM direction; clears any profile
    void setDate(int year, int month, int day);    // UTC calendar date for the geographic sun (#481)

    // Per-mission time scale (#207): game seconds per real second for the day/night clock. The mission
    // loader applies the mission's `time_scale` here (applyMission), overriding the server-config default.
    // Non-positive values are ignored (a frozen clock is a bug, not a feature). Sim-thread only.
    void setTimeScaleRatio(float ratio) noexcept;
    [[nodiscard]] float timeScaleRatio() const noexcept {
        return m_params.timeScaleRatio;
    }

    // Altitude wind profile (#489). A knot is authored meteorologically: altitude (m MSL), speed (m/s),
    // and the heading the wind blows FROM (deg). setWindProfile converts each to a world-frame vector
    // ONCE here (no per-frame libm; the client interpolates the vectors), sorts by altitude, and caps
    // at EnvironmentState::kWindProfileMaxKnots. An empty span clears it (== clearWindProfile). Both
    // setWind() and setWindProfile() are mutually exclusive — the last one called wins.
    struct WindProfileMetKnot {
        float altM{0.0f};
        float speedMs{0.0f};
        float headingDeg{0.0f}; // FROM direction
    };
    void setWindProfile(const std::vector<WindProfileMetKnot>& knots);
    void clearWindProfile();
    [[nodiscard]] bool hasWindProfile() const noexcept {
        return m_windProfileCount > 0;
    }
    // World-frame wind (m/s) at altitude altM, gust folded — the SAME value computed via WindProfile.h
    // windAtAltitude on env-filled state (used by the server per-entity). Falls back to windX/windZ.
    [[nodiscard]] glm::vec2 windAtAltitude(float altM) const noexcept;

    [[nodiscard]] WeatherPreset preset() const noexcept {
        return m_preset;
    }
    [[nodiscard]] float timeOfDay() const noexcept {
        return m_timeOfDay;
    }

    // The mission anchor's longitude, in radians east (#1359). `setTimeOfDay` is LOCAL time at the
    // mission's own location — that is what a mission author writes and what the HUD clock shows —
    // so the anchor's longitude is what converts it to the UTC instant the sun is computed from.
    // Unset (0 = Greenwich) leaves local and UTC identical, which is the pre-#1359 behaviour.
    void setAnchorLongitude(double lonRad) noexcept;

    // Hours to ADD to local time to get UTC: -longitude/15. Mean solar time, not a civil timezone —
    // there is no tz database here and the sun does not use one.
    [[nodiscard]] double utcOffsetHours() const noexcept;

    // The shared UTC clock as a Julian Day (date + fractional UTC time-of-day). This is what a client
    // needs, together with its own camera latitude/longitude, to compute the geographic sun (#481) —
    // so the day/night terminator moves correctly across longitudes and two players far apart see
    // different local suns. Broadcast in MsgWeatherState.
    //
    // ⚠ This is NOT `dayStart + timeOfDay/24`: time-of-day is LOCAL at the anchor, and the conversion
    // is what #1359 was. Feeding local time in as UTC put the shipped sandbox (lon -115) 7h40m ahead
    // of itself — `time: {hour: 12}` rendered at 04:20 local, before sunrise, and the world went black.
    [[nodiscard]] double utcJulianDay() const noexcept;

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

    // Geographic sun (#481). Computes the true world-frame direction TOWARD the sun for an observer
    // at `observerPos` (world metres) and the shared UTC `julianDay`, via the NOAA solar-position
    // algorithm (SolarPosition.h) projected through the local ENU basis (LocalFrame.h). `R` is the
    // planet radius. Client-side and per-observer: the terminator moves across longitudes for free.
    [[nodiscard]] static glm::vec3 geographicSunDirection(double julianDay, glm::dvec3 observerPos,
                                                          double R = kEarthRadiusM);

    // Overwrite `env`'s sun direction, colour and ambient with the geographic sun for this observer.
    // `env.cloudCoverage` must already be set (call applyPresetToEnv first); the lighting is derived
    // from the true local solar elevation rather than the legacy planar time-of-day sinusoid.
    static void applyGeographicSun(EnvironmentState& env, double julianDay, glm::dvec3 observerPos,
                                   double R = kEarthRadiusM);

    // Geographic night sky (#484): fills `env`'s Moon direction/phase and the world->celestial star
    // rotation for an observer at `observerPos` and the shared UTC `julianDay`, via LunarPosition.h +
    // CelestialFrame.h. Client-side and per-observer, like applyGeographicSun (which must be called
    // first — the star/Moon night factor keys off env.sunDirection). Sets env.celestialValid = true.
    static void applyGeographicCelestial(EnvironmentState& env, double julianDay, glm::dvec3 observerPos,
                                         double R = kEarthRadiusM);

  private:
    WeatherControllerParams m_params{};

    WeatherPreset m_preset{WeatherPreset::PartlyCloudy}; // sandbox default
    float m_timeOfDay{9.f};                              // sandbox default: 09:00 LOCAL at the anchor
    // Julian Day at 00:00 UTC of the current date; utcJulianDay() = m_utcDayStartJd + m_timeOfDay/24.
    // Advances by one whole day whenever m_timeOfDay wraps past midnight. Default: 2025-06-21 (#481).
    double m_utcDayStartJd{julianDay(2025, 6, 21, 0.0)};
    // Longitude of the mission anchor, radians east (#1359). 0 = Greenwich, i.e. local time IS UTC.
    double m_anchorLonRad{0.0};
    float m_dwellRemaining{120.f}; // real seconds until next auto-transition

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

    // Altitude wind profile (#489) — steady world-frame vectors per knot, ascending by altitude, gust
    // added at read time. count 0 = no profile (fall back to the m_windHeading/Speed datum scalar).
    struct WindProfileStored {
        float altM{0.0f};
        float windX{0.0f};
        float windZ{0.0f};
    };
    WindProfileStored m_windProfile[EnvironmentState::kWindProfileMaxKnots]{};
    int m_windProfileCount{0};

    uint32_t m_rng{0xDEADBEEF}; // LCG state for transition randomisation
    uint32_t lcg() noexcept;    // returns next LCG value and advances m_rng

    // Apply preset-keyed defaults (gust amplitude, turbulence, wind speed if not mission-set).
    void applyPresetDefaults();
};

} // namespace fl
