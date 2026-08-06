// SPDX-License-Identifier: GPL-3.0-or-later
//
// Turn-law audit (#1143): every AI controller that commands a roll, flown through the real
// FlightIntegrator in a scenario that keeps its heading error alive.
//
// #1141 found that `bankToTurnAileron(headingErrorRad)` commands a roll RATE with no feedback on the
// aircraft's actual bank, so a heading error that persists winds the roll up without limit — a
// loitering aircraft reached 179.8 deg of bank and flew into the ground. Fourteen other controllers
// used the same law. This file is the characterisation that says which of them that actually breaks,
// because the answer is not "all of them": a break turn or a yo-yo is SUPPOSED to roll past 90 deg,
// and bounding it there would be the regression.
//
// Two kinds of assertion live here, deliberately:
//   * station-keeping controllers must stay upright and airborne while their error persists;
//   * manoeuvre controllers must still be ABLE to roll hard — pinned so a later well-meaning
//     "consistency" change cannot quietly neuter them.

#include "ILogger.h"
#include "ai/BreakTurnController.h"
#include "ai/EvadeController.h"
#include "ai/FormationController.h"
#include "ai/Guidance.h"
#include "ai/GunsEmploymentController.h"
#include "ai/HighYoYoController.h"
#include "ai/LagPursuitController.h"
#include "ai/LandingController.h"
#include "ai/LeadPursuitController.h"
#include "ai/LowYoYoController.h"
#include "ai/PursuitController.h"
#include "ai/SwarmController.h"
#include "ai/TakeoffController.h"
#include "ai/WaypointController.h"
#include "entity/EntityDef.h"
#include "entity/EntityManager.h"
#include "entity/EntityTypeRegistry.h"

#include "ai_flight_harness.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

using namespace fl;
using fl::test::FlightTrace;
using fl::test::flyController;
using fl::test::kHarnessR;
using fl::test::levelStateAt;

namespace {

struct NullLogger : ILogger {
    void log(LogLevel, const char*, int, const char*) override {}
    void setMinLevel(LogLevel) override {}
    void flush() override {}
};

// A world with one other aircraft in it, which the test drives.
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
        t.pos[0] = 0.0;
        t.pos[1] = 1000.0;
        t.pos[2] = 0.0;
        targetId = em.spawn("test:target", t);
    }

    EntityState* target() {
        return em.get(targetId);
    }

    // Fly the target in a level circle of `radiusM` at `speedMps`, centred on (cx, alt, cz). This is
    // the scenario that keeps a chaser's bearing error alive indefinitely — the case the rate-only
    // turn law cannot survive.
    fl::test::TickHook orbitingTarget(double cx, double alt, double cz, double radiusM, double speedMps) {
        return [this, cx, alt, cz, radiusM, speedMps](uint64_t tick, const EntityState&) {
            EntityState* tgt = target();
            if (!tgt)
                return;
            const double omega = speedMps / radiusM;
            const double a = omega * (static_cast<double>(tick) / 60.0);
            tgt->transform.pos[0] = cx + radiusM * std::cos(a);
            tgt->transform.pos[1] = alt;
            tgt->transform.pos[2] = cz + radiusM * std::sin(a);
            tgt->transform.vel[0] = static_cast<float>(-speedMps * std::sin(a));
            tgt->transform.vel[1] = 0.f;
            tgt->transform.vel[2] = static_cast<float>(speedMps * std::cos(a));
        };
    }

    // A lead flying straight and level along +X — the benign formation case.
    fl::test::TickHook straightTarget(double x0, double alt, double z0, double speedMps) {
        return [this, x0, alt, z0, speedMps](uint64_t tick, const EntityState&) {
            EntityState* tgt = target();
            if (!tgt)
                return;
            tgt->transform.pos[0] = x0 + speedMps * (static_cast<double>(tick) / 60.0);
            tgt->transform.pos[1] = alt;
            tgt->transform.pos[2] = z0;
            tgt->transform.vel[0] = static_cast<float>(speedMps);
            tgt->transform.vel[1] = 0.f;
            tgt->transform.vel[2] = 0.f;
        };
    }
};

