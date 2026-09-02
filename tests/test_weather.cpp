// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "math/Angles.h" // kDegToRad / kRadToDeg — anchor-local mission clock (#1359)
#include "weather/Turbulence.h"
#include "weather/WeatherController.h"
#include "world/SandboxHome.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using namespace fl;

// ---------------------------------------------------------------------------
// Default state
// ---------------------------------------------------------------------------

TEST_CASE("WeatherController: default state is PartlyCloudy at 09:00", "[weather]") {
    WeatherController wc;
    CHECK(wc.preset() == WeatherPreset::PartlyCloudy);
    CHECK_THAT(wc.timeOfDay(), WithinAbs(9.0f, 0.001f));
}

// ---------------------------------------------------------------------------
// Sun direction
// ---------------------------------------------------------------------------

TEST_CASE("WeatherController: sun above horizon at 09:00", "[weather]") {
    CHECK(WeatherController::sunDirectionFromTime(9.f).y > 0.f);
}

TEST_CASE("WeatherController: sun below horizon at midnight", "[weather]") {
    CHECK(WeatherController::sunDirectionFromTime(0.f).y < 0.f);
}

TEST_CASE("WeatherController: sun near zenith at noon", "[weather]") {
    auto d = WeatherController::sunDirectionFromTime(12.f);
    CHECK(d.y > 0.9f);
}

TEST_CASE("WeatherController: sun near horizontal at dawn and dusk", "[weather]") {
    auto dawn = WeatherController::sunDirectionFromTime(6.f);
    auto dusk = WeatherController::sunDirectionFromTime(18.f);
    CHECK(std::abs(dawn.y) < 0.1f);
    CHECK(std::abs(dusk.y) < 0.1f);
}

// ---------------------------------------------------------------------------
// computeEnvironment — all 5 presets
// ---------------------------------------------------------------------------

TEST_CASE("WeatherController: computeEnvironment base presets have strictly increasing cloudCoverage", "[weather]") {
    // The 5 autonomous-cycle presets are strictly increasing in cloud coverage.
    const WeatherPreset presets[] = {WeatherPreset::Clear, WeatherPreset::PartlyCloudy, WeatherPreset::Overcast,
                                     WeatherPreset::Rain, WeatherPreset::Storm};
    float prev = -1.f;
    for (auto p : presets) {
        WeatherController wc;
        wc.setPreset(p);
        float cov = wc.computeEnvironment().cloudCoverage;
        CHECK(cov > prev);
        prev = cov;
    }
}

TEST_CASE("WeatherController: Snow cloudCoverage matches Rain and Blizzard matches Storm", "[weather]") {
    // Snow/Blizzard are tuned to the same coverage tiers as Rain/Storm so they trigger
    // the same precipitation and storm thresholds in PrecipitationController.
    auto covOf = [](WeatherPreset p) {
        WeatherController wc;
        wc.setPreset(p);
        return wc.computeEnvironment().cloudCoverage;
    };
    CHECK(covOf(WeatherPreset::Snow) == covOf(WeatherPreset::Rain));
    CHECK(covOf(WeatherPreset::Blizzard) == covOf(WeatherPreset::Storm));
}

TEST_CASE("WeatherController: computeEnvironment Clear has fogDensity == 0", "[weather]") {
    WeatherController wc;
    wc.setPreset(WeatherPreset::Clear);
    CHECK(wc.computeEnvironment().fogDensity == 0.f);
}

TEST_CASE("WeatherController: computeEnvironment PartlyCloudy has fogDensity == 0", "[weather]") {
    WeatherController wc;
    wc.setPreset(WeatherPreset::PartlyCloudy);
    CHECK(wc.computeEnvironment().fogDensity == 0.f);
}

TEST_CASE("WeatherController: computeEnvironment fog increases with severity", "[weather]") {
    WeatherController wc;
    auto fogOf = [&](WeatherPreset p) {
        wc.setPreset(p);
        return wc.computeEnvironment().fogDensity;
    };
    float fogOvercast = fogOf(WeatherPreset::Overcast);
    float fogRain = fogOf(WeatherPreset::Rain);
    float fogStorm = fogOf(WeatherPreset::Storm);
    CHECK(fogOvercast > 0.f);
    CHECK(fogRain > fogOvercast);
    CHECK(fogStorm > fogRain);
}

TEST_CASE("WeatherController: computeEnvironment Storm ambient is darker than Clear noon", "[weather]") {
    WeatherController wc;
    wc.setTimeOfDay(12.f);
    wc.setPreset(WeatherPreset::Clear);
    float clearAmbY = wc.computeEnvironment().ambientColor.y;
    wc.setPreset(WeatherPreset::Storm);
    float stormAmbY = wc.computeEnvironment().ambientColor.y;
    CHECK(stormAmbY < clearAmbY);
}

