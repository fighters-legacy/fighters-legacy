// SPDX-License-Identifier: GPL-3.0-or-later
//
// The missile seeker + PN guidance (#627). The cases that matter: the seeker binds to honest
// sensing's rules (PoD gates acquisition, geometry maintains, coast reports where the target WAS),
// guidance flies only at the seeker's last-known state, the launch gate refuses geometry the head
// cannot see, and every trajectory is deterministic — same launch state, same flight, bit for bit.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "ILogger.h"
#include "entity/EntityManager.h"
#include "entity/EntityTypeRegistry.h"
#include "flight/CentralGravityField.h"
#include "sensor/BuiltinSensors.h"
#include "sensor/Detection.h"
#include "spatial/SpatialIndex.h"
#include "weapon/BuiltinWeapon.h"
#include "weapon/ProjectileGuidance.h"
#include "weapon/ProjectileSystem.h"
#include "weapon/Seeker.h"

#include <glm/geometric.hpp>

#include <vector>

using namespace fl;

namespace {

struct NullLoggerS : ILogger {
    void log(LogLevel, const char*, int, const char*) override {}
    void setMinLevel(LogLevel) override {}
    void flush() override {}
};

// A world armed with the builtin IR missile: registry with shooter/target/projectile types, a
// ProjectileSystem resolving the builtin seeker heads.
struct SeekerWorld {
    NullLoggerS log;
    EntityTypeRegistry registry;
    WeaponRegistry weapons;
    uint32_t aimIdx{UINT32_MAX};
    std::unique_ptr<EntityManager> em;
    SpatialIndex si;
    ProjectileSystem ps;

    SeekerWorld() {
        aimIdx = weapons.registerWeapon(BuiltinWeapon::irMissile());
        EntityDef fighter;
        fighter.id = "t:fighter";
        fighter.name = "F";
        fighter.category = ObjectCategory::AirVehicle;
        fighter.maxHp = 100.f;
        registry.registerType(fighter);
        EntityDef proj;
        proj.id = projectileTypeId(BuiltinWeapon::irMissile());
        proj.name = "aim";
        proj.category = ObjectCategory::Projectile;
        proj.maxHp = 1.f;
        registry.registerType(proj);
        em = std::make_unique<EntityManager>(log, registry);

        ps.configure(&weapons, &CentralGravityField::earthInstance());
        ps.setTypeRegistry(&registry);
        ps.setSensorResolver([](const std::string& id) -> std::shared_ptr<const sensor::SensorDef> {
            if (id == "builtin:ir-seeker")
                return {std::shared_ptr<const sensor::SensorDef>{}, &sensor::BuiltinSensors::irSeeker()};
            return nullptr;
        });
    }

    EntityId spawnAt(double x, double y, double z, float vx = 0.f, float vz = 0.f) {
        EntityTransform t{};
        t.pos[0] = x;
        t.pos[1] = y;
        t.pos[2] = z;
        t.vel[0] = vx;
        t.vel[2] = vz;
        t.quat[3] = 1.f;
        return em->spawn("t:fighter", t);
    }

    void rebuildIndex() {
        si.clear();
        em->forEach([this](const EntityState& s) { si.insert(s.id.index, s.transform.pos); });
    }
};

} // namespace

// ---------------------------------------------------------------------------
// PN math
// ---------------------------------------------------------------------------

TEST_CASE("PN: a true collision course commands (almost) nothing", "[seeker][pn]") {
    // Head-on, constant bearing: LOS rate is zero, so the command is zero.
    const glm::vec3 a = proportionalNavAccel({0, 0, 0}, {300.f, 0.f, 0.f}, {3000, 0, 0}, {-200.f, 0.f, 0.f},
                                             kProportionalNavGain, 300.f);
    CHECK(glm::length(a) < 0.01f);
}

TEST_CASE("PN: a crossing target draws a lateral command toward the intercept", "[seeker][pn]") {
    // Target dead ahead, crossing +Z: the LOS rotates toward +Z, so the command must push +Z —
    // and it must be LATERAL (perpendicular to the missile's velocity), never thrust.
    const glm::vec3 vel{600.f, 0.f, 0.f};
    const glm::vec3 a =
        proportionalNavAccel({0, 0, 0}, vel, {3000, 0, 0}, {0.f, 0.f, 150.f}, kProportionalNavGain, 300.f);
    CHECK(a.z > 1.f);
    CHECK(std::abs(glm::dot(a, glm::normalize(vel))) < 0.01f);
}

