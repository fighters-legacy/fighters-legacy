// SPDX-License-Identifier: GPL-3.0-or-later
//
// Orbit GEOMETRY of the loiter controllers (#1340), flown through the real FlightIntegrator.
//
// #1141 rebuilt the three loiter loops (bank-limited turn, climb-rate altitude hold, speed hold);
// this file is the assertion that was still missing, that the circle the aircraft flies is the
// circle the mission asked for. It was not. `orbitSteer` aimed at a lookahead point along the
// TANGENT from the aircraft's own position, which carries no radial error at all -- flying
// perpendicular to the radius is an equilibrium at ANY radius, so every metre the bank-limited turn
// lagged by was permanent and the orbit grew for as long as the run lasted. Measured before the fix,
// on the SHIPPED 3 km default: 3.0 -> 8.7 km in 120 s, monotonically. A 700 m racetrack (the
// demo-bomber-defense case in the issue) reached 4.8 km.
//
// Nothing said so: the mission asked for one circle and the world flew another, and the only symptom
// an author saw was an aircraft that was never where the cameras pointed.

#include "ai/DynamicLoiterController.h"
#include "ai/Guidance.h"
#include "ai/LoiterController.h"
#include "entity/EntityDef.h"
#include "entity/EntityManager.h"
#include "entity/EntityTypeRegistry.h"
#include "mock_log.h"

#include "ai_flight_harness.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <glm/glm.hpp>
#include <numbers>

using namespace fl;
using Catch::Approx;
using fl::test::flyController;
using fl::test::kHarnessR;
using fl::test::levelStateAt;

namespace {

constexpr int kSeconds = 180; // three minutes: long enough for a slow outward drift to be obvious

// Where the aircraft actually flew relative to the orbit centre, sampled every tick. The thirds are
// what separate "settled a little wide" from "still growing": a spiral has no last word.
struct OrbitTrace {
    double minRadiusM{1e30};
    double maxRadiusM{0.0};
    double endRadiusM{0.0};
    double thirdMeanM[3]{};
    double sweptRad{0.0}; // signed total angle travelled about the centre; sign = orbit direction

    [[nodiscard]] double lateMeanM() const {
        return thirdMeanM[2];
    }
};

fl::test::TickHook radiusProbe(glm::dvec3 centre, OrbitTrace& out, int totalTicks) {
    auto prevAngle = std::make_shared<double>(std::nan(""));
    return [&out, centre, totalTicks, prevAngle](uint64_t tick, const EntityState& own) {
        const double dx = own.transform.pos[0] - centre.x;
        const double dz = own.transform.pos[2] - centre.z;
        const double r = std::sqrt(dx * dx + dz * dz);
        out.minRadiusM = std::min(out.minRadiusM, r);
        out.maxRadiusM = std::max(out.maxRadiusM, r);
        out.endRadiusM = r;

        const int third = std::min(2, static_cast<int>(tick) * 3 / totalTicks);
        out.thirdMeanM[third] += r / (totalTicks / 3.0);

        const double a = std::atan2(dz, dx);
        if (!std::isnan(*prevAngle)) {
            double d = a - *prevAngle;
            while (d > std::numbers::pi)
                d -= 2.0 * std::numbers::pi;
            while (d < -std::numbers::pi)
                d += 2.0 * std::numbers::pi;
            out.sweptRad += d;
        }
        *prevAngle = a;
    };
}

// A world holding one escortee, flown by the test (for DynamicLoiterController).
struct EscortWorld {
    NullLogger logger;
    EntityTypeRegistry registry;
    EntityManager em;
    EntityId targetId;

    EscortWorld(double x, double alt, double z) : em(logger, registry) {
        EntityDef d;
        d.id = "test:escortee";
        d.name = "Escortee";
        registry.registerType(d);
        EntityTransform t{};
        t.pos[0] = x;
        t.pos[1] = alt;
        t.pos[2] = z;
        targetId = em.spawn("test:escortee", t);
    }
};

} // namespace

// ---------------------------------------------------------------------------
// The geometry the issue is about
// ---------------------------------------------------------------------------