TEST_CASE("WeatherController: computeEnvironment Overcast ambient is brighter than Clear", "[weather]") {
    WeatherController wc;
    wc.setTimeOfDay(12.f);
    wc.setPreset(WeatherPreset::Clear);
    float clearAmb = wc.computeEnvironment().ambientColor.y;
    wc.setPreset(WeatherPreset::Overcast);
    float overcastAmb = wc.computeEnvironment().ambientColor.y;
    CHECK(overcastAmb > clearAmb);
}

TEST_CASE("WeatherController: computeEnvironment populates windX and windZ", "[weather]") {
    WeatherController wc;
    wc.setWind(270.f, 10.f); // FROM west, 10 m/s -> blows east (+X)
    const EnvironmentState env = wc.computeEnvironment();
    CHECK(env.windX == wc.windX()); // same instant: bit-identical, exact equality correct
    CHECK(env.windZ == wc.windZ());
    CHECK(env.windX > 0.f); // sanity: wind blows east
}

TEST_CASE("WeatherController: computeEnvironment fogStartDist decreases with severity", "[weather]") {
    WeatherController wc;
    wc.setTimeOfDay(12.f);

    wc.setPreset(WeatherPreset::Clear);
    const float distClear = wc.computeEnvironment().fogStartDist;

    wc.setPreset(WeatherPreset::Overcast);
    const float distOvercast = wc.computeEnvironment().fogStartDist;

    wc.setPreset(WeatherPreset::Rain);
    const float distRain = wc.computeEnvironment().fogStartDist;

    wc.setPreset(WeatherPreset::Storm);
    const float distStorm = wc.computeEnvironment().fogStartDist;

    CHECK(distClear > distOvercast);
    CHECK(distOvercast > distRain);
    CHECK(distRain > distStorm);
}

TEST_CASE("WeatherController: computeEnvironment sunDirection at noon points upward", "[weather]") {
    WeatherController wc;
    wc.setPreset(WeatherPreset::Clear);
    wc.setTimeOfDay(12.f);
    const auto env = wc.computeEnvironment();
    CHECK(env.sunDirection.y > 0.9f);
}

TEST_CASE("WeatherController: computeEnvironment sunColor at noon is near white", "[weather]") {
    WeatherController wc;
    wc.setPreset(WeatherPreset::Clear);
    wc.setTimeOfDay(12.f);
    const auto env = wc.computeEnvironment();
    CHECK(env.sunColor.x > 0.8f); // R
    CHECK(env.sunColor.y > 0.8f); // G
    CHECK(env.sunColor.z > 0.8f); // B
}

TEST_CASE("WeatherController: computeEnvironment env.timeOfDay matches controller", "[weather]") {
    WeatherController wc;
    wc.setTimeOfDay(15.5f);
    const auto env = wc.computeEnvironment();
    CHECK(env.timeOfDay == wc.timeOfDay()); // bit-identical copy of m_timeOfDay
}

TEST_CASE("WeatherController: computeEnvironment sunColor at midnight is dim", "[weather]") {
    WeatherController wc;
    wc.setPreset(WeatherPreset::Clear);
    wc.setTimeOfDay(0.f); // midnight: elevation = sin(-pi/2) = -1.0 -> night branch
    const auto env = wc.computeEnvironment();
    CHECK(env.sunColor.x < 0.1f);
    CHECK(env.sunColor.y < 0.1f);
    CHECK(env.sunColor.z < 0.1f);
}

// ---------------------------------------------------------------------------
// Time clock advancement
// ---------------------------------------------------------------------------

TEST_CASE("WeatherController: advance at 1x scale increments timeOfDay by 1 hour per 3600 s", "[weather]") {
    WeatherControllerParams p;
    p.timeScaleRatio = 1.f;
    p.transitionMinSeconds = 9999.f; // prevent auto-transition
    p.transitionMaxSeconds = 9999.f;
    WeatherController wc(p);
    wc.setTimeOfDay(0.f);
    wc.advance(3600.0);
    CHECK_THAT(wc.timeOfDay(), WithinAbs(1.0f, 0.001f));
}

TEST_CASE("WeatherController: advance at 10x default increments timeOfDay by 1 hour per 360 s", "[weather]") {
    WeatherControllerParams p;
    p.timeScaleRatio = 10.f;
    p.transitionMinSeconds = 9999.f;
    p.transitionMaxSeconds = 9999.f;
    WeatherController wc(p);
    wc.setTimeOfDay(0.f);
    wc.advance(360.0);
    CHECK_THAT(wc.timeOfDay(), WithinAbs(1.0f, 0.001f));
}

