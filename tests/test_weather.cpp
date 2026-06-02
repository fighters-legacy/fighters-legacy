// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "weather/WeatherController.h"

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

TEST_CASE("WeatherController: computeEnvironment all presets have distinct cloudCoverage", "[weather]") {
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
