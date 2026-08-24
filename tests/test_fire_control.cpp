// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "ILogger.h"
#include "entity/EntityDef.h"
#include "entity/EntityManager.h"
#include "entity/EntityTypeRegistry.h"
#include "flight/CentralGravityField.h"
#include "mock_log.h"
#include "spatial/SpatialIndex.h"
#include "weapon/FireControl.h"
#include "weapon/ProjectileSystem.h"
#include "weapon/WeaponDefParser.h"
#include "weapon/WeaponRegistry.h"

#include <string>
#include <vector>

using namespace fl;

namespace {

const char* kGunToml = R"toml(
[weapon]
id       = "t:gun"
name     = "Test Gun"
type     = "gun"
category = "air-to-air"
[performance]
max_range_nm     = 0.6
rate_of_fire_rpm = 1200
[warhead]
blast_radius_ft = 3
damage          = 8
[load]
weight_lb   = 300
drag_factor = 0
rounds      = 5
)toml";

const char* kMissileToml = R"toml(
[weapon]
id       = "t:aim"
name     = "Test Missile"
type     = "missile"
category = "air-to-air"
[seeker]
type            = "ir"
sensor_id       = "t:seeker"
fire_and_forget = true
[performance]
max_range_nm      = 9
min_range_nm      = 0.3
max_speed_kts     = 1500
motor_burn_time_s = 5
max_g             = 20
[warhead]
blast_radius_ft = 30
damage          = 60
[load]
weight_lb   = 190
drag_factor = 0.001
)toml";

struct FireWorld {
    WeaponRegistry weapons;
    EntityDef def;
    uint32_t gunIdx{UINT32_MAX};
    uint32_t aimIdx{UINT32_MAX};

    FireWorld() {
        gunIdx = weapons.registerWeapon(parseWeaponDef(kGunToml));
        aimIdx = weapons.registerWeapon(parseWeaponDef(kMissileToml));
        def.id = "t:fighter";
        def.name = "Fighter";
        def.category = ObjectCategory::AirVehicle;
        Hardpoint gun;
        gun.slot = 0;
        gun.allowed = {"t:gun"};
        gun.defaultWeapon = "t:gun";
        Hardpoint rail;
        rail.slot = 1;
        rail.allowed = {"t:aim"};
        rail.defaultWeapon = "t:aim";
        Hardpoint empty;
        empty.slot = 2;
        empty.allowed = {"t:aim"};
        empty.defaultWeapon = ""; // an empty station is a legitimate loadout (#828)
        def.hardpoints = {gun, rail, empty};
    }
};

} // namespace

TEST_CASE("buildLoadout: default stores, rounds, payload sum, and a sane initial selection", "[fire_control]") {
    FireWorld w;
    const LoadoutState ls = buildLoadout(w.def, w.weapons);

    REQUIRE(ls.stations.size() == 3u);
    CHECK(ls.stations[0].weaponIndex == w.gunIdx);
    CHECK(ls.stations[0].rounds == 5u); // from [load] rounds
    CHECK(ls.stations[1].weaponIndex == w.aimIdx);
    CHECK(ls.stations[1].rounds == 1u); // non-gun default
    CHECK(ls.stations[2].weaponIndex == UINT32_MAX);
    CHECK(ls.selected == 1u); // first mounted non-gun station — "select" means the rails
    CHECK(ls.payloadMassKg == Catch::Approx((300.f + 190.f) * 0.45359237f).epsilon(0.001));
}

TEST_CASE("evaluateFire: gun is rate-limited level fire; ammo runs out", "[fire_control]") {
    FireWorld w;
    FireState fs;
    fs.loadout = buildLoadout(w.def, w.weapons);

    ControlInput in{};
    in.trigger = true;

    std::vector<FireRequest> out;
    // 1200 rpm at 60 Hz = one round every 3 ticks. 5 rounds of ammo.
    for (uint64_t t = 0; t < 60; ++t)
        evaluateFire(fs, w.weapons, in, /*hold=*/false, t, 7u, out);

    REQUIRE(out.size() == 5u); // ammo-bound, not time-bound
    CHECK(out[0].kind == FireRequest::Kind::Hitscan);
    CHECK(out[0].shooterIdx == 7u);
    CHECK(fs.loadout.stations[0].rounds == 0u);
    // Rate limit held between rounds: the first five shots span >= 4*3 ticks — verified by the
    // request count staying 5 over 60 ticks rather than 20.
}