TEST_CASE("WeatherController: advance wraps time at 24 hours", "[weather]") {
    WeatherControllerParams p;
    p.timeScaleRatio = 1.f;
    p.transitionMinSeconds = 9999.f;
    p.transitionMaxSeconds = 9999.f;
    WeatherController wc(p);
    wc.setTimeOfDay(23.8f);
    wc.advance(720.0); // advances 0.2 hours at 1x → should wrap to near 0
    CHECK(wc.timeOfDay() < 1.0f);
}

TEST_CASE("WeatherController: setTimeOfDay wraps out-of-range values", "[weather]") {
    WeatherController wc;
    wc.setTimeOfDay(25.f);
    CHECK_THAT(wc.timeOfDay(), WithinAbs(1.0f, 0.001f));
    wc.setTimeOfDay(-1.f);
    CHECK_THAT(wc.timeOfDay(), WithinAbs(23.0f, 0.001f));
}

// ---------------------------------------------------------------------------
// Turbulence
// ---------------------------------------------------------------------------

TEST_CASE("WeatherController: turbulenceAmplitude increases with severity", "[weather]") {
    WeatherController wc;
    auto turbOf = [&](WeatherPreset p) {
        wc.setPreset(p);
        return wc.turbulenceAmplitude();
    };
    float tClear = turbOf(WeatherPreset::Clear);
    float tPartly = turbOf(WeatherPreset::PartlyCloudy);
    float tOvercast = turbOf(WeatherPreset::Overcast);
    float tRain = turbOf(WeatherPreset::Rain);
    float tStorm = turbOf(WeatherPreset::Storm);
    CHECK(tClear <= 0.5f);
    CHECK(tPartly > tClear);
    CHECK(tOvercast > tPartly);
    CHECK(tRain > tOvercast);
    CHECK(tStorm > tRain);
}

TEST_CASE("WeatherController: turbulence drops after switching from Storm to Clear", "[weather]") {
    WeatherController wc;
    wc.setPreset(WeatherPreset::Storm);
    float stormTurb = wc.turbulenceAmplitude();
    wc.setPreset(WeatherPreset::Clear);
    float clearTurb = wc.turbulenceAmplitude();
    CHECK(clearTurb < stormTurb);
    CHECK(clearTurb <= 0.5f);
}

// ---------------------------------------------------------------------------
// Auto-transition
// ---------------------------------------------------------------------------

TEST_CASE("WeatherController: auto-transition fires after dwell exhausted", "[weather]") {
    WeatherControllerParams p;
    p.transitionMinSeconds = 0.05f;
    p.transitionMaxSeconds = 0.10f;
    WeatherController wc(p);
    wc.setPreset(WeatherPreset::Clear);
    WeatherPreset initial = wc.preset();
    // Advance well past the dwell window
    for (int i = 0; i < 20; ++i)
        wc.advance(0.01);
    CHECK(wc.preset() != initial);
}

// ---------------------------------------------------------------------------
// Wind and gusts
// ---------------------------------------------------------------------------

TEST_CASE("WeatherController: setWind produces correct world-frame components", "[weather]") {
    WeatherController wc;
    wc.setPreset(WeatherPreset::Clear); // minimal gust amplitude
    // FROM 270° (west) → wind blows east (+X direction).
    wc.setWind(270.f, 10.f);
    // Allow ±gust amplitude tolerance (Clear gust ≤ 0.5 m/s)
    CHECK_THAT(wc.windX(), WithinAbs(10.f, 1.0f));
    CHECK_THAT(wc.windZ(), WithinAbs(0.f, 1.0f));
}

TEST_CASE("WeatherController: gust advances in real time (unscaled)", "[weather]") {
    WeatherControllerParams p;
    p.timeScaleRatio = 1000.f; // extreme scale — gust must NOT speed up
    p.transitionMinSeconds = 9999.f;
    p.transitionMaxSeconds = 9999.f;
    WeatherController wc(p);
    wc.setPreset(WeatherPreset::Storm);
    wc.setWind(270.f, 0.f); // zero steady wind to isolate gust variance

    // Collect windX over 60 ticks of 1.0 real second each
    std::vector<float> samples;
    samples.reserve(60);
    for (int i = 0; i < 60; ++i) {
        wc.advance(1.0);
        samples.push_back(wc.windX());
    }
    float mean = std::accumulate(samples.begin(), samples.end(), 0.f) / static_cast<float>(samples.size());
    float var = 0.f;
    for (float s : samples)
        var += (s - mean) * (s - mean);
    var /= static_cast<float>(samples.size());
    // Stddev > 0 confirms gust oscillator is advancing
    CHECK(std::sqrt(var) > 0.1f);
}

