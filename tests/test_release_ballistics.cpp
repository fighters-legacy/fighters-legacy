// SPDX-License-Identifier: GPL-3.0-or-later
//
// Bombs and unguided rockets (#629): release ballistics, deterministic salvo dispersion, wind
// drift, and the CCIP predictor's self-consistency with the flight it predicts.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "ILogger.h"
#include "entity/EntityManager.h"
#include "entity/EntityTypeRegistry.h"
#include "flight/BallisticLead.h"
#include "flight/CentralGravityField.h"
#include "mock_log.h"
#include "spatial/SpatialIndex.h"
#include "weapon/ProjectileSystem.h"

#include <glm/geometric.hpp>

#include <cmath>
#include <vector>

using namespace fl;

namespace {

WeaponDef makeBomb() {
    WeaponDef d;
    d.id = "t:bomb";
    d.name = "Test Bomb";
    d.type = WeaponType::Bomb;
    d.category = WeaponCategory::AirToGround;
    d.performance.maxRangeM = 20000.f; // generous overrange bound: the fall decides, not the fuse
    d.warhead.blastRadiusM = 36.f;
    d.warhead.damage = 200.f;
    d.load.massKg = 227.f;
    d.load.dragFactor = 0.002f;
    d.load.rounds = 1;
    return d;
}

WeaponDef makeRocket() {
    WeaponDef d;
    d.id = "t:rocket";
    d.name = "Test Rocket";
    d.type = WeaponType::Rocket;
    d.category = WeaponCategory::AirToGround;
    d.performance.maxRangeM = 3000.f;
    d.performance.maxSpeedMps = 700.f;
    d.performance.motorBurnTimeS = 1.f;
    d.performance.cepM = 30.f; // 10 mrad cone at max range
    d.warhead.blastRadiusM = 10.f;
    d.warhead.damage = 40.f;
    d.load.massKg = 200.f;
    d.load.dragFactor = 0.003f;
    d.load.rounds = 19;
    return d;
}

struct ReleaseWorld {
    NullLogger log;
    EntityTypeRegistry registry;
    WeaponRegistry weapons;
    uint32_t bombIdx{UINT32_MAX};
    uint32_t rocketIdx{UINT32_MAX};
    std::unique_ptr<EntityManager> em;
    SpatialIndex si;
    ProjectileSystem ps;

    ReleaseWorld() {
        bombIdx = weapons.registerWeapon(makeBomb());
        rocketIdx = weapons.registerWeapon(makeRocket());
        EntityDef fighter;
        fighter.id = "t:fighter";
        fighter.name = "F";
        fighter.category = ObjectCategory::AirVehicle;
        fighter.maxHp = 100.f;
        registry.registerType(fighter);
        for (const uint32_t wi : {bombIdx, rocketIdx}) {
            EntityDef proj;
            proj.id = projectileTypeId(*weapons.byIndex(wi));
            proj.name = "proj";
            proj.category = ObjectCategory::Projectile;
            proj.maxHp = 1.f;
            registry.registerType(proj);
        }
        em = std::make_unique<EntityManager>(log, registry);
        ps.configure(&weapons, &CentralGravityField::earthInstance());
        ps.setTypeRegistry(&registry);
    }

    EntityId spawnShooter(double y, float vx) {
        EntityTransform t{};
        t.pos[1] = y;
        t.vel[0] = vx;
        t.quat[3] = 1.f;
        return em->spawn("t:fighter", t);
    }

    // Fly everything out and return the impacts.
    std::vector<ProjectileImpact> flyOut(uint64_t maxTicks = 60 * 60) {
        std::vector<ProjectileImpact> impacts;
        for (uint64_t t = 1; t <= maxTicks && ps.liveCount() > 0; ++t) {
            si.clear();
            em->forEach([this](const EntityState& s) { si.insert(s.id.index, s.transform.pos); });
            ps.step(*em, si, 1.f / 60.f, {}, t, {}, impacts);
        }
        return impacts;
    }
};

} // namespace

TEST_CASE("Bomb: ejected downward, falls on ballistics, lands short of the vacuum range", "[release][bomb]") {
    ReleaseWorld w;
    const EntityId shooter = w.spawnShooter(500.0, 200.f);
    REQUIRE(w.ps.launch(*w.em, w.bombIdx, *w.em->get(shooter), 0u).valid());

    // The ejector pushes DOWN, never ahead: initial velocity keeps the carrier's forward speed and
    // gains a downward component.
    CHECK(w.ps.projectiles()[0].vel.x == Catch::Approx(200.f).margin(0.5));
    CHECK(w.ps.projectiles()[0].vel.y < -0.5f);

    const auto impacts = w.flyOut();
    REQUIRE(impacts.size() == 1u);
    CHECK_FALSE(impacts[0].directHit.valid()); // ground impact, not a fuze

    // ~500 m of fall is ~10.1 s in vacuum; drag stretches the fall slightly and SHORTENS the
    // throw. The vacuum throw at 200 m/s would be ~2020 m — the real one must land short of it
    // but still well downrange.
    CHECK(impacts[0].pos[0] > 500.0);
    CHECK(impacts[0].pos[0] < 2020.0);
}