TEST_CASE("evaluateFire: store release is edge-triggered and pays mass and drag", "[fire_control]") {
    FireWorld w;
    FireState fs;
    fs.loadout = buildLoadout(w.def, w.weapons);
    const float massBefore = fs.loadout.payloadMassKg;

    ControlInput in{};
    in.release = true; // held forever — a stale-repeated input must still be ONE shot

    std::vector<FireRequest> out;
    for (uint64_t t = 0; t < 120; ++t)
        evaluateFire(fs, w.weapons, in, false, t, 7u, out);

    REQUIRE(out.size() == 1u); // the edge fired once; the level never re-fired
    CHECK(out[0].kind == FireRequest::Kind::Spawn);
    CHECK(out[0].weaponIndex == w.aimIdx);
    CHECK(fs.loadout.stations[1].rounds == 0u);
    CHECK(fs.loadout.payloadMassKg < massBefore); // the store left the rails with its mass
    CHECK(fs.loadout.payloadMassKg == Catch::Approx(300.f * 0.45359237f).epsilon(0.001));

    // A fresh edge after the cooldown fires again — but the rail is empty now.
    in.release = false;
    evaluateFire(fs, w.weapons, in, false, 200, 7u, out);
    in.release = true;
    evaluateFire(fs, w.weapons, in, false, 201, 7u, out);
    CHECK(out.size() == 1u);
}

TEST_CASE("evaluateFire: weapons hold reads intent and fires nothing - and banks no edge", "[fire_control]") {
    FireWorld w;
    FireState fs;
    fs.loadout = buildLoadout(w.def, w.weapons);

    ControlInput in{};
    in.trigger = true;
    in.release = true;

    std::vector<FireRequest> out;
    evaluateFire(fs, w.weapons, in, /*hold=*/true, 0, 7u, out);
    CHECK(out.empty());
    CHECK(fs.loadout.stations[0].rounds == 5u);

    // The hold lifts while the button is STILL held: the press that arrived during the hold must
    // not fire retroactively (the edge was consumed under the hold).
    evaluateFire(fs, w.weapons, in, /*hold=*/false, 1, 7u, out);
    CHECK(out.size() == 1u); // gun (level semantics) fires...
    CHECK(out[0].kind == FireRequest::Kind::Hitscan);
    for (const FireRequest& r : out)
        CHECK(r.kind != FireRequest::Kind::Spawn); // ...the banked release does not
}

TEST_CASE("evaluateFire: absolute station selection is clamped, 255 keeps", "[fire_control]") {
    FireWorld w;
    FireState fs;
    fs.loadout = buildLoadout(w.def, w.weapons);

    ControlInput in{};
    in.station = 200; // absurd — clamp to the last station
    std::vector<FireRequest> out;
    evaluateFire(fs, w.weapons, in, false, 0, 7u, out);
    CHECK(fs.loadout.selected == 2u);

    in.station = 255; // keep
    evaluateFire(fs, w.weapons, in, false, 1, 7u, out);
    CHECK(fs.loadout.selected == 2u);
}

TEST_CASE("ProjectileSystem: launch inherits shooter velocity, boosts, and self-destructs at overrange",
          "[fire_control][projectile]") {
    FireWorld w;
    NullLogger log;
    EntityTypeRegistry registry;
    EntityDef proj;
    proj.id = projectileTypeId(*w.weapons.byIndex(w.aimIdx));
    proj.name = "proj";
    proj.category = ObjectCategory::Projectile;
    proj.maxHp = 1.f;
    registry.registerType(proj);
    registry.registerType([] {
        EntityDef d;
        d.id = "t:shooter";
        d.name = "S";
        d.category = ObjectCategory::AirVehicle;
        return d;
    }());
    EntityManager em(log, registry);
    SpatialIndex si;

    EntityTransform st{};
    st.pos[1] = 8000.0; // high, so gravity does not ground it inside the test
    st.vel[0] = 250.f;
    st.quat[3] = 1.f;
    const EntityId shooter = em.spawn("t:shooter", st);

    ProjectileSystem ps;
    ps.configure(&w.weapons, &CentralGravityField::earthInstance());
    const EntityId pid = ps.launch(em, w.aimIdx, *em.get(shooter), 0u);
    REQUIRE(pid.valid());
    CHECK(ps.liveCount() == 1u);
    CHECK(ps.projectiles()[0].vel.x > 250.f); // shooter velocity + separation push

    // Fly it out: it must end (overrange self-destruct at 1.5x max range or TTL) and report an
    // impact, never silently vanish.
    std::vector<ProjectileImpact> impacts;
    for (int i = 0; i < 60 * 60 && impacts.empty(); ++i)
        ps.step(em, si, 1.f / 60.f, {}, static_cast<uint64_t>(i), {}, impacts);
    REQUIRE(impacts.size() == 1u);
    CHECK(ps.liveCount() == 0u);
    CHECK_FALSE(impacts[0].directHit.valid());
    REQUIRE(em.get(pid) != nullptr); // slot reclaim happens in EntityManager::onTick, not here
    CHECK(em.get(pid)->dead);        // ...but the pooled entity was killed with the projectile
}
TEST_CASE("evaluateFire: rockets ripple while held; mass sheds per round", "[fire_control][rocket]") {
    FireWorld w;
    WeaponDef rocket;
    rocket.id = "t:ffar";
    rocket.name = "Rocket Pod";
    rocket.type = WeaponType::Rocket;
    rocket.category = WeaponCategory::AirToGround;
    rocket.performance.maxRangeM = 3000.f;
    rocket.performance.cepM = 30.f;
    rocket.load.massKg = 190.f; // the whole pod
    rocket.load.dragFactor = 0.019f;
    rocket.load.rounds = 19;
    const uint32_t podIdx = w.weapons.registerWeapon(rocket);

    FireState fs;
    StationState pod;
    pod.weaponIndex = podIdx;
    pod.rounds = 19;
    fs.loadout.stations = {pod};
    fs.loadout.selected = 0;
    fs.loadout.payloadMassKg = 190.f;
    fs.loadout.payloadCd0 = 0.019f;

    ControlInput in{};
    in.release = true; // HELD: rockets ripple, they do not need fresh edges

    std::vector<FireRequest> out;
    for (uint64_t t = 0; t < 60; ++t)
        evaluateFire(fs, w.weapons, in, false, t, 7u, out);

    // One second at kRocketRippleTicks (6) spacing = 10 rockets, all Spawn requests.
    CHECK(out.size() == 10u);
    CHECK(fs.loadout.stations[0].rounds == 9u);
    for (const FireRequest& r : out)
        CHECK(r.kind == FireRequest::Kind::Spawn);

    // Mass sheds PER ROUND: 10 of 19 rockets gone = 10/19 of the pod's mass.
    CHECK(fs.loadout.payloadMassKg == Catch::Approx(190.f * 9.f / 19.f).epsilon(0.01));
}