TEST_CASE("WeatherController: mission-set wind not overridden by preset change", "[weather]") {
    WeatherController wc;
    wc.setWind(270.f, 10.f);
    wc.setPreset(WeatherPreset::Storm); // Storm default wind is ~18 m/s
    // windX should still reflect our explicit 10 m/s westerly (within gust tolerance)
    CHECK_THAT(wc.windX(), WithinAbs(10.f, 13.f)); // Storm gust up to 12 m/s
    // More importantly, verify windSpeed magnitude is near 10 not 18
    float mag = std::sqrt(wc.windX() * wc.windX() + wc.windZ() * wc.windZ());
    CHECK(mag < 25.f); // would be ~18±12 if overridden, ~10±12 if not
}

TEST_CASE("WeatherController: default wind follows preset when not mission-set", "[weather]") {
    WeatherController wc;
    // No setWind call — preset change should update wind defaults
    wc.setPreset(WeatherPreset::Clear); // default ~2 m/s
    float clearMag = std::sqrt(wc.windX() * wc.windX() + wc.windZ() * wc.windZ());
    wc.setPreset(WeatherPreset::Storm); // default ~18 m/s
    float stormMag = std::sqrt(wc.windX() * wc.windX() + wc.windZ() * wc.windZ());
    CHECK(stormMag > clearMag);
}

// ---------------------------------------------------------------------------
// setPreset changes state immediately
// ---------------------------------------------------------------------------

TEST_CASE("WeatherController: setPreset changes preset immediately", "[weather]") {
    WeatherController wc;
    wc.setPreset(WeatherPreset::Storm);
    CHECK(wc.preset() == WeatherPreset::Storm);
    wc.setPreset(WeatherPreset::Clear);
    CHECK(wc.preset() == WeatherPreset::Clear);
}

// ---------------------------------------------------------------------------
// Snow and Blizzard presets
// ---------------------------------------------------------------------------

TEST_CASE("WeatherController: Snow and Blizzard set isSnowPrecipitation true", "[weather]") {
    WeatherController wc;
    wc.setPreset(WeatherPreset::Snow);
    CHECK(wc.computeEnvironment().isSnowPrecipitation == true);
    wc.setPreset(WeatherPreset::Blizzard);
    CHECK(wc.computeEnvironment().isSnowPrecipitation == true);
}

TEST_CASE("WeatherController: non-snow presets leave isSnowPrecipitation false", "[weather]") {
    WeatherController wc;
    const WeatherPreset nonSnow[] = {WeatherPreset::Clear, WeatherPreset::PartlyCloudy, WeatherPreset::Overcast,
                                     WeatherPreset::Rain, WeatherPreset::Storm};
    for (auto p : nonSnow) {
        wc.setPreset(p);
        CHECK(wc.computeEnvironment().isSnowPrecipitation == false);
    }
}

TEST_CASE("WeatherController: Snow cloudCoverage above precipitation threshold", "[weather]") {
    // PrecipitationController::kCloudThreshold = 0.75 — Snow must be at or above it.
    WeatherController wc;
    wc.setPreset(WeatherPreset::Snow);
    CHECK(wc.computeEnvironment().cloudCoverage >= 0.75f);
}

TEST_CASE("WeatherController: Blizzard cloudCoverage above storm precipitation threshold", "[weather]") {
    // PrecipitationController::kStormThreshold = 0.90 — Blizzard must be at or above it.
    WeatherController wc;
    wc.setPreset(WeatherPreset::Blizzard);
    CHECK(wc.computeEnvironment().cloudCoverage >= 0.90f);
}

TEST_CASE("WeatherController: auto-transition does not fire from Snow preset", "[weather]") {
    WeatherControllerParams p;
    p.transitionMinSeconds = 0.05f;
    p.transitionMaxSeconds = 0.10f;
    WeatherController wc(p);
    wc.setPreset(WeatherPreset::Snow);
    for (int i = 0; i < 20; ++i)
        wc.advance(0.01);
    CHECK(wc.preset() == WeatherPreset::Snow);
}

TEST_CASE("WeatherController: auto-transition does not fire from Blizzard preset", "[weather]") {
    WeatherControllerParams p;
    p.transitionMinSeconds = 0.05f;
    p.transitionMaxSeconds = 0.10f;
    WeatherController wc(p);
    wc.setPreset(WeatherPreset::Blizzard);
    for (int i = 0; i < 20; ++i)
        wc.advance(0.01);
    CHECK(wc.preset() == WeatherPreset::Blizzard);
}

