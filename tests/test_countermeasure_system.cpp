// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "weapon/CountermeasureSystem.h"

using namespace fl;

namespace {

CountermeasureSusceptibility susc(float chaff, float flare) {
    CountermeasureSusceptibility s;
    s.chaff = chaff;
    s.flare = flare;
    return s;
}

} // namespace

TEST_CASE("CountermeasureSystem: dispense decrements the magazine and puts decoys in the air", "[countermeasure]") {
    CountermeasureSystem cm;
    cm.registerDispenser(5, /*chaff=*/3, /*flare=*/3);

    CHECK(cm.chaffRemaining(5) == 3u);
    CHECK(cm.flareRemaining(5) == 3u);

    const glm::dvec3 pos{100.0, 200.0, 300.0};
    const glm::vec3 vel{250.f, 0.f, 0.f};
    CHECK(cm.dispense(5, pos, vel, /*tick=*/1));
    // One pop releases one chaff + one flare (a combined program).
    CHECK(cm.chaffRemaining(5) == 2u);
    CHECK(cm.flareRemaining(5) == 2u);
    CHECK(cm.liveDecoyCount() == 2u);

    // An unregistered dispenser does nothing.
    CHECK_FALSE(cm.dispense(99, pos, vel, 1));
}

TEST_CASE("CountermeasureSystem: an empty magazine releases nothing", "[countermeasure]") {
    CountermeasureSystem cm;
    cm.registerDispenser(1, /*chaff=*/0, /*flare=*/1);
    const glm::dvec3 pos{};
    const glm::vec3 vel{};
    CHECK(cm.dispense(1, pos, vel, 1));       // the flare pops...
    CHECK_FALSE(cm.dispense(1, pos, vel, 2)); // ...and now both magazines are empty
    CHECK(cm.liveDecoyCount() == 1u);
}

TEST_CASE("CountermeasureSystem: a flare seduces an IR seeker but not a radar one", "[countermeasure]") {
    CountermeasureSystem cm;
    cm.registerDispenser(1, /*chaff=*/0, /*flare=*/1);
    const glm::dvec3 targetPos{0.0, 0.0, 0.0};
    cm.dispense(1, targetPos, glm::vec3{}, /*tick=*/0); // a flare right on the target

    // An IR seeker with a high flare susceptibility is seduced...
    CHECK(cm.seduces(/*missileIdx=*/7, targetPos, sensor::SensorType::Ir, susc(0.f, 1.f), /*tick=*/1));
    // ...but a radar seeker is not fooled by a flare (wrong channel).
    CHECK_FALSE(cm.seduces(7, targetPos, sensor::SensorType::Radar, susc(1.f, 1.f), 1));
    // A flare-immune IR head (susceptibility 0) is never seduced.
    CHECK_FALSE(cm.seduces(7, targetPos, sensor::SensorType::Ir, susc(0.f, 0.f), 1));
}

TEST_CASE("CountermeasureSystem: a decoy only seduces while it is near the target", "[countermeasure]") {
    CountermeasureSystem cm;
    cm.registerDispenser(1, /*chaff=*/1, /*flare=*/0);
    const glm::dvec3 dropPos{0.0, 0.0, 0.0};
    cm.dispense(1, dropPos, glm::vec3{}, 0); // chaff at the origin

    // The target is now far from where the chaff was dropped: no seduction.
    const glm::dvec3 farTarget{5000.0, 0.0, 0.0};
    CHECK_FALSE(cm.seduces(7, farTarget, sensor::SensorType::Radar, susc(1.f, 0.f), 1));
    // But a target still near the cloud is decoyed.
    const glm::dvec3 nearTarget{50.0, 0.0, 0.0};
    CHECK(cm.seduces(7, nearTarget, sensor::SensorType::Radar, susc(1.f, 0.f), 1));
}

TEST_CASE("CountermeasureSystem: decoys expire and stop seducing", "[countermeasure]") {
    CountermeasureSystem cm;
    cm.registerDispenser(1, /*chaff=*/0, /*flare=*/1);
    const glm::dvec3 targetPos{0.0, 0.0, 0.0};
    cm.dispense(1, targetPos, glm::vec3{}, /*tick=*/0);
    REQUIRE(cm.liveDecoyCount() == 1u);

    // Age past the lifetime (~4 s at 60 Hz = 240 ticks). The decoy is gone.
    for (uint64_t t = 1; t <= 300; ++t)
        cm.onTick(1.0 / 60.0, t);
    CHECK(cm.liveDecoyCount() == 0u);
    CHECK_FALSE(cm.seduces(7, targetPos, sensor::SensorType::Ir, susc(0.f, 1.f), 301));
}

TEST_CASE("CountermeasureSystem: seduction is deterministic for a fixed seed", "[countermeasure]") {
    // A partial susceptibility means the roll matters; the same (missile, tick) must decide the same
    // way every time — a replay on any machine breaks the identical lock.
    CountermeasureSystem cm;
    cm.registerDispenser(1, /*chaff=*/0, /*flare=*/1);
    const glm::dvec3 targetPos{};
    cm.dispense(1, targetPos, glm::vec3{}, 0);

    const bool a = cm.seduces(42, targetPos, sensor::SensorType::Ir, susc(0.f, 0.5f), 99);
    const bool b = cm.seduces(42, targetPos, sensor::SensorType::Ir, susc(0.f, 0.5f), 99);
    CHECK(a == b);
}
