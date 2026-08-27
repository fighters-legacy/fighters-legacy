// SPDX-License-Identifier: GPL-3.0-or-later
//
// The terrain floor (#1352), flown through the real FlightIntegrator.
//
// The builtin AI's hard deck was an MSL ALTITUDE compared against guidance.altitude(), and the C++
// controllers had no deck at all. Over the shipped sandbox's ~545 m terrain a 600 m MSL deck is 55 m
// of protection; over terrain higher than the deck the condition "we are too low" is only true BELOW
// GROUND, so the recovery is not late, it is unreachable. Measured on demo-sam-strike before the
// fix, the red CAP fought between 547 and 616 m MSL with the deck flickering on and off, scraped the
// terrain (hp 100 -> 77) and ejected at -0 m AGL.
//
// The scenario here is the geometric core of that: an aircraft steering at something that is itself
// near the ground. A pursuit controller holds its target's altitude, so a target orbiting low over
// high terrain is an instruction to fly into the hill, and nothing in the controller ever said no.
//
// Every case is closed-loop. The single-sample ControlInput assertions these controllers used to
// have cannot see any of this -- the sign of the elevator on one tick is arithmetic, not flight
// (#1143).

#include "ai/Guidance.h"
#include "ai/LoiterController.h"
#include "ai/PursuitController.h"
#include "ai/SplitSController.h"
#include "entity/EntityDef.h"
#include "entity/EntityManager.h"
#include "entity/EntityTypeRegistry.h"
#include "mock_log.h"

#include "ai_flight_harness.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <glm/glm.hpp>

using namespace fl;
using Catch::Approx;
using fl::test::FlightTrace;
using fl::test::flyController;
using fl::test::kHarnessR;
using fl::test::levelStateAt;

namespace {

// The demo-sam-strike site's terrain, which is where this defect was measured.
constexpr float kTerrainM = 545.f;

// A context carrying a ground reference, exactly as WorldBroadcaster fills it each tick.
std::function<AiTickContext()> groundAt(const float& elevM) {
    return [&elevM]() {
        AiTickContext ctx{};
        ctx.groundElevM = &elevM;
        return ctx;
    };
}

// One other aircraft, driven by the test.
struct TargetWorld {
    NullLogger logger;
    EntityTypeRegistry registry;
    EntityManager em;
    EntityId targetId;

    TargetWorld() : em(logger, registry) {
        EntityDef d;
        d.id = "test:target";
        d.name = "Target";
        registry.registerType(d);
        EntityTransform t{};
        t.pos[1] = kTerrainM + 15.0;
        targetId = em.spawn("test:target", t);
    }

    // A target orbiting at `alt` MSL -- low over the terrain, and never straightening out, so the
    // chaser's bearing error never dies and it keeps being told to go down there.
    fl::test::TickHook lowOrbitingTarget(double alt, double radiusM, double speedMps) {
        return [this, alt, radiusM, speedMps](uint64_t tick, const EntityState&) {
            EntityState* tgt = em.get(targetId);
            if (!tgt)
                return;
            const double omega = speedMps / radiusM;
            const double a = omega * (static_cast<double>(tick) / 60.0);
            tgt->transform.pos[0] = radiusM * std::cos(a);
            tgt->transform.pos[1] = alt;
            tgt->transform.pos[2] = radiusM * std::sin(a);
            tgt->transform.vel[0] = static_cast<float>(-speedMps * std::sin(a));
            tgt->transform.vel[1] = 0.f;
            tgt->transform.vel[2] = static_cast<float>(speedMps * std::cos(a));
        };
    }
};

} // namespace

// ---------------------------------------------------------------------------
// The null contract
// ---------------------------------------------------------------------------

TEST_CASE("no ground reference disables the floor rather than inventing one", "[terrainfloor][ai]") {
    // AiTickContext's normative null: "not evaluated here", never "sea level". A controller unit
    // test that builds AiTickContext{} must get exactly the behaviour it had before the floor
    // existed -- and over 545 m terrain, reading the null as 0 would put the deck 545 m underground.
    const AiTickContext none{};
    const double pos[3] = {0.0, 100.0, 0.0};
    CHECK_FALSE(fl::ai::aglOf(pos, none).has_value());

    ControlInput ctrl{};
    const float quat[4] = {0.f, 0.f, 0.f, 1.f};
    const float vel[3] = {150.f, 0.f, 0.f};
    CHECK_FALSE(fl::ai::terrainFloorRecovery(ctrl, quat, pos, vel, none));
    CHECK(ctrl.elevator == 0.f);
    CHECK(ctrl.throttle == 0.f);
}