TEST_CASE("PN: the command is clamped to the airframe's structural limit", "[seeker][pn]") {
    const glm::vec3 a =
        proportionalNavAccel({0, 0, 0}, {900.f, 0.f, 0.f}, {800, 0, 0}, {0.f, 0.f, 400.f}, kProportionalNavGain, 100.f);
    CHECK(glm::length(a) == Catch::Approx(100.f).epsilon(0.001));
}

// ---------------------------------------------------------------------------
// Legacy lobe + seeker checks
// ---------------------------------------------------------------------------

TEST_CASE("synthesizeLegacySeekerDef keeps old packs flying", "[seeker]") {
    SeekerDef s;
    s.type = SeekerType::Infrared;
    s.fovDeg = 40.f;
    s.acquisitionRangeM = 12000.f;
    REQUIRE(s.usesLegacyLobe());

    const sensor::SensorDef d = synthesizeLegacySeekerDef(s);
    CHECK(d.type == sensor::SensorType::Ir);
    CHECK_FALSE(d.emitter);
    CHECK(d.search.azHalfAngleDeg == 40.f);
    CHECK(d.search.maxRangeM == 12000.f);
    REQUIRE(d.track.has_value());
    CHECK(d.track->azHalfAngleDeg > d.search.azHalfAngleDeg); // the gimbal holds more than it acquires
    CHECK(d.lockHoldS > 0.f);
}

TEST_CASE("stepSeekerCheck: geometry maintains, a break coasts, coast expiry drops", "[seeker]") {
    SeekerWorld w;
    const EntityId target = w.spawnAt(2000.0, 8000.0, 0.0);

    SeekerTrack st;
    st.targetId = target;
    st.track.state = sensor::ContactState::Locked; // launch-gate state
    const sensor::SensorDef& head = sensor::BuiltinSensors::irSeeker();
    const float quat[4] = {0.f, 0.f, 0.f, 1.f}; // nose +X, target dead ahead
    const sensor::SensingEnvironment env{};

    // In lobe: the lock holds with NO die, and last-known state updates to the truth.
    stepSeekerCheck(st, head, false, {0.0, 8000.0, 0.0}, quat, w.em->get(target), SignatureDef{}, env,
                    /*missileIdx=*/9, /*tick=*/6, /*dtS=*/0.1f, /*seduced=*/false);
    CHECK(st.track.state == sensor::ContactState::Locked);
    CHECK(st.lastKnownPos.x == 2000.0);

    // The target leaves the gimbal (teleports behind): the track COASTS on the frozen point.
    w.em->get(target)->transform.pos[0] = -5000.0;
    stepSeekerCheck(st, head, false, {0.0, 8000.0, 0.0}, quat, w.em->get(target), SignatureDef{}, env, 9, 12, 0.1f,
                    false);
    CHECK(st.track.state == sensor::ContactState::Coasting);
    CHECK(st.lastKnownPos.x == 2000.0); // WHERE THE TARGET WAS — never updated while coasting

    // Coast runs out (lockHoldS = 1.0 s): the track drops.
    for (int i = 0; i < 12; ++i)
        stepSeekerCheck(st, head, false, {0.0, 8000.0, 0.0}, quat, w.em->get(target), SignatureDef{}, env, 9,
                        18 + static_cast<uint64_t>(i) * 6, 0.1f, false);
    CHECK(st.track.state == sensor::ContactState::Lost);
}

TEST_CASE("stepSeekerCheck: a seduced check is a coast, and reacquire needs no new die", "[seeker]") {
    SeekerWorld w;
    const EntityId target = w.spawnAt(2000.0, 8000.0, 0.0);

    SeekerTrack st;
    st.targetId = target;
    st.track.state = sensor::ContactState::Locked;
    const sensor::SensorDef& head = sensor::BuiltinSensors::irSeeker();
    const float quat[4] = {0.f, 0.f, 0.f, 1.f};

    // The flare seam says "seduced": the target is treated as invisible this check.
    stepSeekerCheck(st, head, false, {0.0, 8000.0, 0.0}, quat, w.em->get(target), SignatureDef{}, {}, 9, 6, 0.1f,
                    /*seduced=*/true);
    CHECK(st.track.state == sensor::ContactState::Coasting);

    // The seduction ends while the coast still runs: geometry alone recovers the lock (the central
    // rule — it was never lost, only unobserved).
    stepSeekerCheck(st, head, false, {0.0, 8000.0, 0.0}, quat, w.em->get(target), SignatureDef{}, {}, 9, 12, 0.1f,
                    /*seduced=*/false);
    CHECK(st.track.state != sensor::ContactState::Lost);
    CHECK(st.track.state != sensor::ContactState::Coasting);
}

