// SPDX-License-Identifier: GPL-3.0-or-later
//
// Crewed control frame (#966/#969): a WorldBroadcaster steps a multi-seat aircraft as a per-seat
// control frame — a bot pilot flies via its IEntityController while a bot gunner aims a turret and
// fires along its bore, independently of the airframe nose. WB-level integration, like the fire-path
// tests, because the point is sample -> merge -> slew -> fire. Single-seat parity is covered by the
// existing (unchanged) broadcaster tests staying green.

#include "ILogger.h"
#include "entity/EntityManager.h"
#include "entity/EntityTypeRegistry.h"
#include "entity/IEntityController.h"
#include "entity/ISeatController.h"
#include "job/JobSystem.h"
#include "net/WorldBroadcaster.h"
#include "weapon/ProjectileSystem.h" // projectileTypeId
#include "weapon/WeaponRegistry.h"

#include "mock_network.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <memory>
#include <vector>

using namespace fl;
using Catch::Approx;

namespace {

struct NullLog : ILogger {
    void log(LogLevel, const char*, int, const char*) override {}
    void setMinLevel(LogLevel) override {}
    void flush() override {}
};

// A pilot that flies straight and never fires (so every projectile in the test is the gunner's).
struct NeutralPilot : IEntityController {
    ControlInput sample(const EntityState&, uint64_t, double, const AiTickContext&) override {
        return ControlInput{}; // throttle 0, no trigger/release -> parked + no pilot fire
    }
};

// A stub tail gunner (the real one is #971): always aims WORLD +Y (straight up — the airframe nose is
// +X, so this is unmistakably NOT nose fire, and firing upward keeps the rockets clear of the ground
// so they survive to be inspected). Opens fire only after `fireAfter` ticks, so the turret is fully
// slewed on-aim before the first rocket leaves.
struct StubGunner : ISeatController {
    uint64_t fireAfter{90};
    SeatCommand sample(const EntityState&, const SeatView&, uint64_t tick, double, const AiTickContext&) override {
        SeatCommand cmd;
        cmd.hasAim = true;
        cmd.aimDirWorld = glm::vec3{0.f, 1.f, 0.f};
        cmd.release = tick > fireAfter;
        return cmd;
    }
};

WeaponDef makeRocket() {
    WeaponDef d;
    d.id = "test:rkt";
    d.name = "Test Rocket";
    d.type = WeaponType::Rocket;
    d.category = WeaponCategory::AirToGround;
    d.performance.maxRangeM = 4000.f;
    d.performance.maxSpeedMps = 500.f;
    d.performance.motorBurnTimeS = 1.f;
    d.performance.cepM = 0.f; // no dispersion: the store leaves exactly along the bore
    d.warhead.blastRadiusM = 10.f;
    d.warhead.damage = 40.f;
    d.load.massKg = 20.f;
    d.load.dragFactor = 0.f;
    d.load.rounds = 40;
    return d;
}

// A bomber: a Fly+Fire pilot on station 0, and a Fire tail-gunner aiming a turret that mounts
// station 1. Built directly (not parsed) — the parser's one-owner invariant is covered in
// test_entity; this def is a valid partition (one Fly seat, disjoint stations).
EntityDef makeBomberDef() {
    EntityDef d;
    d.id = "test:bomber";
    d.name = "Bomber";
    d.category = ObjectCategory::AirVehicle;
    d.maxHp = 300.f;

    Hardpoint hp0;
    hp0.slot = 0;
    hp0.allowed = {"test:rkt"};
    hp0.defaultWeapon = "test:rkt";
    Hardpoint hp1;
    hp1.slot = 1;
    hp1.allowed = {"test:rkt"};
    hp1.defaultWeapon = "test:rkt";
    d.hardpoints = {hp0, hp1};

    TurretDef t;
    t.id = "tail";
    t.azMinDeg = -180.f;
    t.azMaxDeg = 180.f;
    t.elMinDeg = -85.f;
    t.elMaxDeg = 85.f;
    t.slewRateDegS = 90.f;
    t.stations = {1};
    d.turrets = {t};

    SeatDef pilot;
    pilot.role = "pilot";
    pilot.capabilities =
        withCapability(withCapability(CrewCapabilityMask{0}, CrewCapability::Fly), CrewCapability::Fire);
    pilot.stations = {0};
    SeatDef gunner;
    gunner.role = "tail-gunner";
    gunner.capabilities = withCapability(CrewCapabilityMask{0}, CrewCapability::Fire);
    gunner.turret = "tail";
    gunner.defaultOccupancy = SeatOccupancyDefault::Bot;
    gunner.botSpec = "stub";
    d.crew = {pilot, gunner};
    return d;
}

struct CrewFixture {
    NullLog logger;
    TrackingNetwork net;
    EntityTypeRegistry registry;
    WeaponRegistry weapons;
    std::unique_ptr<EntityManager> em;
    std::unique_ptr<WorldBroadcaster> wb;

    explicit CrewFixture(uint64_t gunnerFireAfter = 90) {
        weapons.registerWeapon(makeRocket());
        registry.registerType(makeBomberDef());
        // The projectile type the rocket flies off as.
        EntityDef proj;
        proj.id = projectileTypeId(*weapons.byIndex(0));
        proj.name = "rkt-proj";
        proj.category = ObjectCategory::Projectile;
        proj.maxHp = 1.f;
        registry.registerType(proj);

        em = std::make_unique<EntityManager>(logger, registry);
        wb = std::make_unique<WorldBroadcaster>(*em, registry, net, logger);
        wb->setWeaponRegistry(&weapons);
        wb->setGroundElevation(0.f);
        wb->setSeatControllerFactory([gunnerFireAfter](const SeatDef&, uint8_t) -> std::unique_ptr<ISeatController> {
            auto g = std::make_unique<StubGunner>();
            g->fireAfter = gunnerFireAfter;
            return g;
        });
    }

