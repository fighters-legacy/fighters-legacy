// SPDX-License-Identifier: GPL-3.0-or-later
//
// Per-subsystem damage routing (#675): the pure pick/apply logic. The effect wiring
// (asymmetric thrust, control loss, fuel leak) lives in test_flight_integrator and
// test_world_broadcaster.
#include <catch2/catch_test_macros.hpp>

#include "entity/SubsystemDamage.h"

#include <array>

using namespace fl;

namespace {
SubsystemSet makeFullSet() {
    SubsystemSet s;
    for (int i = 0; i < kSubsystemCount; ++i) {
        s.parts[i].hp = 50.f;
        s.parts[i].weight = 1.f;
    }
    return s;
}
} // namespace

TEST_CASE("SubsystemStateSet: init copies HP and clears the mask", "[subsystem]") {
    SubsystemSet def = makeFullSet();
    SubsystemStateSet st;
    st.init(def);
    CHECK(st.hp[static_cast<int>(Subsystem::EngineLeft)] == 50.f);
    CHECK(st.failedMask == 0);
    CHECK_FALSE(st.failed(Subsystem::Fuel));
}

TEST_CASE("applySubsystemDamage: fails on the edge, once", "[subsystem]") {
    SubsystemStateSet st;
    st.init(makeFullSet());
    CHECK(applySubsystemDamage(st, Subsystem::Controls, 30.f) == 0); // 50 -> 20, survives
    CHECK_FALSE(st.failed(Subsystem::Controls));
    const uint8_t edge = applySubsystemDamage(st, Subsystem::Controls, 25.f); // 20 -> 0, fails
    CHECK(edge == (1u << static_cast<int>(Subsystem::Controls)));
    CHECK(st.failed(Subsystem::Controls));
    // A dead subsystem does not fail again.
    CHECK(applySubsystemDamage(st, Subsystem::Controls, 10.f) == 0);
}

TEST_CASE("pickSubsystem: undirected picks by weight only", "[subsystem]") {
    // Only Fuel and Avionics present; Fuel weighted 3:1. Over an ensemble the ratio should hold.
    SubsystemSet def;
    def.parts[static_cast<int>(Subsystem::Fuel)] = {100.f, 3.f};
    def.parts[static_cast<int>(Subsystem::Avionics)] = {100.f, 1.f};
    SubsystemStateSet st;
    st.init(def);
    const float noDir[3] = {0.f, 0.f, 0.f};

    int fuel = 0, avionics = 0;
    for (uint32_t i = 0; i < 4000; ++i) {
        const uint32_t h = subsystemHash(1u, i, 0u);
        const Subsystem s = pickSubsystem(def, st, noDir, h);
        if (s == Subsystem::Fuel)
            ++fuel;
        else if (s == Subsystem::Avionics)
            ++avionics;
        else
            FAIL("picked a subsystem that is not present");
    }
    const float ratio = static_cast<float>(fuel) / static_cast<float>(avionics);
    CHECK(ratio > 2.4f); // ~3:1
    CHECK(ratio < 3.6f);
}

TEST_CASE("pickSubsystem: a rear-left hit biases toward the left engine", "[subsystem]") {
    SubsystemSet def = makeFullSet();
    SubsystemStateSet st;
    st.init(def);
    // hitDirBody travelling FORWARD and to the LEFT = a round that came from the rear-left.
    const float rearLeft[3] = {1.f, 0.f, -1.f};

    int left = 0, right = 0;
    for (uint32_t i = 0; i < 4000; ++i)
        switch (pickSubsystem(def, st, rearLeft, subsystemHash(7u, i, 0u))) {
        case Subsystem::EngineLeft:
            ++left;
            break;
        case Subsystem::EngineRight:
            ++right;
            break;
        default:
            break;
        }
    CHECK(left > right * 2); // the left engine dominates a rear-left hit
}

TEST_CASE("pickSubsystem: a nose-on hit biases toward avionics", "[subsystem]") {
    SubsystemSet def = makeFullSet();
    SubsystemStateSet st;
    st.init(def);
    // hitDirBody travelling AFT (came from the front) = struck the nose.
    const float noseOn[3] = {-1.f, 0.f, 0.f};

    int avionics = 0, other = 0;
    for (uint32_t i = 0; i < 4000; ++i) {
        if (pickSubsystem(def, st, noseOn, subsystemHash(3u, i, 0u)) == Subsystem::Avionics)
            ++avionics;
        else
            ++other;
    }
    // Avionics at 2.5x among 6 (others at ~0.36-1.0) should be the single most-hit subsystem.
    CHECK(avionics > other / 5);
}

TEST_CASE("pickSubsystem: skips absent and already-failed subsystems", "[subsystem]") {
    SubsystemSet def;
    def.parts[static_cast<int>(Subsystem::EngineLeft)] = {100.f, 1.f};
    def.parts[static_cast<int>(Subsystem::EngineRight)] = {100.f, 1.f};
    // Controls/avionics/etc. absent (hp 0).
    SubsystemStateSet st;
    st.init(def);
    const float noDir[3] = {0.f, 0.f, 0.f};

    // Kill the left engine; every subsequent pick must be the right one.
    st.failedMask |= (1u << static_cast<int>(Subsystem::EngineLeft));
    for (uint32_t i = 0; i < 500; ++i)
        CHECK(pickSubsystem(def, st, noDir, subsystemHash(9u, i, 0u)) == Subsystem::EngineRight);

    // Both dead: nothing eligible.
    st.failedMask |= (1u << static_cast<int>(Subsystem::EngineRight));
    CHECK(pickSubsystem(def, st, noDir, subsystemHash(9u, 1u, 0u)) == Subsystem::Count);
}

TEST_CASE("subsystemHash is deterministic and 24-bit", "[subsystem]") {
    CHECK(subsystemHash(1u, 2u, 3u) == subsystemHash(1u, 2u, 3u));
    CHECK(subsystemHash(1u, 2u, 3u) != subsystemHash(1u, 2u, 4u));
    for (uint32_t i = 0; i < 1000; ++i)
        CHECK(subsystemHash(i, i * 7u, 5u) < 0x01000000u);
}