// Every station-keeping controller must survive the same thing: an error it cannot null, for long
// enough that an unbounded roll would have inverted it.
void checkStaysUpright(const FlightTrace& t, const char* what, float maxSideslipDeg = 15.f) {
    INFO(what << ": bank " << t.maxAbsBankDeg << " deg, sideslip " << t.maxAbsSideslipDeg << " deg, alt " << t.minAltM
              << ".." << t.maxAltM << ", crash " << t.crashTimeS << " s, end speed " << t.endSpeedMps);
    CHECK_FALSE(t.crashed());
    // 90 deg is knife-edge; anything past it is on its back. Every one of these controllers reached
    // 179.7-180.0 deg before #1143.
    CHECK(t.maxAbsBankDeg < 90.f);
    // A turn flown sideways is the other half of the finding: all of them peaked near 89 deg of
    // sideslip, i.e. flying at right angles to where they were pointing.
    CHECK(t.maxAbsSideslipDeg < maxSideslipDeg);
}

constexpr int kSoakSeconds = 90; // long enough for an unbounded roll to reach inverted several times

} // namespace

// ---------------------------------------------------------------------------
// Station-keeping: the heading error persists, so the roll must not
// ---------------------------------------------------------------------------

TEST_CASE("WaypointController survives a waypoint it cannot capture (#1143)", "[turnlaw]") {
    // A waypoint closer than the aircraft's turn radius can never be captured: it orbits it forever,
    // with the bearing sweeping through 360 deg. The rate-only law rolls up.
    fl::ai::WaypointController ctrl(std::vector<glm::dvec3>{glm::dvec3{500.0, 1000.0, 0.0}}, /*captureRadiusM=*/100.f);
    const FlightTrace t = flyController(ctrl, levelStateAt(0.0, 1000.0, 0.0, 150.f), kSoakSeconds);
    checkStaysUpright(t, "waypoint it cannot capture");
}

TEST_CASE("PursuitController survives a target in a sustained turn (#1143)", "[turnlaw]") {
    TargetWorld w;
    fl::ai::PursuitController ctrl(w.em, w.targetId);
    const FlightTrace t = flyController(ctrl, levelStateAt(6000.0, 1000.0, 0.0, 150.f), kSoakSeconds,
                                        w.orbitingTarget(0.0, 1000.0, 0.0, 2500.0, 180.0));
    checkStaysUpright(t, "pursuit of an orbiting target");
}

TEST_CASE("LeadPursuitController survives a target in a sustained turn (#1143)", "[turnlaw]") {
    TargetWorld w;
    fl::ai::LeadPursuitController ctrl(w.em, w.targetId);
    const FlightTrace t = flyController(ctrl, levelStateAt(6000.0, 1000.0, 0.0, 150.f), kSoakSeconds,
                                        w.orbitingTarget(0.0, 1000.0, 0.0, 2500.0, 180.0));
    checkStaysUpright(t, "lead pursuit of an orbiting target");
}

TEST_CASE("LagPursuitController survives a target in a sustained turn (#1143)", "[turnlaw]") {
    TargetWorld w;
    fl::ai::LagPursuitController ctrl(w.em, w.targetId);
    const FlightTrace t = flyController(ctrl, levelStateAt(6000.0, 1000.0, 0.0, 150.f), kSoakSeconds,
                                        w.orbitingTarget(0.0, 1000.0, 0.0, 2500.0, 180.0));
    checkStaysUpright(t, "lag pursuit of an orbiting target");
}

