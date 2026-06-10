// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/WindshieldRain.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

static EnvironmentState makeEnv(float cloudCoverage, float windX = 0.0f, float windZ = 0.0f) {
    EnvironmentState env{};
    env.cloudCoverage = cloudCoverage;
    env.windX = windX;
    env.windZ = windZ;
    return env;
}

TEST_CASE("WindshieldRain: no elements below threshold") {
    fl::WindshieldRain wr;
    wr.update(1.0f / 60.0f, makeEnv(0.0f));
    CHECK(wr.elements().empty());
    wr.update(1.0f / 60.0f, makeEnv(0.74f));
    CHECK(wr.elements().empty());
}

TEST_CASE("WindshieldRain: elements produced at threshold") {
    fl::WindshieldRain wr;
    wr.update(1.0f / 60.0f, makeEnv(0.75f));
    CHECK(!wr.elements().empty());
}

TEST_CASE("WindshieldRain: all elements are Line type") {
    fl::WindshieldRain wr;
    wr.update(1.0f / 60.0f, makeEnv(0.85f));
    for (const auto& el : wr.elements())
        CHECK(el.type == HudElement::Type::Line);
}

TEST_CASE("WindshieldRain: rain elements are semi-transparent") {
    fl::WindshieldRain wr;
    wr.update(1.0f / 60.0f, makeEnv(0.85f));
    for (const auto& el : wr.elements())
        CHECK(el.a < 0.5f);
}

TEST_CASE("WindshieldRain: storm alpha greater than rain alpha") {
    fl::WindshieldRain rain, storm;
    rain.update(1.0f / 60.0f, makeEnv(0.75f));
    storm.update(1.0f / 60.0f, makeEnv(0.95f));
    REQUIRE(!rain.elements().empty());
    REQUIRE(!storm.elements().empty());
    CHECK(storm.elements()[0].a > rain.elements()[0].a);
}

TEST_CASE("WindshieldRain: storm streak longer than rain streak") {
    fl::WindshieldRain rain, storm;
    rain.update(1.0f / 60.0f, makeEnv(0.75f));
    storm.update(1.0f / 60.0f, makeEnv(0.95f));
    REQUIRE(!rain.elements().empty());
    REQUIRE(!storm.elements().empty());
    const float rainLen = rain.elements()[0].y2 - rain.elements()[0].y;
    const float stormLen = storm.elements()[0].y2 - storm.elements()[0].y;
    CHECK(stormLen > rainLen);
}

TEST_CASE("WindshieldRain: positive windX tilts streaks right") {
    fl::WindshieldRain rain, snow;
    const auto env = makeEnv(0.85f, /*windX=*/15.0f);
    rain.update(1.0f / 60.0f, env, 0.f, false);
    snow.update(1.0f / 60.0f, env, 0.f, true);
    for (const auto& el : rain.elements())
        CHECK(el.x2 > el.x);
    for (const auto& el : snow.elements())
        CHECK(el.x2 > el.x);
}

TEST_CASE("WindshieldRain: negative windX tilts streaks left") {
    fl::WindshieldRain wr;
    wr.update(1.0f / 60.0f, makeEnv(0.85f, /*windX=*/-15.0f));
    for (const auto& el : wr.elements())
        CHECK(el.x2 < el.x);
}

TEST_CASE("WindshieldRain: zero windX gives vertical streaks") {
    fl::WindshieldRain wr;
    wr.update(1.0f / 60.0f, makeEnv(0.85f, /*windX=*/0.0f));
    for (const auto& el : wr.elements())
        CHECK(el.x2 == Catch::Approx(el.x).margin(1e-5f));
}

TEST_CASE("WindshieldRain: time advancement moves drop positions") {
    fl::WindshieldRain wr;
    const auto env = makeEnv(0.85f);
    wr.update(1.0f / 60.0f, env);
    const float firstY = wr.elements()[0].y;
    wr.update(1.0f / 60.0f, env);
    const float secondY = wr.elements()[0].y;
    CHECK(secondY != Catch::Approx(firstY));
}

TEST_CASE("WindshieldRain: element count equals kDropCount when active") {
    fl::WindshieldRain wr;
    wr.update(1.0f / 60.0f, makeEnv(0.85f));
    CHECK(wr.elements().size() == 48);
}

