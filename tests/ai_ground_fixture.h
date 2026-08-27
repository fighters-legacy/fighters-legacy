// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Shared scaffolding for the two closed-loop AI files that fly over TERRAIN -- test_ai_terrain_floor
// (#1352) and test_ai_energy (#1353). They are two halves of one crash and they need the same world:
// ground at a real elevation, and something low over it to chase.
//
// It is separate from tests/test_ai_turn_law.cpp's own TargetWorld on purpose. That one spawns its
// target at 1,000 m over a flat y = 0 plane and exists to keep a heading error alive; this one
// exists to put the ground somewhere that matters. Merging them would mean a fixture that does
// neither job clearly.

#include "entity/AiTickContext.h"
#include "entity/EntityDef.h"
#include "entity/EntityManager.h"
#include "entity/EntityState.h"
#include "entity/EntityTypeRegistry.h"
#include "mock_log.h"

#include "ai_flight_harness.h"

#include <cmath>
#include <functional>

namespace fl::test {

// The demo-sam-strike site's terrain, which is where both defects were measured.
inline constexpr float kSiteTerrainM = 545.f;

// A context carrying a ground reference, exactly as WorldBroadcaster fills it each tick.
inline std::function<fl::AiTickContext()> groundAt(const float& elevM) {
    return [&elevM]() {
        fl::AiTickContext ctx{};
        ctx.groundElevM = &elevM;
        return ctx;
    };
}

// One other aircraft, driven by the test.
struct GroundWorld {
    fl::NullLogger logger;
    fl::EntityTypeRegistry registry;
    fl::EntityManager em;
    fl::EntityId targetId;

    GroundWorld() : em(logger, registry) {
        fl::EntityDef d;
        d.id = "test:target";
        d.name = "Target";
        registry.registerType(d);
        fl::EntityTransform t{};
        t.pos[1] = kSiteTerrainM + 15.0;
        targetId = em.spawn("test:target", t);
    }

    // A target orbiting at `alt` MSL -- low over the terrain, and never straightening out, so a
    // chaser's bearing error never dies and it keeps being told to go down there. That instruction
    // is the geometric core of both #1352 (it obeys, into the hill) and #1353 (obeying costs every
    // knot it has).
    fl::test::TickHook lowOrbitingTarget(double alt, double radiusM, double speedMps) {
        return [this, alt, radiusM, speedMps](uint64_t tick, const fl::EntityState&) {
            fl::EntityState* tgt = em.get(targetId);
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

} // namespace fl::test