TEST_CASE("GunsEmploymentController survives a target in a sustained turn (#1143)", "[turnlaw]") {
    TargetWorld w;
    fl::ai::GunsEmploymentController ctrl(w.em, w.targetId);
    const FlightTrace t = flyController(ctrl, levelStateAt(4000.0, 1000.0, 0.0, 150.f), kSoakSeconds,
                                        w.orbitingTarget(0.0, 1000.0, 0.0, 2500.0, 180.0));
    checkStaysUpright(t, "guns tracking an orbiting target");
}

TEST_CASE("FormationController survives a lead in a sustained turn (#1143)", "[turnlaw]") {
    TargetWorld w;
    fl::ai::FormationController ctrl(w.em, w.targetId, /*slotIndex=*/1);
    const FlightTrace t = flyController(ctrl, levelStateAt(2000.0, 1000.0, 500.0, 150.f), kSoakSeconds,
                                        w.orbitingTarget(0.0, 1000.0, 0.0, 4000.0, 160.0));
    checkStaysUpright(t, "formation on an orbiting lead");
}

TEST_CASE("FormationController holds station on a straight lead (#1143)", "[turnlaw]") {
    TargetWorld w;
    fl::ai::FormationController ctrl(w.em, w.targetId, /*slotIndex=*/1);
    const FlightTrace t = flyController(ctrl, levelStateAt(0.0, 1000.0, 300.0, 150.f), kSoakSeconds,
                                        w.straightTarget(0.0, 1000.0, 0.0, 150.0));
    checkStaysUpright(t, "formation on a straight lead");
}

TEST_CASE("EvadeController survives a threat that keeps chasing (#1143)", "[turnlaw]") {
    // Evade turns AWAY, so a threat sitting still leaves it flying straight with no error at all —
    // which is why the first cut of this test measured 9 deg of bank and proved nothing. A threat
    // that keeps repositioning is the real case, and it keeps the (negated) heading error alive
    // exactly like a target in a sustained turn does for a pursuer.
    TargetWorld w;
    fl::ai::EvadeController ctrl(w.em, w.targetId);
    const FlightTrace t = flyController(ctrl, levelStateAt(1500.0, 3000.0, 0.0, 200.f), kSoakSeconds,
                                        w.orbitingTarget(0.0, 3000.0, 0.0, 2000.0, 200.0));
    // 25 deg rather than 15: a 60 deg banked escape at full throttle with afterburner is the hardest
    // aerodynamic case here, and the honest measured residual is 18 deg — down from 89 deg, which is
    // the fix. Tightening this number would mean tuning the controller against a threshold invented
    // for the test rather than against the defect. What the residual costs (this controller also
    // climbs several km on unspent afterburner energy) is Evade's energy management, not its turn
    // law, and is called out in the PR rather than quietly absorbed here.
    checkStaysUpright(t, "evading a circling threat", /*maxSideslipDeg=*/25.f);
}

TEST_CASE("SwarmController survives a migration point it orbits (#1143)", "[turnlaw]") {
    TargetWorld w;
    fl::ai::SwarmController ctrl(w.em, glm::dvec3{0.0, 1000.0, 0.0});
    const FlightTrace t = flyController(ctrl, levelStateAt(800.0, 1000.0, 0.0, 150.f), kSoakSeconds);
    checkStaysUpright(t, "swarm around a migration point");
}

// ⚠ These two pass on the OLD law as well, and that is reported rather than hidden: an approach and
// a climbout chase a FIXED bearing, so the heading error decays instead of persisting, and no
// scenario tried here makes the rate-only law fail for them. Their change to the attitude-closed law
// with a 25 deg ceiling is therefore a safety property — a wing down near the runway is how you
// arrive on one wingtip — not a fix for a measured defect. The tests guard the ceiling.

