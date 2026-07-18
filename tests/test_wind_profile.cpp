// SPDX-License-Identifier: GPL-3.0-or-later
// Altitude wind profile tests (#489): the shared interp (WindProfile.h) + WeatherController authoring.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "weather/WeatherController.h"
#include "weather/WindProfile.h"

#include <vector>

using namespace fl;

namespace {
EnvironmentState envWithProfile() {
    EnvironmentState env;
    env.windX = 1.0f; // datum fallback
    env.windZ = 2.0f;
    env.windProfileCount = 3;
    env.windProfile[0] = {0.0f, 10.0f, 0.0f};    // surface: 10 m/s +X
    env.windProfile[1] = {1000.0f, 20.0f, 0.0f}; // 1 km: 20 m/s +X
    env.windProfile[2] = {5000.0f, 0.0f, 40.0f}; // 5 km: 40 m/s +Z
    return env;
}
} // namespace

TEST_CASE("windAtAltitude clamps below/above and interpolates between knots", "[wind][profile]") {
    const EnvironmentState env = envWithProfile();
    // Below the surface knot -> surface wind.
    CHECK(windAtAltitude(env, -500.0f).x == Catch::Approx(10.0f));
    // Above the top knot -> top wind.
    CHECK(windAtAltitude(env, 9000.0f).y == Catch::Approx(40.0f));
    // Halfway between knot 0 (10) and knot 1 (20) at 500 m -> 15.
    CHECK(windAtAltitude(env, 500.0f).x == Catch::Approx(15.0f));
    // Quarter between knot 1 (1 km, 20,0) and knot 2 (5 km, 0,40) at 2 km -> t=0.25.
    const glm::vec2 w = windAtAltitude(env, 2000.0f);
    CHECK(w.x == Catch::Approx(20.0f + (0.0f - 20.0f) * 0.25f));
    CHECK(w.y == Catch::Approx(0.0f + (40.0f - 0.0f) * 0.25f));
}

TEST_CASE("windAtAltitude falls back to the datum scalar with no profile", "[wind][profile]") {
    EnvironmentState env;
    env.windX = 3.5f;
    env.windZ = -1.5f;
    env.windProfileCount = 0;
    CHECK(windAtAltitude(env, 0.0f).x == Catch::Approx(3.5f));
    CHECK(windAtAltitude(env, 8000.0f).y == Catch::Approx(-1.5f));
}

TEST_CASE("WeatherController::setWindProfile converts met knots and sorts by altitude", "[wind][profile]") {
    WeatherController wc;
    // FROM 270 deg (west) blows toward +X. Provide out of order to check sorting.
    wc.setWindProfile({
        {2000.0f, 30.0f, 270.0f},
        {0.0f, 10.0f, 270.0f},
    });
    REQUIRE(wc.hasWindProfile());
    const EnvironmentState env = wc.computeEnvironment();
    REQUIRE(env.windProfileCount == 2);
    CHECK(env.windProfile[0].altM == Catch::Approx(0.0f)); // sorted ascending
    CHECK(env.windProfile[1].altM == Catch::Approx(2000.0f));
    // FROM-west wind blows +X; +Z component ~0 (gust folds along +X).
    CHECK(env.windProfile[0].windX > 9.0f);
    CHECK(env.windProfile[0].windZ == Catch::Approx(0.0f).margin(1e-3));
    // The surface knot becomes the datum windX/windZ (what an old client sees).
    CHECK(env.windX == Catch::Approx(env.windProfile[0].windX));
    CHECK(env.windZ == Catch::Approx(env.windProfile[0].windZ));
}

TEST_CASE("setWind clears a wind profile (mission flat wind wins)", "[wind][profile]") {
    WeatherController wc;
    wc.setWindProfile({{0.0f, 10.0f, 270.0f}, {3000.0f, 40.0f, 270.0f}});
    REQUIRE(wc.hasWindProfile());
    wc.setWind(90.0f, 5.0f);
    CHECK_FALSE(wc.hasWindProfile());
    CHECK(wc.computeEnvironment().windProfileCount == 0);
}

TEST_CASE("server and client agree on wind at altitude (parity)", "[wind][profile]") {
    // The server (WorldBroadcaster) and client (ClientPrediction) both call the SAME WindProfile.h
    // windAtAltitude on the broadcast EnvironmentState, so the value is bit-identical by construction.
    WeatherController wc;
    wc.setWindProfile({{0.0f, 12.0f, 240.0f}, {1500.0f, 25.0f, 260.0f}, {6000.0f, 55.0f, 280.0f}});
    const EnvironmentState env = wc.computeEnvironment();
    for (float alt : {-100.0f, 0.0f, 750.0f, 1500.0f, 3000.0f, 6000.0f, 12000.0f}) {
        const glm::vec2 a = windAtAltitude(env, alt);
        const glm::vec2 b = windAtAltitude(env, alt); // same inputs -> identical
        CHECK(a.x == b.x);
        CHECK(a.y == b.y);
    }
}
