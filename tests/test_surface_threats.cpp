// SPDX-License-Identifier: GPL-3.0-or-later
//
// Builtin surface targets + threats (#863): the five builtin surface entities register, and the SAM /
// AAA emplacements engage an aircraft they honestly detect and shoot back. WB-level integration, like
// the fire-path tests in test_world_broadcaster, because the whole point is sense -> decide -> fire.

#include "ILogger.h"
#include "ai/SurfaceThreatControllers.h"
#include "ai/Threat.h"
#include "content/ContentBootstrap.h"
#include "entity/EntityManager.h"
#include "entity/EntityTypeRegistry.h"
#include "net/WorldBroadcaster.h"
#include "sensor/BuiltinSensors.h"
#include "weapon/WeaponRegistry.h"

#include "mock_network.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <numbers>
#include <string>

using namespace fl;

namespace {

struct NullLog : ILogger {
    void log(LogLevel, const char*, int, const char*) override {}
    void setMinLevel(LogLevel) override {}
    void flush() override {}
};

// Resolve the builtin sensor heads the surface entities reference (the SAM radar; the eyeball is the
// implicit default and never asks the resolver).
std::shared_ptr<const sensor::SensorDef> builtinSensorResolver(const std::string& id) {
    if (id == "builtin:sam-radar")
        return {std::shared_ptr<const sensor::SensorDef>{}, &sensor::BuiltinSensors::groundRadar()};
    return nullptr;
}

// A WB armed and wired the way fl-server wires it for the sandbox.
struct ThreatFixture {
    NullLog logger;
    TrackingNetwork net;
    EntityTypeRegistry registry;
    WeaponRegistry weapons;
    std::unique_ptr<EntityManager> em;
    std::unique_ptr<WorldBroadcaster> wb;

    ThreatFixture() {
        registry.registerType(builtinDebugEntityDef());
        registerBuiltinSurfaceEntities(registry);
        registerBuiltinWeapons(weapons);
        registerProjectileEntityDefs(weapons, registry, logger);

        em = std::make_unique<EntityManager>(logger, registry);
        fl::WorldQueries q_wb;
        q_wb.sensorDefs = builtinSensorResolver;
        // `this` rather than a pointer taken after construction: the query is frozen at construction
        // now, and the fixture outlives every call into it.
        q_wb.targetDesignator = [this](const EntityState& shooter, const float axis[3]) -> EntityId {
            return ai::designateFromContacts(shooter, axis, wb->contactsFor(shooter.id.index), 60000.f,
                                             std::numbers::pi_v<float>, nullptr);
        };
        wb = std::make_unique<WorldBroadcaster>(*em, registry, net, logger, nullptr, std::move(q_wb));
        wb->setWeaponRegistry(&weapons);
        wb->setSensorCheckHz(60.f); // sense every tick — these tests are about the engagement, not cadence
        // Launch designation the way fl-server wires it: a shooter designates the nearest hostile it
        // has an honest track on (so a SAM's SARH guides at the aircraft instead of flying dumb).
    }

    // Spawn facing +X (identity) unless noseUp, which points the nose at +Y (a 90 deg rotation about
    // +Z): the vertical-launcher geometry a ground SAM needs so its missile climbs instead of flying
    // into the dirt off the rail.
    EntityId spawn(const char* type, double x, double y, double z, uint16_t faction, bool noseUp = false) {
        EntityTransform t{};
        t.pos[0] = x;
        t.pos[1] = y;
        t.pos[2] = z;
        if (noseUp) {
            t.quat[2] = 0.70710678f; // z
            t.quat[3] = 0.70710678f; // w
        } else {
            t.quat[3] = 1.f; // identity: nose along +X
        }
        const EntityId id = em->spawn(type, t);
        if (EntityState* st = em->get(id))
            st->factionIndex = faction;
        return id;
    }

    void tick(uint64_t n, uint64_t& t) {
        for (uint64_t i = 0; i < n; ++i)
            wb->onTick(1.0 / 60.0, ++t);
    }

    int liveOfType(const char* typeId) const {
        const uint32_t typeIdx = registry.indexById(typeId);
        int n = 0;
        em->forEach([&](const EntityState& e) {
            if (!e.dead && e.typeIndex == typeIdx)
                ++n;
        });
        return n;
    }
};

} // namespace

