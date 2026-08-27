// SPDX-License-Identifier: GPL-3.0-or-later
//
// Energy management (#1353), flown through the real FlightIntegrator.
//
// #1352 gave the AI a terrain-relative hard deck. It kept the aircraft off the ground and left it
// slow: the pursuit case there ends at ~104 m/s against a ~150 m/s cruise. This file is the other
// half of the same crash.
//
// `builtin:fighter` had no notion of its own energy. It pulled kCombatBankRad (80 deg) at any speed
// and the recovery held its maximum speed-scaled pull all the way down, so in a sustained engagement
// on the #1334 trainer -- a low-thrust airframe with a real stall -- it turned itself onto the back
// of the drag curve and stayed there. Measured on demo-sam-strike: 73-166 m/s during the fight, then
// ~30 s oscillating 547 <-> 616 m MSL at 74-88 m/s with the deck flickering, a terrain scrape
// (hp 100 -> 77), and `ejection: entity 4 -> pilot KIA (-0 m AGL, 69 m/s)`.
//
// It was never short of thrust -- the recovery already commanded throttle 1.0. It was short of
// AIRSPEED, and its own pull is what held it there. A mush is escaped with energy, not with more
// pull. This is the energy counterpart of #1141's "the answer to *we are low* was *more nose-up*".
//
// The trainer's numbers, from BuiltinFlightModel.cpp: Vs(1 g, gross) = 54 m/s at sea level,
// T/W = 0.32, +-7 g structural. Corner speed is therefore ~143 m/s -- about its own cruise -- so
// below cruise every extra degree of bank is bought with airspeed it cannot replace.

#include "ai/Guidance.h"
#include "ai/PursuitController.h"
#include "ai_ground_fixture.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <glm/glm.hpp>

using namespace fl;
using Catch::Approx;
using fl::test::FlightTrace;
using fl::test::flyController;
using fl::test::groundAt;
using fl::test::GroundWorld;
using fl::test::levelStateAt;

constexpr float kTerrain = fl::test::kSiteTerrainM;

namespace {

// Nothing but the recovery, every tick: isolates the hard-deck law from any controller's geometry.
// This is the aircraft in the state the issue measured -- low, slow, and being pulled.
struct RecoveryOnly : fl::IEntityController {
    // recoverSpeedMps 0 reproduces the PRE-#1353 law exactly: the firm pull at every speed, with no
    // unload stage. Having both behind one knob is what makes the two cases below comparisons rather
    // than assertions about a number nobody can check.
    float recoverSpeedMps{fl::ai::kDeckRecoverSpeedMps};
    explicit RecoveryOnly(float recoverAt = fl::ai::kDeckRecoverSpeedMps) : recoverSpeedMps(recoverAt) {}

    fl::ControlInput sample(const fl::EntityState& state, uint64_t, double, const fl::AiTickContext& ctx) override {
        fl::ControlInput ctrl{};
        (void)fl::ai::terrainFloorRecovery(ctrl, state.transform.quat, state.transform.pos, state.transform.vel, ctx,
                                           fl::ai::kCombatDeckAglM, m_planetRadiusM, 0.f, recoverSpeedMps);
        return ctrl;
    }
};

} // namespace

TEST_CASE("the hard-deck recovery gets a mushing aircraft off the back of the drag curve", "[energy][ai][closedloop]") {
    // 80 m/s at 100 m AGL: the state the sandbox CAP died in. Full throttle is already commanded and
    // is not the problem; what has to change is the pull.
    const float ground = kTerrain;
    RecoveryOnly nowLaw;
    RecoveryOnly oldLaw(0.f); // the pre-#1353 always-pull recovery, for the comparison
    const FlightTrace t = flyController(nowLaw, levelStateAt(0.0, kTerrain + 100.0, 0.0, 80.f), 60, {},
                                        groundAt(ground), {}, static_cast<double>(kTerrain));
    const FlightTrace before = flyController(oldLaw, levelStateAt(0.0, kTerrain + 100.0, 0.0, 80.f), 60, {},
                                             groundAt(ground), {}, static_cast<double>(kTerrain));

    INFO("now:    AGL " << t.minAltM - kTerrain << ".." << t.maxAltM - kTerrain << ", end AGL " << t.endAltM - kTerrain
                        << ", end speed " << t.endSpeedMps << ", crash " << t.crashTimeS);
    INFO("before: AGL " << before.minAltM - kTerrain << ".." << before.maxAltM - kTerrain << ", end AGL "
                        << before.endAltM - kTerrain << ", end speed " << before.endSpeedMps << ", crash "
                        << before.crashTimeS);

    // Measured: the old law ended at 58.3 m/s -- BELOW the 54 m/s 1 g stall -- after porpoising
    // 0.08..461 m AGL. The new one ends at 155.5 m/s and 316 m AGL. The aircraft is not merely
    // alive, it is flying.
    CHECK(before.endSpeedMps < 80.f);       // it never got its energy back...
    CHECK(before.minAltM - kTerrain < 5.0); // ...and it very nearly arrived
    CHECK_FALSE(t.crashed());
    CHECK(t.endSpeedMps > 140.f);
    CHECK(t.endAltM - kTerrain > fl::ai::kCombatDeckAglM); // and it climbed back out through the deck
}

