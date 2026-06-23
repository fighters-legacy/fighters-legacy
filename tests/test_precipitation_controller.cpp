// SPDX-License-Identifier: GPL-3.0-or-later
#include "PrecipitationController.h"
#include "render/ParticleSystem.h"

#include <catch2/catch_test_macros.hpp>

using namespace fl;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static fl::ParticleSystem makePs() {
    fl::ParticleSystem ps;
    fl::ParticlePreset base{};
    base.spawnRate = 100.f;
    base.particleLifetime = 2.f;
    ps.registerPreset("rain", base);
    ps.registerPreset("storm_rain", base);
    ps.registerPreset("snow", base);
    ps.registerPreset("storm_snow", base);
    return ps;
}

static EnvironmentState makeEnv(float cloudCoverage, bool isSnow = false) {
    EnvironmentState env{};
    env.cloudCoverage = cloudCoverage;
    env.isSnowPrecipitation = isSnow;
    return env;
}

static CameraView makeCam() {
    return CameraView{};
}

// ---------------------------------------------------------------------------
// Cloud coverage threshold
// ---------------------------------------------------------------------------

TEST_CASE("PrecipitationController: no emitters below cloud threshold", "[precipitation]") {
    PrecipitationController pc;
    auto ps = makePs();
    CHECK(pc.build(makeEnv(0.0f), makeCam(), ps).empty());
    CHECK(pc.build(makeEnv(0.74f), makeCam(), ps).empty());
}

TEST_CASE("PrecipitationController: emitters produced at cloud threshold", "[precipitation]") {
    PrecipitationController pc;
    auto ps = makePs();
    CHECK(!pc.build(makeEnv(0.75f), makeCam(), ps).empty());
}

TEST_CASE("PrecipitationController: 9 emitters produced when active", "[precipitation]") {
    PrecipitationController pc;
    auto ps = makePs();
    CHECK(pc.build(makeEnv(0.85f), makeCam(), ps).size() == 9);
}

// ---------------------------------------------------------------------------
// Precipitation type selection
// ---------------------------------------------------------------------------

TEST_CASE("PrecipitationController: isSnowPrecipitation=false selects rain preset", "[precipitation]") {
    PrecipitationController pc;
    auto ps = makePs();
    const auto emitters = pc.build(makeEnv(0.85f, /*isSnow=*/false), makeCam(), ps);
    REQUIRE(emitters.size() == 9);
    for (const auto& e : emitters)
        CHECK(std::string_view(e.effectName) == "rain");
}

TEST_CASE("PrecipitationController: isSnowPrecipitation=true selects snow preset", "[precipitation]") {
    PrecipitationController pc;
    auto ps = makePs();
    const auto emitters = pc.build(makeEnv(0.85f, /*isSnow=*/true), makeCam(), ps);
    REQUIRE(emitters.size() == 9);
    for (const auto& e : emitters)
        CHECK(std::string_view(e.effectName) == "snow");
}

TEST_CASE("PrecipitationController: storm coverage + no snow selects storm_rain", "[precipitation]") {
    PrecipitationController pc;
    auto ps = makePs();
    const auto emitters = pc.build(makeEnv(0.95f, /*isSnow=*/false), makeCam(), ps);
    REQUIRE(emitters.size() == 9);
    for (const auto& e : emitters)
        CHECK(std::string_view(e.effectName) == "storm_rain");
}

TEST_CASE("PrecipitationController: storm coverage + snow selects storm_snow", "[precipitation]") {
    PrecipitationController pc;
    auto ps = makePs();
    const auto emitters = pc.build(makeEnv(0.95f, /*isSnow=*/true), makeCam(), ps);
    REQUIRE(emitters.size() == 9);
    for (const auto& e : emitters)
        CHECK(std::string_view(e.effectName) == "storm_snow");
}

// ---------------------------------------------------------------------------
// Missing preset graceful fallback
// ---------------------------------------------------------------------------

TEST_CASE("PrecipitationController: unregistered snow preset returns empty span", "[precipitation]") {
    PrecipitationController pc;
    fl::ParticleSystem emptyPs; // no presets registered
    CHECK(pc.build(makeEnv(0.85f, /*isSnow=*/true), makeCam(), emptyPs).empty());
}