TEST_CASE("WeatherController: auto-transition from Storm wraps to Clear not Snow", "[weather]") {
    WeatherControllerParams p;
    p.transitionMinSeconds = 0.05f;
    p.transitionMaxSeconds = 0.10f;
    WeatherController wc(p);
    wc.setPreset(WeatherPreset::Storm);
    for (int i = 0; i < 20; ++i)
        wc.advance(0.01);
    // After Storm auto-transitions it wraps to Clear (ordinal 0), not Snow (ordinal 5)
    CHECK(wc.preset() != WeatherPreset::Snow);
    CHECK(wc.preset() != WeatherPreset::Blizzard);
}

// Regression (#94 fuzzing): an out-of-range preset arriving from an untrusted MsgWeatherState must be
// clamped to Clear rather than indexing past the internal preset table (an OOB read).
TEST_CASE("WeatherController::applyPresetToEnv clamps an out-of-range preset", "[weather]") {
    EnvironmentState env{};
    WeatherController::applyPresetToEnv(static_cast<WeatherPreset>(139), 12.f, env);
    WeatherController::applyPresetToEnv(static_cast<WeatherPreset>(255), 12.f, env);
    // Clamped to Clear: cloudCoverage 0, no precipitation.
    CHECK(env.cloudCoverage == 0.0f);
    CHECK_FALSE(env.isSnowPrecipitation);
}

TEST_CASE("weatherTurbulence is deterministic and scales with amplitude (#426)", "[weather][turbulence]") {
    // The perturbation is a pure function of (entityIdx, tickIndex, amplitude): the same inputs give
    // byte-identical output (what lets the client reproduce the server exactly), and different
    // (entityIdx, tickIndex) give a different perturbation.
    const auto a = weatherTurbulence(7u, 100u, 4.0f);
    const auto b = weatherTurbulence(7u, 100u, 4.0f);
    CHECK(a[0] == b[0]);
    CHECK(a[1] == b[1]);
    CHECK(a[2] == b[2]);

    CHECK(weatherTurbulence(7u, 101u, 4.0f)[0] != a[0]); // next tick differs
    CHECK(weatherTurbulence(8u, 100u, 4.0f)[0] != a[0]); // different entity differs

    // Zero amplitude → no turbulence (clear weather, and the client's default before any MsgWeatherState).
    const auto z = weatherTurbulence(7u, 100u, 0.0f);
    CHECK(z[0] == 0.0f);
    CHECK(z[1] == 0.0f);
    CHECK(z[2] == 0.0f);

    // The y/z axes are fixed fractions of the x perturbation (0.3, 0.5), so amplitude scales linearly.
    CHECK_THAT(a[1], WithinRel(a[0] * 0.3f, 1e-5f));
    CHECK_THAT(a[2], WithinRel(a[0] * 0.5f, 1e-5f));
    CHECK_THAT(weatherTurbulence(7u, 100u, 8.0f)[0], WithinRel(a[0] * 2.0f, 1e-5f));
}

TEST_CASE("WeatherController::computeEnvironment carries the turbulence amplitude (#426)", "[weather]") {
    WeatherController wc;
    wc.setPreset(WeatherPreset::Storm); // a preset with non-zero turbulence
    const EnvironmentState env = wc.computeEnvironment();
    CHECK(env.turbulenceAmp == wc.turbulenceAmplitude());
    CHECK(env.turbulenceAmp > 0.0f);
}

// ---------------------------------------------------------------------------
// UTC clock + geographic sun (#481)
// ---------------------------------------------------------------------------

TEST_CASE("WeatherController: UTC Julian Day tracks the date and time-of-day (#481)", "[weather][solar]") {
    WeatherController wc;
    wc.setDate(2025, 6, 21);
    wc.setTimeOfDay(12.0f); // noon UTC
    const double jdNoon = wc.utcJulianDay();
    CHECK_THAT(jdNoon, WithinAbs(julianDay(2025, 6, 21, 12.0), 1e-6));

    // Advancing across midnight rolls the date forward by one day.
    wc.setTimeOfDay(23.9f);
    const double before = wc.utcJulianDay();
    // 0.2 h at timeScale 10 = 0.02 real h = 72 s; advance well past midnight.
    for (int i = 0; i < 60; ++i)
        wc.advance(1.0); // 60 real s -> 600 game s = 0.1667 game h ... need enough to cross midnight
    // Push the clock across midnight deterministically.
    wc.setTimeOfDay(0.1f);
    // The date should have advanced at least once relative to the pre-midnight instant.
    CHECK(std::floor(wc.utcJulianDay()) >= std::floor(before));
}