TEST_CASE("the firm dive arrest is not given up to get it", "[energy][ai][closedloop]") {
    // The regression guard on the other side of the trade. #1339 sized the firm pull against a real
    // dive: the altitude cascade is far too gentle to stop a 30 deg descent at 175 m/s, and measured
    // with the cascade doing the recovery, both strikers flew into the terrain at ~100 m AGL. The
    // #1353 fix must not quietly hand every recovery to the cascade -- above kDeckRecoverSpeedMps
    // the pull is still the pull.
    const float ground = kTerrain;

    // 175 m/s, 25 deg nose down, entering the deck from above.
    FlightState init = levelStateAt(0.0, kTerrain + 290.0, 0.0, 175.f);
    // Body +X forward, +Y up, +Z right: a POSITIVE rotation about +Z takes the nose up, so a dive is
    // negative. Getting this backwards makes the case a climb, which arrests itself and proves nothing.
    const glm::quat q = glm::angleAxis(glm::radians(-25.f), glm::vec3(0.f, 0.f, 1.f));
    init.quat[0] = q.x;
    init.quat[1] = q.y;
    init.quat[2] = q.z;
    init.quat[3] = q.w;
    init.vel_body[0] = 175.f;

    RecoveryOnly nowLaw;
    RecoveryOnly oldLaw(0.f); // the pre-#1353 always-pull recovery
    const FlightTrace t = flyController(nowLaw, init, 30, {}, groundAt(ground), {}, static_cast<double>(kTerrain));
    FlightState initCopy = init;
    const FlightTrace before =
        flyController(oldLaw, initCopy, 30, {}, groundAt(ground), {}, static_cast<double>(kTerrain));

    INFO("dive arrest now: min AGL " << t.minAltM - kTerrain << ", end AGL " << t.endAltM - kTerrain << ", end speed "
                                     << t.endSpeedMps << ", crash " << t.crashTimeS);
    INFO("dive arrest before: min AGL " << before.minAltM - kTerrain << ", crash " << before.crashTimeS);
    CHECK_FALSE(t.crashed());
    // Measured: 211.8 m AGL now against 211.8 m before -- above kDeckRecoverSpeedMps the two laws
    // ARE the same law, which is the point of the guard. The tolerance is there so a future change
    // to the pull has to be a deliberate one.
    CHECK(t.minAltM >= before.minAltM - 5.0);
}

TEST_CASE("a fighter that runs out of energy leaves instead of dying", "[energy][ai][closedloop]") {
    // The C++ counterpart of builtin:fighter's disengage. A pursuit told to chase a target that keeps
    // turning is the scenario that bled the sandbox CAP dry; the bank it commands is now the bank the
    // airspeed will pay for, so the chase costs what it can afford.
    //
    // Before #1353 this same case (in test_ai_terrain_floor.cpp) survived on the hard deck alone and
    // ended at ~104 m/s against a ~150 m/s cruise -- alive, and still on the back of the drag curve.
    GroundWorld world;
    ai::PursuitController ctrl(world.em, world.targetId);
    const float ground = kTerrain;
    const FlightTrace t = flyController(ctrl, levelStateAt(2500.0, kTerrain + 300.0, 0.0, 150.f), 90,
                                        world.lowOrbitingTarget(kTerrain + 15.0, 1500.0, 140.0), groundAt(ground), {},
                                        static_cast<double>(kTerrain));
    INFO("energy-limited pursuit: min AGL " << t.minAltM - kTerrain << ", end speed " << t.endSpeedMps << ", max bank "
                                            << t.maxAbsBankDeg << ", crash " << t.crashTimeS);
    CHECK_FALSE(t.crashed());
    CHECK(t.endSpeedMps > 140.f);
}

TEST_CASE("a bank angle is a load factor, and the limit says so", "[energy][ai]") {
    using fl::ai::bankLimitForSpeed;
    using fl::ai::kCombatBankRad;

    // Fast: the role ceiling is what binds, which is the pre-#1353 behaviour where it was affordable.
    CHECK(bankLimitForSpeed(250.f, kCombatBankRad) == Approx(kCombatBankRad));

    // Cruise: a hard turn, but not 80 degrees -- the trainer cannot hold 5.8 g at 150 m/s.
    const float atCruise = bankLimitForSpeed(150.f, kCombatBankRad);
    CHECK(atCruise > 1.0f);
    CHECK(atCruise < kCombatBankRad);

    // Slow: less and less, and eventually NO turn at all. Zero means fly straight and accelerate --
    // a caller that reads it as "unlimited" has inverted the whole point.
    CHECK(bankLimitForSpeed(100.f, kCombatBankRad) < atCruise);
    CHECK(bankLimitForSpeed(80.f, kCombatBankRad) == 0.f);
    CHECK(bankLimitForSpeed(0.f, kCombatBankRad) == 0.f);
}