TEST_CASE("LandingController flies an offset approach without dropping a wing (#1143)", "[turnlaw]") {
    // An approach with a lateral offset holds a correction on the way down, a few hundred feet above
    // the ground.
    fl::ai::LandingController ctrl(glm::dvec3{8000.0, 0.0, 0.0}, /*headingDeg=*/90.f, /*runwayElevM=*/0.f);
    const FlightTrace t = flyController(ctrl, levelStateAt(-4000.0, 700.0, 1200.0, 90.f), 60);
    INFO("offset approach: bank " << t.maxAbsBankDeg << " deg, sideslip " << t.maxAbsSideslipDeg << " deg, alt "
                                  << t.minAltM << ".." << t.maxAltM);
    // A landing ENDS on the ground, so "crashed" is not the signal here — the bank is.
    CHECK(t.maxAbsBankDeg < 45.f);
    CHECK(t.maxAbsSideslipDeg < 20.f);
}

TEST_CASE("TakeoffController climbs out without dropping a wing (#1143)", "[turnlaw]") {
    // Airborne climbout with the runway heading to chase and the ground right there.
    fl::ai::TakeoffController ctrl(glm::dvec3{0.0, 0.0, 0.0}, /*headingDeg=*/90.f, /*runwayElevM=*/0.f);
    const FlightTrace t = flyController(ctrl, levelStateAt(2000.0, 300.0, 800.0, 120.f), 60);
    INFO("climbout: bank " << t.maxAbsBankDeg << " deg, sideslip " << t.maxAbsSideslipDeg << " deg, alt " << t.minAltM
                           << ".." << t.maxAltM);
    CHECK_FALSE(t.crashed());
    CHECK(t.maxAbsBankDeg < 45.f);
    CHECK(t.maxAbsSideslipDeg < 20.f);
}

// ---------------------------------------------------------------------------
// Manoeuvre controllers: rolling hard IS the behaviour
// ---------------------------------------------------------------------------
//
// These are pinned in the opposite direction. They run for a couple of seconds under a state
// machine, not indefinitely, and a bank limit would be the regression — so the assertion is that
// they still roll, and that the aircraft is still flying when the manoeuvre ends.

TEST_CASE("BreakTurnController still rolls hard and stays flying (#1143)", "[turnlaw][manoeuvre]") {
    TargetWorld w;
    fl::ai::BreakTurnController ctrl(w.em, w.targetId);
    const FlightTrace t =
        flyController(ctrl, levelStateAt(3000.0, 3000.0, 0.0, 200.f), 8, w.straightTarget(0.0, 3000.0, 0.0, 150.0));
    INFO("break turn: bank " << t.maxAbsBankDeg << " deg, alt " << t.minAltM << ".." << t.maxAltM);
    CHECK(t.maxAbsBankDeg > 45.f); // it is a BREAK turn — do not bound this to a loiter's 45 deg
    CHECK_FALSE(t.crashed());
}

TEST_CASE("HighYoYoController still rolls and climbs (#1143)", "[turnlaw][manoeuvre]") {
    TargetWorld w;
    fl::ai::HighYoYoController ctrl(w.em, w.targetId);
    const FlightTrace t = flyController(ctrl, levelStateAt(2000.0, 3000.0, 0.0, 220.f), 8,
                                        w.orbitingTarget(0.0, 3000.0, 0.0, 1500.0, 150.0));
    INFO("high yo-yo: bank " << t.maxAbsBankDeg << " deg, alt " << t.minAltM << ".." << t.maxAltM);
    CHECK(t.maxAbsBankDeg > 20.f);
    CHECK_FALSE(t.crashed());
}

TEST_CASE("LowYoYoController still rolls and dives (#1143)", "[turnlaw][manoeuvre]") {
    TargetWorld w;
    fl::ai::LowYoYoController ctrl(w.em, w.targetId);
    const FlightTrace t = flyController(ctrl, levelStateAt(2000.0, 4000.0, 0.0, 220.f), 8,
                                        w.orbitingTarget(0.0, 4000.0, 0.0, 1500.0, 150.0));
    INFO("low yo-yo: bank " << t.maxAbsBankDeg << " deg, alt " << t.minAltM << ".." << t.maxAltM);
    CHECK(t.maxAbsBankDeg > 20.f);
    CHECK_FALSE(t.crashed());
}