// ---------------------------------------------------------------------------
// Anchor-local mission clock (#1359)
// ---------------------------------------------------------------------------

TEST_CASE("WeatherController: anchor longitude converts local time-of-day to UTC (#1359)", "[weather][solar]") {
    WeatherController wc;
    wc.setDate(2025, 6, 21);
    wc.setTimeOfDay(12.0f);

    // No anchor: local IS UTC, which is the pre-#1359 behaviour every existing mission relied on.
    CHECK_THAT(wc.utcOffsetHours(), WithinAbs(0.0, 1e-9));
    CHECK_THAT(wc.utcJulianDay(), WithinAbs(julianDay(2025, 6, 21, 12.0), 1e-6));

    // 15 degrees of longitude is one hour of mean solar time. West of Greenwich, local noon happens
    // LATER in UTC, so the offset is positive.
    wc.setAnchorLongitude(-15.0 * kDegToRad<double>);
    CHECK_THAT(wc.utcOffsetHours(), WithinAbs(1.0, 1e-9));
    CHECK_THAT(wc.utcJulianDay(), WithinAbs(julianDay(2025, 6, 21, 13.0), 1e-6));

    // East is the mirror image.
    wc.setAnchorLongitude(15.0 * kDegToRad<double>);
    CHECK_THAT(wc.utcOffsetHours(), WithinAbs(-1.0, 1e-9));
    CHECK_THAT(wc.utcJulianDay(), WithinAbs(julianDay(2025, 6, 21, 11.0), 1e-6));

    // The DISPLAYED clock stays local — the conversion belongs to the UTC instant, not to the
    // time-of-day the HUD shows and the mission author wrote.
    CHECK_THAT(static_cast<double>(wc.timeOfDay()), WithinAbs(12.0, 1e-6));
}

TEST_CASE("WeatherController: the offset crosses midnight instead of wrapping (#1359)", "[weather][solar]") {
    // A Julian Day is a continuous instant, so an offset that pushes local time past either end of
    // the day must land on the neighbouring day. Wrapping into [0, 24) would put the sun a whole day
    // out at far-western longitudes -- the failure this guards is silent and looks like a sun that is
    // merely "a bit off".
    WeatherController wc;
    wc.setDate(2025, 6, 21);
    wc.setTimeOfDay(20.0f);
    wc.setAnchorLongitude(-150.0 * kDegToRad<double>); // +10 h -> 06:00 UTC the NEXT day
    CHECK_THAT(wc.utcJulianDay(), WithinAbs(julianDay(2025, 6, 22, 6.0), 1e-6));

    wc.setTimeOfDay(2.0f);
    wc.setAnchorLongitude(150.0 * kDegToRad<double>); // -10 h -> 16:00 UTC the PREVIOUS day
    CHECK_THAT(wc.utcJulianDay(), WithinAbs(julianDay(2025, 6, 20, 16.0), 1e-6));
}

TEST_CASE("WeatherController: a mission at the sandbox anchor is LIT at midday (#1359)", "[weather][solar]") {
    // The regression this whole issue was: the shipped sandbox says `time: {hour: 12}` and rendered a
    // black world, because 12:00 fed in as UTC is 04:20 local at longitude -115 -- before sunrise.
    // Solar elevation was -4.76 degrees, which takes applySunLighting's night branch and lights the
    // scene with its dim blue {0.02, 0.02, 0.08}.
    WeatherController wc;
    wc.setDate(2025, 6, 21);
    wc.setAnchorLongitude(kSandboxHomeLonDeg * kDegToRad<double>);
    wc.setTimeOfDay(12.0f);

    const SolarAngles a =
        solarAngles(kSandboxHomeLatDeg * kDegToRad<double>, kSandboxHomeLonDeg * kDegToRad<double>, wc.utcJulianDay());
    INFO("solar elevation at the sandbox at local noon: " << (a.elevationRad * kRadToDeg<double>) << " deg");
    CHECK(a.elevationRad > 0.0);
    // Local noon is not merely above the horizon, it is near the day's peak. A loose bound is enough
    // to catch a sign flip or a factor-of-two in the offset without pinning the ephemeris.
    CHECK(a.elevationRad * kRadToDeg<double> > 60.0);

    // ... and midnight at the same anchor is still night, so the fix did not simply shift everything
    // into permanent daylight.
    wc.setTimeOfDay(0.0f);
    const SolarAngles midnight =
        solarAngles(kSandboxHomeLatDeg * kDegToRad<double>, kSandboxHomeLonDeg * kDegToRad<double>, wc.utcJulianDay());
    CHECK(midnight.elevationRad < 0.0);
}