    EntityId spawnBomber(double x) {
        EntityTransform t{};
        t.pos[0] = x;
        t.pos[1] = 0.1; // parked on the ground (in-contact -> the parking hold pins it static, nose +X)
        t.quat[3] = 1.f;
        const EntityId id = em->spawn("test:bomber", t);
        wb->registerController(id, std::make_unique<NeutralPilot>(), nullptr, /*initialAirspeed=*/0.f);
        return id;
    }

    void tick(uint64_t n) {
        for (uint64_t i = 0; i < n; ++i)
            wb->onTick(1.0 / 60.0, ++m_tick);
    }

    // Every live projectile's velocity, gathered for inspection.
    std::vector<glm::vec3> projectileVels() const {
        const uint32_t projType = registry.indexById(projectileTypeId(*weapons.byIndex(0)).c_str());
        std::vector<glm::vec3> out;
        em->forEach([&](const EntityState& e) {
            if (!e.dead && e.typeIndex == projType)
                out.push_back(glm::vec3{e.transform.vel[0], e.transform.vel[1], e.transform.vel[2]});
        });
        return out;
    }

    uint64_t m_tick{0};
};

} // namespace

TEST_CASE("Crewed frame: a bot gunner fires along the turret bore, not the airframe nose (#969)", "[crew]") {
    CrewFixture fx(/*gunnerFireAfter=*/90);
    fx.spawnBomber(0.0);

    // Slew the turret onto world +Y (the airframe nose is +X), then let the gunner fire.
    fx.tick(120);

    const auto vels = fx.projectileVels();
    REQUIRE(!vels.empty()); // the gunner fired

    // Every rocket left along world +Y (the turret bore), NOT along +X (the airframe nose). If the
    // fire path had ignored the turret, these would all be +X.
    for (const glm::vec3& v : vels) {
        const float len = glm::length(v);
        REQUIRE(len > 1.f);
        CHECK(v.y / len > 0.9f);           // dominantly +Y (up), the turret bore
        CHECK(std::abs(v.x) / len < 0.2f); // not the nose
    }
}

TEST_CASE("Crewed frame: an empty seat with no factory contributes no fire (#969)", "[crew]") {
    // With no seat-controller factory, the gunner seat has no bot -> it fires nothing, and the
    // neutral pilot fires nothing, so no projectile ever spawns. The frame still steps cleanly.
    NullLog logger;
    TrackingNetwork net;
    EntityTypeRegistry registry;
    WeaponRegistry weapons;
    weapons.registerWeapon(makeRocket());
    registry.registerType(makeBomberDef());
    EntityDef proj;
    proj.id = projectileTypeId(*weapons.byIndex(0));
    proj.name = "p";
    proj.category = ObjectCategory::Projectile;
    proj.maxHp = 1.f;
    registry.registerType(proj);
    EntityManager em(logger, registry);
    WorldBroadcaster wb(em, registry, net, logger);
    wb.setWeaponRegistry(&weapons);
    wb.setGroundElevation(0.f);
    // deliberately NO setSeatControllerFactory

    EntityTransform t{};
    t.pos[1] = 1.0;
    t.quat[3] = 1.f;
    const EntityId id = em.spawn("test:bomber", t);
    wb.registerController(id, std::make_unique<NeutralPilot>(), nullptr, 0.f);

    uint64_t tick = 0;
    for (int i = 0; i < 120; ++i)
        wb.onTick(1.0 / 60.0, ++tick);

    const uint32_t projType = registry.indexById(projectileTypeId(*weapons.byIndex(0)).c_str());
    int projCount = 0;
    em.forEach([&](const EntityState& e) {
        if (!e.dead && e.typeIndex == projType)
            ++projCount;
    });
    CHECK(projCount == 0);
}

TEST_CASE("Crewed frame: the per-seat pass is serial-equivalent across worker counts (#969)", "[crew][tsan]") {
    // Eight crewed bombers spread out, each with a bot gunner. Running the same scenario with 1 vs 4
    // sim workers must produce byte-identical projectiles — the crew sampling + turret slew stay
    // disjoint per entity and the seat-bot dice are a pure function of (entity, seat, tick).
    auto run = [](uint32_t workers) {
        CrewFixture fx(/*gunnerFireAfter=*/90);
        JobSystem jobs(workers);
        fx.wb->setJobSystem(jobs);
        for (int b = 0; b < 8; ++b)
            fx.spawnBomber(static_cast<double>(b) * 500.0);
        fx.tick(120);
        std::vector<glm::vec3> vels = fx.projectileVels();
        std::sort(vels.begin(), vels.end(), [](const glm::vec3& a, const glm::vec3& c) {
            if (a.x != c.x)
                return a.x < c.x;
            if (a.y != c.y)
                return a.y < c.y;
            return a.z < c.z;
        });
        return vels;
    };

    const auto serial = run(1);
    const auto parallel = run(4);
    REQUIRE(!serial.empty());
    REQUIRE(serial.size() == parallel.size());
    for (std::size_t i = 0; i < serial.size(); ++i) {
        CHECK(serial[i].x == Approx(parallel[i].x));
        CHECK(serial[i].y == Approx(parallel[i].y));
        CHECK(serial[i].z == Approx(parallel[i].z));
    }
}