TEST_CASE("WindshieldRain: clears when coverage drops below threshold") {
    fl::WindshieldRain wr;
    wr.update(1.0f / 60.0f, makeEnv(0.85f));
    REQUIRE(!wr.elements().empty());
    wr.update(1.0f / 60.0f, makeEnv(0.50f));
    CHECK(wr.elements().empty());
}

TEST_CASE("WindshieldRain: snow streaks shorter than rain streaks") {
    fl::WindshieldRain rain, snow;
    const auto env = makeEnv(0.85f);
    rain.update(1.0f / 60.0f, env, 0.f, false);
    snow.update(1.0f / 60.0f, env, 0.f, true);
    REQUIRE(!rain.elements().empty());
    REQUIRE(!snow.elements().empty());
    const float rainLen = rain.elements()[0].y2 - rain.elements()[0].y;
    const float snowLen = snow.elements()[0].y2 - snow.elements()[0].y;
    CHECK(snowLen < rainLen);
}

TEST_CASE("WindshieldRain: snow elements are white") {
    fl::WindshieldRain wr;
    wr.update(1.0f / 60.0f, makeEnv(0.85f), 0.f, true);
    for (const auto& el : wr.elements()) {
        CHECK(el.r == Catch::Approx(1.0f));
        CHECK(el.g == Catch::Approx(1.0f));
        CHECK(el.b == Catch::Approx(1.0f));
    }
}

TEST_CASE("WindshieldRain: rain elements are blue-tinted") {
    fl::WindshieldRain wr;
    wr.update(1.0f / 60.0f, makeEnv(0.85f), 0.f, false);
    for (const auto& el : wr.elements()) {
        CHECK(el.b > el.r);
        CHECK(el.b > el.g);
    }
}

TEST_CASE("WindshieldRain: rain and snow positions differ") {
    fl::WindshieldRain rain, snow;
    const auto env = makeEnv(0.85f);
    rain.update(1.0f / 60.0f, env, 0.f, false);
    snow.update(1.0f / 60.0f, env, 0.f, true);
    REQUIRE(rain.elements().size() == snow.elements().size());
    bool anyDiffers = false;
    for (std::size_t i = 0; i < rain.elements().size(); ++i) {
        if (rain.elements()[i].y != Catch::Approx(snow.elements()[i].y).margin(1e-5f)) {
            anyDiffers = true;
            break;
        }
    }
    CHECK(anyDiffers);
}

TEST_CASE("WindshieldRain: snow also tilts with crosswind") {
    fl::WindshieldRain wr;
    wr.update(1.0f / 60.0f, makeEnv(0.85f, /*windX=*/15.0f), 0.f, true);
    for (const auto& el : wr.elements())
        CHECK(el.x2 > el.x);
}

TEST_CASE("WindshieldRain: windZ only does not tilt streaks") {
    fl::WindshieldRain wr;
    wr.update(1.0f / 60.0f, makeEnv(0.85f, /*windX=*/0.0f, /*windZ=*/15.0f));
    for (const auto& el : wr.elements())
        CHECK(el.x2 == Catch::Approx(el.x).margin(1e-5f));
}

TEST_CASE("WindshieldRain: dt zero still produces elements") {
    fl::WindshieldRain wr;
    wr.update(0.0f, makeEnv(0.85f));
    CHECK(!wr.elements().empty());
}

TEST_CASE("WindshieldRain: 90 degree right roll rotates streaks to horizontal") {
    constexpr float kPi = 3.14159265f;
    fl::WindshieldRain wr;
    wr.update(1.0f / 60.0f, makeEnv(0.85f, /*windX=*/0.0f), kPi / 2.0f);
    for (const auto& el : wr.elements()) {
        CHECK(el.y2 == Catch::Approx(el.y).margin(1e-4f));
        CHECK(el.x2 > el.x);
    }
}

TEST_CASE("WindshieldRain: snow stroke width wider than rain") {
    fl::WindshieldRain rain, snow;
    const auto env = makeEnv(0.85f);
    rain.update(1.0f / 60.0f, env, 0.f, false);
    snow.update(1.0f / 60.0f, env, 0.f, true);
    REQUIRE(!rain.elements().empty());
    REQUIRE(!snow.elements().empty());
    CHECK(snow.elements()[0].strokeWidth > rain.elements()[0].strokeWidth);
}