// ---------------------------------------------------------------------------
// End-to-end flight
// ---------------------------------------------------------------------------

TEST_CASE("IR missile: guided intercept of a crossing target", "[seeker][projectile]") {
    SeekerWorld w;
    const EntityId shooter = w.spawnAt(0.0, 8000.0, 0.0, /*vx=*/250.f);
    const EntityId target = w.spawnAt(2500.0, 8000.0, 0.0, /*vx=*/0.f, /*vz=*/150.f);

    const EntityId pid = w.ps.launch(*w.em, w.aimIdx, *w.em->get(shooter), 0u, target);
    REQUIRE(pid.valid());
    REQUIRE(w.ps.projectiles()[0].seeker.targetId == target); // the launch gate passed

    std::vector<ProjectileImpact> impacts;
    for (uint64_t t = 1; t <= 60 * 30 && impacts.empty(); ++t) {
        // The target flies its crossing course; the missile chases its CONTACT, not the truth.
        EntityState* ts = w.em->get(target);
        ts->transform.pos[0] += ts->transform.vel[0] / 60.0;
        ts->transform.pos[2] += ts->transform.vel[2] / 60.0;
        w.rebuildIndex();
        w.ps.step(*w.em, w.si, 1.f / 60.f, {}, t, {}, impacts);
    }

    REQUIRE(impacts.size() == 1u);
    CHECK(impacts[0].directHit == target); // proximity fuze on the designated target
}

TEST_CASE("IR missile: the launch gate refuses a target the head cannot see", "[seeker][projectile]") {
    SeekerWorld w;
    const EntityId shooter = w.spawnAt(0.0, 8000.0, 0.0, 250.f);
    const EntityId behind = w.spawnAt(-2500.0, 8000.0, 0.0); // dead six — outside the gimbal

    const EntityId pid = w.ps.launch(*w.em, w.aimIdx, *w.em->get(shooter), 0u, behind);
    REQUIRE(pid.valid());                                       // the store still leaves the rails...
    CHECK_FALSE(w.ps.projectiles()[0].seeker.targetId.valid()); // ...but it flies DUMB
}

TEST_CASE("IR missile: same launch state, bit-identical trajectory", "[seeker][projectile]") {
    auto fly = [](int seedTicks) {
        SeekerWorld w;
        const EntityId shooter = w.spawnAt(0.0, 8000.0, 0.0, 250.f);
        const EntityId target = w.spawnAt(2500.0, 8000.0, 0.0, 0.f, 150.f);
        w.ps.launch(*w.em, w.aimIdx, *w.em->get(shooter), 0u, target);
        std::vector<ProjectileImpact> impacts;
        std::vector<glm::dvec3> trail;
        for (uint64_t t = 1; t <= static_cast<uint64_t>(seedTicks) && impacts.empty(); ++t) {
            EntityState* ts = w.em->get(target);
            ts->transform.pos[2] += ts->transform.vel[2] / 60.0;
            w.rebuildIndex();
            w.ps.step(*w.em, w.si, 1.f / 60.f, {}, t, {}, impacts);
            if (!w.ps.projectiles().empty())
                trail.push_back(w.ps.projectiles()[0].pos);
        }
        return trail;
    };
    const auto a = fly(240);
    const auto b = fly(240);
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i)
        REQUIRE(a[i] == b[i]); // bit-identical, not approximately equal
}

// ---------------------------------------------------------------------------
// Dice statistics (the reacquire-from-Lost path is the only roll a seeker makes)
// ---------------------------------------------------------------------------

TEST_CASE("seeker dice: the seeded ensemble tracks the authored PoD", "[seeker][pod]") {
    const float pod = sensor::BuiltinSensors::irSeeker().search.pod; // 0.55
    int passes = 0;
    constexpr int kTrials = 4096;
    for (int i = 0; i < kTrials; ++i) {
        const uint32_t h = sensor::detectionHash(/*observer=*/42u, /*target=*/7u, /*tick=*/static_cast<uint64_t>(i),
                                                 /*slot=*/0u, /*lobeSalt=*/1u);
        if (sensor::rollPasses(h, pod))
            ++passes;
    }
    const float rate = static_cast<float>(passes) / kTrials;
    CHECK(rate > pod - 0.05f);
    CHECK(rate < pod + 0.05f);
}
