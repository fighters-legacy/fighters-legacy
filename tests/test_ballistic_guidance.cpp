// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ballistic missile guidance (#355): boost-phase steering to an impact point, the lofted pitch
// program, MIRV deployment through the controller-spawn seam, and attribution chaining.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "ILogger.h"
#include "ai/AiControllerFactory.h"
#include "ai/BallisticGuidanceController.h"
#include "content/ContentBootstrap.h"
#include "entity/EntityManager.h"
#include "entity/EntityTypeRegistry.h"
#include "flight/BallisticForceModel.h"
#include "flight/FlightIntegrator.h"
#include "flight/FlightModelParser.h"
#include "flight/LocalFrame.h"
#include "mock_log.h"
#include "net/WorldBroadcaster.h"

#include "mock_network.h"

#include <cmath>
#include <memory>
#include <string>

using namespace fl;

namespace {

const std::string kSrbmToml = R"(
[aircraft]
name = "Test SRBM"
type = "ballistic"

[flight_model]
mass_kg      = 700.0
wing_area_m2 = 0.3
wingspan_m   = 0.5
mac_m        = 0.5
fuel_kg      = 800.0
ixx_kg_m2    = 300.0
iyy_kg_m2    = 4000.0
izz_kg_m2    = 4000.0

[engine.boost]
thrust_n    = 60000.0
burn_time_s = 15.0

[aero.drag_polar]
cd0 = 0.05
)";

// Fly a guided shot at a downrange target and return the horizontal miss distance (m).
double flyShot(double rangeM) {
    auto model = std::make_shared<FlightModelData>(parseFlightModel(kSrbmToml));
    FlightIntegrator fi(model);
    fi.setForceModel(BallisticForceModel::instance());
    fi.setSpeedGuard(8000.0);

    FlightState s{};
    s.pos_world[1] = 1.0;
    s.vel_body[0] = 30.f; // off the rail with a little forward speed, nose level
    s.fuel_kg = 800.f;
    s.mass_kg = 1500.f;
    fi.reset(s);

    ai::BallisticGuidanceController::Params p;
    p.targetPos = {rangeM, 0.0, 0.0};
    ai::BallisticGuidanceController ctrl(p);

    // Mirror integrator state into an EntityState the controller can read.
    EntityState es{};
    es.id = {1, 1};

    PayloadEffect px{};
    double impactX = 0.0;
    for (int t = 0; t < 60 * 240; ++t) {
        const FlightState& fs = fi.state();
        for (int a = 0; a < 3; ++a)
            es.transform.pos[a] = fs.pos_world[a];
        // World-frame velocity for the controller: rotate body velocity by the quat.
        const glm::quat q{fs.quat[3], fs.quat[0], fs.quat[1], fs.quat[2]};
        const glm::vec3 velWorld = q * glm::vec3{static_cast<float>(fs.vel_body[0]), static_cast<float>(fs.vel_body[1]),
                                                 static_cast<float>(fs.vel_body[2])};
        for (int a = 0; a < 3; ++a)
            es.transform.vel[a] = velWorld[a];
        for (int a = 0; a < 4; ++a)
            es.transform.quat[a] = fs.quat[a];

        const ControlInput in = ctrl.sample(es, static_cast<uint64_t>(t), 1.0 / 60.0);
        fi.step(1.f / 60.f, in, px);

        const glm::dvec3 pos{fi.state().pos_world[0], fi.state().pos_world[1], fi.state().pos_world[2]};
        if (t > 60 * 30 && fl::localAltitude(pos, kEarthRadiusM) < 2.0) {
            impactX = std::sqrt(pos.x * pos.x + pos.z * pos.z);
            break;
        }
    }
    return std::abs(impactX - rangeM);
}

} // namespace

TEST_CASE("BallisticGuidance: hits near the commanded impact point at several ranges", "[ballistic_guidance]") {
    // Honest tolerances: a fixed-impulse solid motor with a simple lofted pitch program is a Scud,
    // not a JDAM. It undershoots targets set well inside its max range (the loft is optimal near max
    // range and too steep for a short shot) and lands close near the top of the envelope — ~40% miss
    // at 20 km, but only ~4% at 32 km. The meaningful claim is that guidance beats the UNGUIDED
    // alternative by a wide margin: a 45-degree max-range shot from this booster lands near 45-50 km,
    // so the unguided miss on a 20 km target would be ~25+ km — several times the guided miss below.
    // Terminal accuracy (energy management, a real Lambert solver) is #355's future refinement.
    //
    // #891: these misses widened slightly when the integrator's transport term was corrected to
    // conserve energy — the old explicit-tangent term spuriously ADDED energy during the boost/loft
    // rotation, flattering the short-range shot. The bound reflects the corrected physics.
    CHECK(flyShot(20000.0) < 9000.0);
    CHECK(flyShot(32000.0) < 7000.0);
}