TEST_CASE("LoiterController flies the circle it was given instead of spiralling out of it (#1340)", "[loiter][orbit]") {
    // The SHIPPED default radius, which is what every load-spawned loiter entity flies. Before the
    // fix this reached 8.7 km from a 3 km command and was still growing when the run ended.
    constexpr float kRadius = 3000.f;
    constexpr double kAlt = 1500.0;
    const glm::dvec3 centre(0.0, kAlt, 0.0);

    OrbitTrace orbit;
    fl::ai::LoiterController ctrl(centre, kRadius, static_cast<float>(kAlt));
    const auto t = flyController(ctrl, levelStateAt(kRadius, kAlt, 0.0, 120.f), kSeconds,
                                 radiusProbe(centre, orbit, 60 * kSeconds));

    INFO("radius thirds " << orbit.thirdMeanM[0] << " / " << orbit.thirdMeanM[1] << " / " << orbit.thirdMeanM[2]
                          << ", end " << orbit.endRadiusM << ", alt " << t.minAltM << ".." << t.maxAltM << " end "
                          << t.endAltM << ", end speed " << t.endSpeedMps);
    CHECK_FALSE(t.crashed());
    // Settled, not spiralling: the last third is no wider than the middle one.
    CHECK(orbit.thirdMeanM[2] <= orbit.thirdMeanM[1] * 1.10);
    // And it is the commanded circle, allowing the tracking lag and the bank margin the airframe's
    // own speed imposes (the trainer holds ~160 m/s level here, which needs ~3.6 km at 36 deg).
    CHECK(orbit.lateMeanM() < kRadius * 1.30);
    CHECK(std::abs(t.endAltM - kAlt) < 100.0);
}

TEST_CASE("LoiterController opens a radius the airframe cannot turn, and then holds it (#1340)", "[loiter][orbit]") {
    // The reported case: 700 m at the speed the trainer actually flies (it will not fly the 70 m/s
    // such a circle needs) is not turnable at the 45 deg orbit bank limit, which needs r >= ~1250 m.
    // The old law kept commanding the turn it could not close and spiralled to 4.8 km, sagging.
    // The commanded radius is a FLOOR now: the orbit opens to what the aircraft can fly and STAYS.
    constexpr float kRadius = 700.f;
    constexpr double kAlt = 1500.0;
    const glm::dvec3 centre(0.0, kAlt, 0.0);

    OrbitTrace orbit;
    fl::ai::LoiterController ctrl(centre, kRadius, static_cast<float>(kAlt));
    const auto t = flyController(ctrl, levelStateAt(kRadius, kAlt, 0.0, 110.f), kSeconds,
                                 radiusProbe(centre, orbit, 60 * kSeconds));

    INFO("radius thirds " << orbit.thirdMeanM[0] << " / " << orbit.thirdMeanM[1] << " / " << orbit.thirdMeanM[2]
                          << ", end " << orbit.endRadiusM << ", alt " << t.minAltM << ".." << t.maxAltM << " end "
                          << t.endAltM << ", end speed " << t.endSpeedMps);
    CHECK_FALSE(t.crashed());
    CHECK(orbit.thirdMeanM[2] <= orbit.thirdMeanM[1] * 1.10);
    // Bounded, and close to what the airframe's own speed can turn — not 4.8 km and climbing.
    CHECK(orbit.lateMeanM() < 2500.0);
    CHECK(orbit.lateMeanM() > fl::ai::orbitRadiusFloor(t.endSpeedMps) * 0.75);
    // ALTITUDE: the second half of the report. The unreachable speed target used to pin the throttle
    // at idle and the aircraft glided down at a steady 1.9 m/s — 570 m gone in 300 s.
    CHECK(t.minAltM > kAlt - 250.0);
    CHECK(t.endAltM > kAlt - 250.0);
}

