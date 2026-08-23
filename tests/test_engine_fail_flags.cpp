// SPDX-License-Identifier: GPL-3.0-or-later
//
// fl::engineTotalLoss / engineLeftOut / engineRightOut / engineLostThrust (#1258).
//
// This classification was restated in all four force models. It had to be got right four times,
// and the next flag bit would have had to be classified four times -- in files whose authors are
// thinking about rotor arms and hull drag, not about which bits mean "no thrust at all".
//
// The grouping is the part worth pinning, because it is not obvious from the bit names: a
// CENTRELINE kill (#901) is a total loss, not an asymmetric one, and a compressor surge (#308) is
// a total loss that is only temporary. Get either wrong and a single-engine airframe starts
// yawing toward a dead engine it does not have.

#include "flight/EngineFailFlags.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("no flags is no failure", "[engine_fail]") {
    CHECK_FALSE(fl::engineTotalLoss(0));
    CHECK_FALSE(fl::engineLeftOut(0));
    CHECK_FALSE(fl::engineRightOut(0));
}

TEST_CASE("every total-loss bit is a total loss on its own", "[engine_fail]") {
    CHECK(fl::engineTotalLoss(fl::kEngineFailGeneric));
    CHECK(fl::engineTotalLoss(fl::kEngineFlameout));
    CHECK(fl::engineTotalLoss(fl::kEngineCompStall)); // transient, but total while it lasts

    // #901: a centreline kill is a TOTAL loss, deliberately NOT an asymmetric one -- a
    // single-engine airframe has no dead side to swing toward.
    CHECK(fl::engineTotalLoss(fl::kEngineFailCenter));
    CHECK_FALSE(fl::engineLeftOut(fl::kEngineFailCenter));
    CHECK_FALSE(fl::engineRightOut(fl::kEngineFailCenter));
}

TEST_CASE("one side out is asymmetric, both sides out is total", "[engine_fail]") {
    CHECK(fl::engineLeftOut(fl::kEngineFailLeft));
    CHECK_FALSE(fl::engineRightOut(fl::kEngineFailLeft));
    CHECK_FALSE(fl::engineTotalLoss(fl::kEngineFailLeft));

    CHECK(fl::engineRightOut(fl::kEngineFailRight));
    CHECK_FALSE(fl::engineTotalLoss(fl::kEngineFailRight));

    // Losing both is the same outcome as losing the lot, and must not read as "asymmetric".
    const uint8_t both = fl::kEngineFailLeft | fl::kEngineFailRight;
    CHECK(fl::engineTotalLoss(both));
}

TEST_CASE("a total-loss bit wins over a side bit", "[engine_fail]") {
    // A flameout while the left engine is already out is still a total loss: the models branch on
    // totalLoss first, so this ordering is what stops a dead aircraft from also yawing.
    CHECK(fl::engineTotalLoss(fl::kEngineFlameout | fl::kEngineFailLeft));
    CHECK(fl::engineLeftOut(fl::kEngineFlameout | fl::kEngineFailLeft));
}

TEST_CASE("lost thrust is one engine's share, spelled as a division", "[engine_fail]") {
    CHECK(fl::engineLostThrust(1000.f, 2) == 500.f);
    CHECK(fl::engineLostThrust(1000.f, 4) == 250.f);

    // A def that does not say assumes a twin.
    CHECK(fl::engineLostThrust(1000.f, 0) == 500.f);
    CHECK(fl::engineLostThrust(1000.f, -1) == 500.f);

    // Bit-exactly the division the four copies performed. Multiplying by a precomputed 1/count
    // would be a different float, and this runs inside the flight model.
    const float thrust = 73312.7f;
    CHECK(fl::engineLostThrust(thrust, 3) == thrust / static_cast<float>(3));
}

TEST_CASE("the helpers are usable at compile time", "[engine_fail]") {
    static_assert(fl::engineTotalLoss(fl::kEngineFailCenter));
    static_assert(!fl::engineTotalLoss(fl::kEngineFailLeft));
    static_assert(fl::engineLostThrust(100.f, 4) == 25.f);
}