TEST_CASE("the floor is measured from the terrain, not from sea level", "[terrainfloor][ai]") {
    const float ground = kTerrainM;
    AiTickContext ctx{};
    ctx.groundElevM = &ground;

    // 600 m MSL: comfortably "high" by the old rule, and 55 m above the ground.
    const double low[3] = {0.0, 600.0, 0.0};
    REQUIRE(fl::ai::aglOf(low, ctx).value() == Approx(55.0).margin(0.5));

    ControlInput ctrl{};
    const float quat[4] = {0.f, 0.f, 0.f, 1.f};
    const float vel[3] = {150.f, 0.f, 0.f};
    REQUIRE(fl::ai::terrainFloorRecovery(ctrl, quat, low, vel, ctx));
    CHECK(ctrl.throttle == 1.f);
    CHECK(ctrl.elevator > 0.f); // wings are level here, so the pull is unlocked

    // Same aircraft, same MSL rule, 400 m higher ground: still inside the deck.
    const float highGround = 900.f;
    ctx.groundElevM = &highGround;
    ControlInput ctrl2{};
    const double high[3] = {0.0, 1100.0, 0.0};
    CHECK(fl::ai::terrainFloorRecovery(ctrl2, quat, high, vel, ctx));
}

TEST_CASE("rolled past knife-edge, the recovery levels the wings before it pulls", "[terrainfloor][ai]") {
    // #1141's ordering, kept: a firm pull while inverted is a split-S into the terrain.
    const float ground = kTerrainM;
    AiTickContext ctx{};
    ctx.groundElevM = &ground;
    const double pos[3] = {0.0, 600.0, 0.0};
    const float vel[3] = {150.f, 0.f, 0.f};

    // Roll 150 deg about the nose (+X body axis near the origin).
    const glm::quat q = glm::angleAxis(glm::radians(150.f), glm::vec3(1.f, 0.f, 0.f));
    const float quat[4] = {q.x, q.y, q.z, q.w};

    ControlInput ctrl{};
    REQUIRE(fl::ai::terrainFloorRecovery(ctrl, quat, pos, vel, ctx));
    CHECK(ctrl.elevator == 0.f);          // pull gated off until the lift vector points up
    CHECK(std::abs(ctrl.aileron) > 0.5f); // and the roll is commanded hard
}

// ---------------------------------------------------------------------------
// Closed loop: the defect, and the fix
// ---------------------------------------------------------------------------

TEST_CASE("a pursuit chasing something low over terrain no longer flies into it", "[terrainfloor][ai][closedloop]") {
    // The chaser starts 300 m above the terrain and is handed a target orbiting 15 m above it.
    // steerTowardPoint holds the TARGET's altitude, so the instruction is literally "descend to
    // 15 m AGL and keep turning" -- and before this issue no C++ controller declined it.
    //
    // crashBelowAltM is the terrain, not the harness's 1 m default: over ground at 545 m MSL, an
    // aircraft at 400 m is already wreckage, and a test that only notices sea level would call this
    // flight a success.
    const double kCrashAt = static_cast<double>(kTerrainM);
    constexpr int kSeconds = 90;
    constexpr double kTargetAlt = kTerrainM + 15.0;

    FlightTrace blind{};
    {
        TargetWorld world;
        ai::PursuitController ctrl(world.em, world.targetId);
        blind = flyController(ctrl, levelStateAt(2500.0, kTerrainM + 300.0, 0.0, 150.f), kSeconds,
                              world.lowOrbitingTarget(kTargetAlt, 1500.0, 140.0), {}, {}, kCrashAt);
    }

    FlightTrace floored{};
    const float ground = kTerrainM;
    {
        TargetWorld world;
        ai::PursuitController ctrl(world.em, world.targetId);
        floored = flyController(ctrl, levelStateAt(2500.0, kTerrainM + 300.0, 0.0, 150.f), kSeconds,
                                world.lowOrbitingTarget(kTargetAlt, 1500.0, 140.0), groundAt(ground), {}, kCrashAt);
    }

    INFO("no ground reference: min alt " << blind.minAltM << " (AGL " << blind.minAltM - kTerrainM << "), crash "
                                         << blind.crashTimeS << " s");
    INFO("terrain floor: min alt " << floored.minAltM << " (AGL " << floored.minAltM - kTerrainM << "), crash "
                                   << floored.crashTimeS << " s, end speed " << floored.endSpeedMps);

    // Without a ground reference the aircraft goes where it was told, which is into the hill.
    // Measured: ground contact at 5.9 s, -0.6 m AGL. Not a near miss, not a scrape -- it arrives.
    CHECK(blind.crashed());
    CHECK(blind.crashTimeS < 15.0);

    // With one, it does not. Measured: 246 m AGL at the lowest, over the full 90 s. That is 54 m
    // under the 300 m deck, which is the recovery's own overshoot and is why the deck is sized with
    // room in it rather than set at the height the aircraft must not go below.
    CHECK_FALSE(floored.crashed());
    CHECK(floored.minAltM - kTerrainM > 150.0);

    // NOT asserted here, deliberately: the trainer ends this run at ~104 m/s against a ~150 m/s
    // cruise. The deck keeps it airborne and it is still slow, because holding the recovery's pull
    // is not how an aircraft gets back off the drag curve. That is #1353, the other half of the same
    // crash, and it is a separate fix -- see this test's sibling once that lands.
}