TEST_CASE("LoiterController orbits the commanded way round (#1340)", "[loiter][orbit]") {
    // The aim point moved from the tangent to a lead ANGLE around the circle, so the direction sign
    // is new code. A clockwise orbit (bearing from the centre increasing in the XZ plane) and a
    // counter-clockwise one must sweep opposite ways, and both must sweep — an aircraft parked on a
    // heading also has a stable radius.
    constexpr float kRadius = 3000.f;
    constexpr double kAlt = 1500.0;
    const glm::dvec3 centre(0.0, kAlt, 0.0);

    OrbitTrace cw, ccw;
    fl::ai::LoiterController right(centre, kRadius, static_cast<float>(kAlt), 0.65f, fl::ai::LoiterDir::Clockwise);
    fl::ai::LoiterController left(centre, kRadius, static_cast<float>(kAlt), 0.65f,
                                  fl::ai::LoiterDir::CounterClockwise);
    flyController(right, levelStateAt(kRadius, kAlt, 0.0, 140.f), 120, radiusProbe(centre, cw, 60 * 120));
    flyController(left, levelStateAt(kRadius, kAlt, 0.0, 140.f), 120, radiusProbe(centre, ccw, 60 * 120));

    INFO("swept: cw " << cw.sweptRad << " rad, ccw " << ccw.sweptRad << " rad");
    CHECK(cw.sweptRad > 2.0); // at least a third of a lap, the other way from...
    CHECK(ccw.sweptRad < -2.0);
}

TEST_CASE("DynamicLoiterController escorts at the commanded radius (#1340)", "[loiter][orbit]") {
    // The escort shares orbitSteer, so it inherits both the drift and the fix. Its centre moves,
    // which is the whole point of it — a stationary escortee is the case that isolates the geometry.
    constexpr float kRadius = 3000.f;
    constexpr double kAlt = 1500.0;
    const glm::dvec3 centre(0.0, kAlt, 0.0);

    EscortWorld world(centre.x, kAlt, centre.z);
    OrbitTrace orbit;
    fl::ai::DynamicLoiterController ctrl(world.em, world.targetId, kRadius);
    const auto t = flyController(ctrl, levelStateAt(kRadius, kAlt, 0.0, 140.f), kSeconds,
                                 radiusProbe(centre, orbit, 60 * kSeconds));

    INFO("radius thirds " << orbit.thirdMeanM[0] << " / " << orbit.thirdMeanM[1] << " / " << orbit.thirdMeanM[2]
                          << ", end " << orbit.endRadiusM << ", alt " << t.minAltM << ".." << t.maxAltM);
    CHECK_FALSE(t.crashed());
    CHECK(orbit.thirdMeanM[2] <= orbit.thirdMeanM[1] * 1.10);
    CHECK(orbit.lateMeanM() < kRadius * 1.30);
}

// ---------------------------------------------------------------------------
// The primitives, in isolation
// ---------------------------------------------------------------------------

TEST_CASE("flyableOrbitRadius inverts turnSpeedForRadius (#1340)", "[loiter][orbit][guidance]") {
    // The two halves of the same geometry: the speed a radius can be turned at, and the radius a
    // speed can be turned in. Round-tripping is what makes them one law rather than two constants.
    for (const float r : {700.f, 1500.f, 3000.f, 12000.f}) {
        const float v = fl::ai::turnSpeedForRadius(r, fl::ai::kOrbitBankRad);
        CHECK(fl::ai::flyableOrbitRadius(v, fl::ai::kOrbitBankRad) == Approx(r).epsilon(0.001));
    }
    CHECK(fl::ai::flyableOrbitRadius(0.f, fl::ai::kOrbitBankRad) == Approx(0.f));
}

TEST_CASE("orbitRadiusFloor leaves the same bank margin the speed target does (#1340)", "[loiter][orbit][guidance]") {
    // A floor at the bare geometric minimum puts the aircraft on the bank limit for the whole orbit,
    // where it has nothing left to hold altitude with: measured, the trainer bled 1,060 m in 300 s.
    // The floor therefore carries the SAME margin orbitSpeedForRadius holds on the speed.
    constexpr float kSpeed = 110.f;
    const float floorR = fl::ai::orbitRadiusFloor(kSpeed);
    CHECK(floorR > fl::ai::flyableOrbitRadius(kSpeed, fl::ai::kOrbitBankRad));

    // Which is the same as saying: at the floor radius, the speed the orbit wants IS this speed.
    CHECK(fl::ai::orbitSpeedForRadius(floorR) == Approx(kSpeed).epsilon(0.01));

    // And the bank that circle actually needs is comfortably inside the limit.
    const float bankNeeded = std::atan(kSpeed * kSpeed / (9.80665f * floorR));
    CHECK(bankNeeded < fl::ai::kOrbitBankRad * 0.85f);
}