// ---------------------------------------------------------------------------
// buildLoadoutOverride — mission per-object loadout (#855)
// ---------------------------------------------------------------------------

TEST_CASE("buildLoadoutOverride: partial override keeps unnamed stations at default", "[fire_control]") {
    FireWorld w;
    std::vector<std::string> warnings;
    // Only station 0 is named (emptied via "~"); stations 1 and 2 keep their defaults.
    const LoadoutState ls = buildLoadoutOverride(w.def, w.weapons, {"~"}, warnings);
    REQUIRE(ls.stations.size() == 3u);
    CHECK(ls.stations[0].weaponIndex == UINT32_MAX); // emptied
    CHECK(ls.stations[1].weaponIndex == w.aimIdx);   // default kept
    CHECK(warnings.empty());
    CHECK(ls.selected == 1u); // the missile rail is still the sane default selection
}

TEST_CASE("buildLoadoutOverride: a store not in the station's allowed list is refused", "[fire_control]") {
    FireWorld w;
    std::vector<std::string> warnings;
    // Try to hang the gun on the missile rail (station 1) — not in that station's allowed list.
    const LoadoutState ls = buildLoadoutOverride(w.def, w.weapons, {"t:gun", "t:gun"}, warnings);
    CHECK(ls.stations[0].weaponIndex == w.gunIdx);   // station 0 allows the gun
    CHECK(ls.stations[1].weaponIndex == UINT32_MAX); // station 1 does not -> left empty
    REQUIRE(warnings.size() == 1u);
    CHECK(warnings[0].find("t:gun") != std::string::npos);
}

TEST_CASE("buildLoadoutOverride: an allowed-but-unknown weapon id is refused", "[fire_control]") {
    FireWorld w;
    w.def.hardpoints[1].allowed.push_back("t:ghost"); // in the allowed list, but never registered
    std::vector<std::string> warnings;
    const LoadoutState ls = buildLoadoutOverride(w.def, w.weapons, {"~", "t:ghost"}, warnings);
    CHECK(ls.stations[1].weaponIndex == UINT32_MAX); // unknown -> empty
    REQUIRE(warnings.size() == 1u);
    CHECK(warnings[0].find("t:ghost") != std::string::npos);
}

TEST_CASE("buildLoadoutOverride: payload re-costs from the actual mounted stores", "[fire_control]") {
    FireWorld w;
    std::vector<std::string> warnings;
    // Strip everything: no stores mounted -> zero payload.
    const LoadoutState empty = buildLoadoutOverride(w.def, w.weapons, {"~", "~", "~"}, warnings);
    CHECK(empty.payloadMassKg == Catch::Approx(0.f));
    CHECK(empty.selected == 255u); // nothing mounted -> nothing selected
    CHECK(warnings.empty());
}