TEST_CASE("the nav floor is a floor, not an altitude policy", "[terrainfloor][ai][closedloop]") {
    // A mission that asks for a low orbit still gets a low orbit. kNavDeckAglM is deliberately set
    // below anything a mission would sensibly command, so a controller HOLDING an altitude is never
    // fighting the deck -- only a controller about to hit the ground meets it.
    const float ground = kTerrainM;
    constexpr double kOrbitAglM = 150.0;
    const glm::dvec3 centre{0.0, 0.0, 0.0};

    ai::LoiterController loiter(centre, 3000.f, static_cast<float>(kTerrainM + kOrbitAglM), 0.7f);
    const FlightTrace t = flyController(loiter, levelStateAt(3000.0, kTerrainM + kOrbitAglM, 0.0, 150.f), 120, {},
                                        groundAt(ground), {}, static_cast<double>(kTerrainM));

    INFO("low orbit: alt " << t.minAltM << ".." << t.maxAltM << " (AGL " << t.minAltM - kTerrainM << ".."
                           << t.maxAltM - kTerrainM << "), crash " << t.crashTimeS);
    CHECK_FALSE(t.crashed());
    // Measured: the orbit lives at 132..151 m AGL for the whole two minutes -- the commanded height,
    // not the deck. If kNavDeckAglM were sized like the combat one this band would be pinned against
    // it instead, and a low-level mission would be un-authorable.
    CHECK(t.minAltM - kTerrainM > 100.0);
    CHECK(t.maxAltM - kTerrainM < kOrbitAglM + 100.0);
}

TEST_CASE("a Split-S at the deck is abandoned, not flown", "[terrainfloor][ai]") {
    // The manoeuvre IS a dive. Started below the deck it is a way to hit the ground pointing the
    // right way, so the recovery outranks it -- while the phase clock keeps running, so the state
    // machine sequencing the manoeuvre still transitions out on schedule.
    const float ground = kTerrainM;
    AiTickContext ctx{};
    ctx.groundElevM = &ground;

    EntityState es{};
    es.transform.pos[0] = 0.0;
    es.transform.pos[1] = kTerrainM + 100.0; // inside the combat deck
    es.transform.pos[2] = 0.0;
    es.transform.quat[3] = 1.f;
    es.transform.vel[0] = 150.f;

    ai::SplitSController splitS;
    const ControlInput low = splitS.sample(es, 0, 1.0 / 60.0, ctx);
    CHECK(low.throttle == 1.f);   // the manoeuvre's roll phase commands idle + full speedbrake
    CHECK(low.speedbrake == 0.f); // ...and the recovery does not

    // Same controller, same phase, clear of the terrain: the manoeuvre is flown.
    ai::SplitSController clearOfIt;
    es.transform.pos[1] = kTerrainM + 3000.0;
    const ControlInput high = clearOfIt.sample(es, 0, 1.0 / 60.0, ctx);
    CHECK(high.speedbrake == 1.f);
    CHECK(high.throttle == 0.f);
}