TEST_CASE("registerBuiltinSurfaceEntities registers all six surface types (#863, +carrier #38)", "[surface_threats]") {
    EntityTypeRegistry registry;
    CHECK(registerBuiltinSurfaceEntities(registry) == 6u); // five #863 types + builtin:carrier (#38)
    for (const char* id : {"builtin:ground-vehicle", "builtin:naval-vessel", "builtin:static-target",
                           "builtin:sam-site", "builtin:aaa", "builtin:carrier"}) {
        const EntityDef* def = registry.findById(id);
        REQUIRE(def != nullptr);
        CHECK(def->damage.has_value());             // a real damage model, not binary death
        CHECK(def->damage->subsystems.has_value()); // + a subsystem table
        CHECK(def->collisionRadiusM > 0.f);         // an explicit collision sphere
    }
    // The SAM emits (RWR seam) and carries a launcher; the AAA carries a gun.
    CHECK(registry.findById("builtin:sam-site")->sensorIds == std::vector<std::string>{"builtin:sam-radar"});
    CHECK(registry.findById("builtin:sam-site")->hardpoints.size() == 1u);
    CHECK(registry.findById("builtin:aaa")->hardpoints.size() == 1u);
}

TEST_CASE("a builtin SAM site acquires an aircraft on radar and launches a SARH (#863)", "[surface_threats]") {
    ThreatFixture f;

    // SAM (faction 2) facing +X; a hostile aircraft (faction 1) 6 km dead ahead — well inside radar
    // range and the launcher's forward cone. (Emplacements are tested at altitude to isolate the
    // engagement logic from ground-contact physics; a ground SAM's launch geometry is a #585 concern.)
    const EntityId sam = f.spawn("builtin:sam-site", 0.0, 5000.0, 0.0, 2);
    f.spawn("builtin:debug-entity", 6000.0, 5000.0, 0.0, 1);
    f.wb->registerController(sam, std::make_unique<ai::SamEngagementController>(*f.em));

    REQUIRE(f.liveOfType("projectile:builtin:sarh-missile") == 0);
    uint64_t t = 0;
    int peak = 0;
    for (int i = 0; i < 120; ++i) { // detect + react, then the launcher fires on its interval
        f.tick(1, t);
        peak = std::max(peak, f.liveOfType("projectile:builtin:sarh-missile"));
    }
    CHECK(peak >= 1); // acquired and launched
}

TEST_CASE("a builtin SAM does not fire at a friendly or when nothing is in range (#863)", "[surface_threats]") {
    ThreatFixture f;

    // Same faction as the SAM -> not hostile -> no launch, however long it looks.
    const EntityId sam = f.spawn("builtin:sam-site", 0.0, 5000.0, 0.0, 2);
    f.spawn("builtin:debug-entity", 6000.0, 5000.0, 0.0, 2); // friendly
    f.wb->registerController(sam, std::make_unique<ai::SamEngagementController>(*f.em));

    uint64_t t = 0;
    f.tick(120, t);
    CHECK(f.liveOfType("projectile:builtin:sarh-missile") == 0);
}

TEST_CASE("a builtin AAA leads and fires on an aircraft in its engagement cone (#863)", "[surface_threats]") {
    ThreatFixture f;

    // AAA (faction 2) facing +X; a hostile aircraft (faction 1) 700 m dead ahead on its boresight line
    // — inside the cannon's reach and the eyeball's sight. It fires within the first few ticks (before
    // it drifts off the level boresight) and its rounds connect.
    const EntityId aaa = f.spawn("builtin:aaa", 0.0, 5000.0, 0.0, 2);
    const EntityId victim = f.spawn("builtin:debug-entity", 700.0, 5000.0, 0.0, 1);
    f.wb->registerController(aaa, std::make_unique<ai::AaaFireController>(*f.em));

    REQUIRE(victim.valid());
    uint64_t t = 0;
    f.tick(40, t);
    // The AAA led the target and its rounds connected: the victim is damaged, or destroyed outright
    // (reaped, so it is gone from the pool) by sustained cannon fire. The AAA is the only damage
    // source in the scene, so a dead victim can only be its work.
    const EntityState* vs = f.em->get(victim);
    const bool damagedOrDestroyed = (vs == nullptr) || (vs->hp < 100.f);
    CHECK(damagedOrDestroyed);
}