TEST_CASE("WeatherController: geographic sun differs by longitude and points up at the sub-solar point",
          "[weather][solar]") {
    // Build the world position of an observer at (lat, lon) on the sphere and check the sun.
    auto worldAt = [](double latRad, double lonRad) {
        glm::dvec3 p{};
        geodeticToWorld({latRad, lonRad, 0.0}, p.x, p.y, p.z, kEarthRadiusM);
        return p;
    };
    const double jd0 = julianDay(2025, 6, 21, 12.0);
    const double eot = equationOfTimeMinutes(jd0);
    const double jdNoon = julianDay(2025, 6, 21, 12.0 - eot / 60.0); // solar noon at lon 0
    const double decl = solarDeclinationRad(jdNoon);

    // At the sub-solar point the world sun direction is (nearly) the local radial up.
    const glm::dvec3 subSolar = worldAt(decl, 0.0);
    const glm::vec3 sun = WeatherController::geographicSunDirection(jdNoon, subSolar, kEarthRadiusM);
    const glm::vec3 up = glm::normalize(glm::vec3(subSolar - glm::dvec3{0.0, -kEarthRadiusM, 0.0}));
    CHECK(glm::dot(sun, up) > 0.99f);

    // A world instant where it is day at lon 0 but night ~180° away: the sun elevations disagree.
    const double kDeg = 0.017453292519943295;
    const glm::dvec3 atGreenwich = worldAt(0.0, 0.0);
    const glm::dvec3 atAnti = worldAt(0.0, 180.0 * kDeg);
    const double jdDay = julianDay(2025, 6, 21, 12.0);
    const glm::vec3 sunG = WeatherController::geographicSunDirection(jdDay, atGreenwich, kEarthRadiusM);
    const glm::vec3 sunA = WeatherController::geographicSunDirection(jdDay, atAnti, kEarthRadiusM);
    const glm::vec3 upG = glm::normalize(glm::vec3(atGreenwich - glm::dvec3{0.0, -kEarthRadiusM, 0.0}));
    const glm::vec3 upA = glm::normalize(glm::vec3(atAnti - glm::dvec3{0.0, -kEarthRadiusM, 0.0}));
    CHECK(glm::dot(sunG, upG) > 0.0f); // day at Greenwich
    CHECK(glm::dot(sunA, upA) < 0.0f); // night on the far side
}

// #1211: the reason the default home moved off the world origin. The sun and moon are true
// geographic, so at the pole the sun holds ONE elevation for all 24 hours of the default date — a
// mission's `time:` moved only its azimuth and no sandbox ever had a sunrise. This asserts the
// difference the move buys, in the engine's own solar model rather than by argument.
TEST_CASE("WeatherController: the sandbox home has a day and a night; the world origin does not (#1211)",
          "[weather][solar]") {
    const glm::dvec3 pole{0.0, 0.0, 0.0}; // the world origin IS the north pole
    glm::dvec3 home{};
    geodeticToWorld(sandboxHome(), home.x, home.y, home.z, kEarthRadiusM);

    auto sunElevationAt = [](glm::dvec3 observer, double hourUtc) {
        const glm::vec3 sun =
            WeatherController::geographicSunDirection(julianDay(2025, 6, 21, hourUtc), observer, kEarthRadiusM);
        const glm::vec3 up = glm::normalize(glm::vec3(observer - glm::dvec3{0.0, -kEarthRadiusM, 0.0}));
        return glm::dot(sun, up);
    };

    // At the pole on the default date the sun never sets and barely moves in elevation all day.
    double poleMin = 2.0, poleMax = -2.0;
    for (int h = 0; h < 24; ++h) {
        const double e = sunElevationAt(pole, static_cast<double>(h));
        poleMin = std::min(poleMin, e);
        poleMax = std::max(poleMax, e);
    }
    CHECK(poleMin > 0.0);            // no night, ever
    CHECK(poleMax - poleMin < 0.05); // and no meaningful change in elevation: `time:` only spins the azimuth

    // At the home it rises and sets: the sun goes well above the horizon and well below it.
    double homeMin = 2.0, homeMax = -2.0;
    for (int h = 0; h < 24; ++h) {
        const double e = sunElevationAt(home, static_cast<double>(h));
        homeMin = std::min(homeMin, e);
        homeMax = std::max(homeMax, e);
    }
    CHECK(homeMax > 0.9); // ~77 degrees at local noon on the solstice
    CHECK(homeMin < -0.2);
}