TEST_CASE("BallisticGuidance: MIRV deploys past apogee, deterministically", "[ballistic_guidance]") {
    ai::BallisticGuidanceController::Params p;
    p.targetPos = {30000.0, 0.0, 0.0};
    p.mirvCount = 3;
    ai::BallisticGuidanceController ctrl(p);

    // A bus already FALLING (past apogee): deploy fires on the first sample.
    EntityState es{};
    es.id = {1, 1};
    es.transform.pos[0] = 15000.0;
    es.transform.pos[1] = 12000.0;
    es.transform.vel[0] = 600.f;
    es.transform.vel[1] = -50.f;
    es.transform.quat[3] = 1.f;
    ctrl.sample(es, 1, 1.0 / 60.0);
    REQUIRE(ctrl.mirvDeployed());

    auto reqs = ctrl.drainSpawnRequests();
    REQUIRE(reqs.size() == 3u);
    for (const SpawnRequest& r : reqs) {
        CHECK(r.typeId.empty()); // "same type as the bus"
        REQUIRE(r.makeController != nullptr);
        CHECK(r.makeController() != nullptr);
    }
    CHECK(ctrl.drainSpawnRequests().empty()); // drained once, deploys once

    // Deterministic: an identical bus fans identical velocities.
    ai::BallisticGuidanceController ctrl2(p);
    ctrl2.sample(es, 1, 1.0 / 60.0);
    auto reqs2 = ctrl2.drainSpawnRequests();
    REQUIRE(reqs2.size() == 3u);
    for (std::size_t i = 0; i < 3; ++i)
        for (int a = 0; a < 3; ++a)
            REQUIRE(reqs[i].transform.vel[a] == reqs2[i].transform.vel[a]);
}

TEST_CASE("WorldBroadcaster: MIRV children spawn through the seam and inherit ownership",
          "[ballistic_guidance][world_broadcaster]") {
    NullLogger logger;
    NullNetwork net;
    EntityTypeRegistry registry;
    EntityDef srbm;
    srbm.id = "t:srbm";
    srbm.name = "SRBM";
    srbm.category = ObjectCategory::AirVehicle;
    srbm.maxHp = 10.f;
    registry.registerType(srbm);
    EntityManager em(logger, registry);

    WorldBroadcaster broadcaster(em, registry, net, logger);

    // A falling bus owned by peer 42: deploy fires on its first sampled tick.
    EntityTransform t{};
    t.pos[1] = 12000.0;
    t.vel[0] = 600.f;
    t.vel[1] = -50.f;
    t.quat[3] = 1.f;
    const EntityId bus = em.spawn("t:srbm", t, /*ownerId=*/42u);

    ai::BallisticGuidanceController::Params p;
    p.targetPos = {30000.0, 0.0, 0.0};
    p.mirvCount = 3;
    broadcaster.registerController(bus, std::make_unique<ai::BallisticGuidanceController>(p), nullptr);

    broadcaster.onTick(1.0 / 60.0, 1u);
    broadcaster.onTick(1.0 / 60.0, 2u);

    int children = 0;
    em.forEach([&](const EntityState& s) {
        if (s.id != bus && !s.dead) {
            ++children;
            CHECK(s.ownerId == 42u); // an RV kill credits whoever launched the bus
        }
    });
    CHECK(children == 3);
}

TEST_CASE("AiControllerFactory: ballistic grammar parses and validates", "[ballistic_guidance]") {
    std::vector<std::string_view> ok{"40000", "0", "0"};
    CHECK(ai::createController("ballistic", ok) != nullptr);

    std::vector<std::string_view> mirv{"40000", "0", "0", "3", "1500"};
    CHECK(ai::createController("ballistic", mirv) != nullptr);

    std::vector<std::string_view> tooFew{"40000", "0"};
    CHECK(ai::createController("ballistic", tooFew) == nullptr);

    std::vector<std::string_view> sillyMirv{"40000", "0", "0", "9999"};
    CHECK(ai::createController("ballistic", sillyMirv) == nullptr);
}