TEST_CASE("Bomb: drifts downwind by the drag model, not by an offset", "[release][bomb]") {
    ReleaseWorld w;
    const EntityId shooter = w.spawnShooter(500.0, 200.f);
    REQUIRE(w.ps.launch(*w.em, w.bombIdx, *w.em->get(shooter), 0u).valid());
    w.ps.setWind({0.f, 0.f, 15.f}); // 15 m/s crosswind
    const auto impacts = w.flyOut();
    REQUIRE(impacts.size() == 1u);
    CHECK(impacts[0].pos[2] > 5.0); // blown downwind across ~10 s of fall

    // And with no wind the same drop lands on the centreline.
    ReleaseWorld calm;
    const EntityId s2 = calm.spawnShooter(500.0, 200.f);
    REQUIRE(calm.ps.launch(*calm.em, calm.bombIdx, *calm.em->get(s2), 0u).valid());
    const auto calmImpacts = calm.flyOut();
    REQUIRE(calmImpacts.size() == 1u);
    CHECK(std::abs(calmImpacts[0].pos[2]) < 0.5);
}

TEST_CASE("Rockets: a salvo fans out deterministically from the authored CEP", "[release][rocket]") {
    auto salvo = [](int count) {
        ReleaseWorld w;
        const EntityId shooter = w.spawnShooter(3000.0, 200.f);
        std::vector<float> zVel;
        for (int i = 0; i < count; ++i) {
            REQUIRE(w.ps.launch(*w.em, w.rocketIdx, *w.em->get(shooter), 0u, EntityId::null(),
                                /*tick=*/static_cast<uint64_t>(100 + i))
                        .valid());
            zVel.push_back(w.ps.projectiles().back().vel.z);
        }
        return zVel;
    };

    const auto a = salvo(20);
    // The salvo SPREADS: not every rocket flies the same line.
    float minZ = a[0], maxZ = a[0];
    for (float z : a) {
        minZ = std::min(minZ, z);
        maxZ = std::max(maxZ, z);
    }
    CHECK(maxZ - minZ > 0.05f);
    // And the spread is bounded by the cone: sigma = cep/maxRange = 10 mrad on ~215 m/s ≈ 2.2 m/s.
    CHECK(maxZ - minZ < 10.f);

    // DETERMINISTIC: the same salvo in a fresh world is bit-identical.
    const auto b = salvo(20);
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i)
        REQUIRE(a[i] == b[i]);
}

TEST_CASE("Turret launch: a store leaves along the aim direction, not the nose (#970)", "[release][turret]") {
    ReleaseWorld w;
    // A stationary shooter facing +X (identity quat), so the airframe nose is +X.
    const EntityId shooter = w.spawnShooter(3000.0, 0.f);

    // Nose fire (no aim override): the rocket leaves along +X, bit-identical to before.
    REQUIRE(w.ps.launch(*w.em, w.rocketIdx, *w.em->get(shooter), 0u).valid());
    const glm::vec3 noseVel = w.ps.projectiles().back().vel;
    CHECK(noseVel.x > 10.f);
    CHECK(std::abs(noseVel.z) < 1.f);

    // Turret fire aimed +Z (90 deg off the nose): the rocket leaves along +Z instead.
    const glm::vec3 aim{0.f, 0.f, 1.f};
    REQUIRE(w.ps.launch(*w.em, w.rocketIdx, *w.em->get(shooter), 0u, EntityId::null(), 0u, &aim).valid());
    const glm::vec3 turretVel = w.ps.projectiles().back().vel;
    CHECK(turretVel.z > 10.f);
    CHECK(std::abs(turretVel.x) < 1.f);
}

TEST_CASE("CCIP predicts where the bomb actually lands", "[release][ccip]") {
    ReleaseWorld w;
    const EntityId shooter = w.spawnShooter(500.0, 200.f);
    REQUIRE(w.ps.launch(*w.em, w.bombIdx, *w.em->get(shooter), 0u).valid());
    w.ps.setWind({0.f, 0.f, 8.f});

    // Predict from the projectile's ACTUAL release state (post-ejection), through the same model.
    const Projectile& p = w.ps.projectiles()[0];
    const double posArr[3] = {p.pos.x, p.pos.y, p.pos.z};
    const auto g = CentralGravityField::earthInstance().accelWorld(posArr);
    const auto ccip = computeCcip(p.pos, p.vel, {0.f, 0.f, 8.f}, ProjectileSystem::kCoastDecayPerS, {g[0], g[1], g[2]},
                                  [](const glm::dvec3& pos) {
                                      return CentralGravityField::earthInstance().geodeticAltitude(
                                          &pos.x); // flat datum: terrain elevation 0
                                  });
    REQUIRE(ccip.valid);

    const auto impacts = w.flyOut();
    REQUIRE(impacts.size() == 1u);
    // The predictor integrates the same physics at the same step: it may only disagree by
    // integration phase and the constant-vs-central gravity approximation over a 10 s fall.
    CHECK(std::abs(impacts[0].pos[0] - ccip.impact.x) < 15.0);
    CHECK(std::abs(impacts[0].pos[2] - ccip.impact.z) < 15.0);
}