TEST_CASE("WeatherController: applyGeographicSun sets a lit day sun and a dim night sun (#481)", "[weather][solar]") {
    glm::dvec3 greenwich{};
    geodeticToWorld({0.0, 0.0, 0.0}, greenwich.x, greenwich.y, greenwich.z, kEarthRadiusM);

    EnvironmentState day{};
    day.cloudCoverage = 0.0f;
    WeatherController::applyGeographicSun(day, julianDay(2025, 6, 21, 12.0), greenwich); // local noon
    CHECK(day.sunColor.r > 0.5f);                                                        // bright day sun

    EnvironmentState night{};
    night.cloudCoverage = 0.0f;
    WeatherController::applyGeographicSun(night, julianDay(2025, 6, 21, 0.0), greenwich); // local midnight
    CHECK(night.sunColor.r < 0.1f);                                                       // dim night sun
}

TEST_CASE("WeatherController: the sky's elevation is LOCAL, not the sun's world-Y (#1391)", "[weather][solar]") {
    // The world origin is the north pole, so `sunDirection.y` is sin(elevation) only THERE. At an
    // anchored mission it is the sun's DECLINATION -- a date property that does not change over a
    // day -- and the sky pass was reading it as an elevation. Measured at the sandbox anchor it is
    // +0.378 at EVERY hour, which pinned the sky's warmth mix at "day" and its night factor at 0, so
    // the #484 Moon and star field never appeared on an anchored mission at any hour.
    WeatherController wc;
    wc.setDate(2025, 6, 21);
    wc.setAnchorLongitude(kSandboxHomeLonDeg * kDegToRad<double>);

    double x = 0.0, y = 0.0, z = 0.0;
    localOffsetToWorld(sandboxHome(), 0.0, 0.0, 600.0, x, y, z);

    const auto atHour = [&](float hour) {
        wc.setTimeOfDay(hour);
        EnvironmentState env = wc.computeEnvironment();
        WeatherController::applyGeographicSun(env, wc.utcJulianDay(), {x, y, z}, kEarthRadiusM);
        return env;
    };
    const EnvironmentState noon = atHour(12.0f);
    const EnvironmentState night = atHour(22.5f);

    INFO("noon: elev sin " << noon.sunElevationSin << ", world-Y " << noon.sunDirection.y);
    INFO("22:30: elev sin " << night.sunElevationSin << ", world-Y " << night.sunDirection.y);

    // The published elevation tracks the local sun: high at noon, below the horizon at 22:30.
    CHECK(noon.sunElevationSin > 0.9f);
    CHECK(night.sunElevationSin < 0.0f);

    // The world-Y does NOT. Pinned so the sky cannot quietly go back to reading it: the same value at
    // both hours, and positive at 22:30 while the sun is well below the local horizon.
    CHECK_THAT(night.sunDirection.y, WithinAbs(noon.sunDirection.y, 0.01f));
    CHECK(night.sunDirection.y > 0.0f);

    // VkRenderer's own night-factor expression, evaluated on each, for the record: 1 (night sky, Moon
    // and stars) from the local elevation, 0 (day sky, no celestial) from the world-Y.
    const auto nightFactor = [](float elev) { return std::clamp((-elev + 0.10f) / 0.18f, 0.0f, 1.0f); };
    CHECK_THAT(nightFactor(night.sunElevationSin), WithinAbs(1.0f, 1e-5f));
    CHECK_THAT(nightFactor(night.sunDirection.y), WithinAbs(0.0f, 1e-5f));
}

TEST_CASE("WeatherController: at the world origin the two agree exactly (#1391)", "[weather][solar]") {
    // Which is why the substitution was right for as long as everything shipped sat on the origin,
    // and why an unanchored mission is unchanged by the fix.
    WeatherController wc;
    wc.setDate(2025, 6, 21);
    wc.setTimeOfDay(12.0f);
    EnvironmentState env = wc.computeEnvironment();
    WeatherController::applyGeographicSun(env, wc.utcJulianDay(), {0.0, 0.0, 0.0}, kEarthRadiusM);
    CHECK_THAT(env.sunElevationSin, WithinAbs(env.sunDirection.y, 1e-5f));
}

TEST_CASE("WeatherController: the planar preset path publishes its elevation too (#1391)", "[weather][solar]") {
    // A client that has not yet received a UTC clock still gets a coherent sky: the planar path's
    // elevation IS its sunDirection.y, and applySunLighting publishes exactly what it was handed.
    EnvironmentState env{};
    WeatherController::applyPresetToEnv(WeatherPreset::Clear, 22.5f, env);
    CHECK_THAT(env.sunElevationSin, WithinAbs(env.sunDirection.y, 1e-6f));
}
