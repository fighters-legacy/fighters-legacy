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
#include "mock_log.h"
#include "net/WorldBroadcaster.h"
#include "sensor/BuiltinSensors.h"
#include "weapon/WeaponRegistry.h"

#include "mock_network.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>
#include <string>

using namespace fl;

namespace {

// Resolve the builtin sensor heads, exactly as makeSensorDefResolver does for fl-server.
//
// This used to answer for "builtin:sam-radar" ALONE, which quietly disabled half of what these tests
// look at: a SARH missile whose head (builtin:sarh-seeker) does not resolve fails the launch
// acquisition gate and FLIES DUMB. Every SAM shot in this file was therefore ballistic, and the one
// test that existed passed only because its target sat dead ahead on the launcher's boresight, where
// a dumb store arrives anyway. Resolving the whole builtin set is what makes a miss here mean
// something. (Found while fixing #1204 — an under-wired harness reading as engine behaviour.)
std::shared_ptr<const sensor::SensorDef> builtinSensorResolver(const std::string& id) {
    for (const sensor::SensorDef* builtin :
         {&sensor::BuiltinSensors::eyeball(), &sensor::BuiltinSensors::irSeeker(),
          &sensor::BuiltinSensors::radarSeeker(), &sensor::BuiltinSensors::sarhSeeker(),
          &sensor::BuiltinSensors::groundRadar(), &sensor::BuiltinSensors::interceptRadar()})
        if (id == builtin->id)
            return {std::shared_ptr<const sensor::SensorDef>{}, builtin};
    return nullptr;
}

// A WB armed and wired the way fl-server wires it for the sandbox.
struct ThreatFixture {
    NullLogger logger;
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
        //
        // The geometry is fl-server's SHIPPED default (server_config.h FlightConfig:
        // designate_range_m = 15000, designate_half_angle_deg = 15), not a permissive stand-in. It
        // used to be 60 km and a full hemisphere, which quietly made this fixture a different engine
        // from the one that ships: #1208 -- a battery that designates properly, throws the id away,
        // and has the fire path re-designate from its horizontal nose through the boresight cone --
        // could not fail here, because in this fixture that cone swallowed the whole world. Every
        // SAM test in the file passed while no missile in a real server arrived within kilometres.
        q_wb.targetDesignator = [this](const EntityState& shooter, const float axis[3]) -> EntityId {
            return ai::designateFromContacts(shooter, axis, wb->contactsFor(shooter.id.index), 15000.f,
                                             15.f * std::numbers::pi_v<float> / 180.f, nullptr);
        };
        wb = std::make_unique<WorldBroadcaster>(*em, registry, net, logger, nullptr, std::move(q_wb));
        wb->setWeaponRegistry(&weapons);
        wb->setSensorCheckHz(60.f); // sense every tick — these tests are about the engagement, not cadence
        // Launch designation the way fl-server wires it: a shooter designates the nearest hostile it
        // has an honest track on (so a SAM's SARH guides at the aircraft instead of flying dumb).
    }

    // Spawn facing +X (identity). There used to be a `noseUp` option here that pitched an emplacement
    // to point at +Y, described as "the vertical-launcher geometry a ground SAM needs so its missile
    // climbs instead of flying into the dirt off the rail" -- i.e. a hand-applied workaround for
    // #1204, in the fixture, for a defect nothing asserted. The controller elevates its own launcher
    // now, so an emplacement is spawned the way a mission places one: level.
    //
    // The fixture wires no groundElevation query, so the terrain is the y = 0 plane: y == 0 IS the
    // deck, and that is what makes the ground tests below ground tests.
    EntityId spawn(const char* type, double x, double y, double z, uint16_t faction) {
        EntityTransform t{};
        t.pos[0] = x;
        t.pos[1] = y;
        t.pos[2] = z;
        t.quat[3] = 1.f; // identity: nose along +X
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
    // range and the launcher's forward cone. This one stays at altitude on purpose: it is the
    // acquire-and-launch test, and the ground cases below own the launch geometry (#1204).
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

// ── ground launch geometry (#1204) ───────────────────────────────────────────────────────────────
//
// The defect these pin: a SAM emplaced ON the terrain fired along its airframe nose, which for an
// emplacement standing on flat ground is horizontal. ProjectileSystem::launch offsets the store 12 m
// along that vector at the launcher's own altitude, so the missile began its life AT deck level and
// the ground check in step() ended it within a few steps. Measured on builtin:sam-site against a
// target at 3 km altitude, before the fix -- the store's lifetime in ticks, and whether the target
// was ever hit:
//
//     range     on the deck        at 600 m
//      4 km     443, killed        443, killed     <- steep LOS: it climbed off the rail anyway
//      8 km      39, UNTOUCHED     622, damaged
//     12 km      28, UNTOUCHED     846, killed
//     20 km      24, UNTOUCHED    1431, killed
//     28 km      24, UNTOUCHED    2361, damaged
//
// Nothing in this file caught it, for two compounding reasons: every emplacement was tested at
// altitude (the comment saying so called a ground SAM's launch geometry someone else's concern), and
// the fixture's sensor resolver did not know the missile's own seeker, so the shots were ballistic
// and only ever aimed at boresight targets. Both are fixed above.
//
// The assertion is PARITY, not an absolute: the same engagement is run from the deck and from
// altitude, and the emplacement on the deck must do what the one in the air does. That is the honest
// claim -- "a ground SAM is not worse off than an airborne one" -- and it does not silently re-pass
// if the missile, the seeker or the flight model is later retuned.

namespace {

// Run one engagement and report whether the target was hit at all, plus how long the store lived.
// The per-tick sample is deliberate: a launched missile is gone within seconds, so an end-of-run
// count reads identically to never firing. #1204 records that exact mistake costing an hour.
struct EngagementResult {
    int peakMissiles{0};
    int ticksAlive{0};
    bool targetHit{false};
};

EngagementResult runSamEngagement(double samAltM, double rangeM, double targetAltAboveSamM, int ticks) {
    ThreatFixture f;
    const EntityId sam = f.spawn("builtin:sam-site", 0.0, samAltM, 0.0, 2);
    const EntityId victim = f.spawn("builtin:debug-entity", rangeM, samAltM + targetAltAboveSamM, 0.0, 1);
    f.wb->registerController(sam, std::make_unique<ai::SamEngagementController>(*f.em));

    const EntityState* vs0 = f.em->get(victim);
    const float startHp = vs0 ? vs0->hp : 0.f;

    EngagementResult r;
    uint64_t t = 0;
    for (int i = 0; i < ticks; ++i) {
        f.tick(1, t);
        const int n = f.liveOfType("projectile:builtin:sarh-missile");
        r.peakMissiles = std::max(r.peakMissiles, n);
        if (n > 0)
            ++r.ticksAlive;
    }
    const EntityState* vs = f.em->get(victim);
    r.targetHit = (vs == nullptr) || (vs->hp < startHp); // reaped = destroyed outright
    return r;
}

} // namespace

TEST_CASE("a SAM emplaced on the ground gets its missile away and kills (#1204)", "[surface_threats]") {
    // 12 km is past the range where a nose-level launch survived at all: before the fix the store
    // lived 28 ticks and the aircraft was untouched.
    const EngagementResult ground = runSamEngagement(/*samAltM=*/0.0, /*rangeM=*/12000.0,
                                                     /*targetAltAboveSamM=*/3000.0, /*ticks=*/1800);
    CHECK(ground.peakMissiles >= 1); // it always got this far -- the trigger was never the problem
    CHECK(ground.ticksAlive > 120);  // it now survives its opening steps instead of being reaped
    CHECK(ground.targetHit);         // and arrives -- the one observable no test looked at
}

TEST_CASE("a ground SAM engages as well as an airborne one, across the envelope (#1204)", "[surface_threats]") {
    // The emplacement on the deck and the identical one 600 m up must behave the same. Anything the
    // airborne launcher cannot do (28 km is a long shot for this missile inside the run window) is
    // not held against the ground one -- the claim is parity, not lethality at every range.
    for (const double rangeM : {8000.0, 12000.0, 20000.0}) {
        CAPTURE(rangeM);
        const EngagementResult ground = runSamEngagement(0.0, rangeM, 3000.0, 2400);
        const EngagementResult airborne = runSamEngagement(600.0, rangeM, 3000.0, 2400);
        CHECK(ground.peakMissiles == airborne.peakMissiles);
        CHECK(ground.targetHit == airborne.targetHit);
        // Before the fix this was 24-39 ticks on the deck against 622-1431 in the air. Half is a
        // loose bound on purpose: it fails on a reaped store and does not chase small tuning drift.
        CHECK(ground.ticksAlive >= airborne.ticksAlive / 2);
    }
}

TEST_CASE("a SAM launches above the horizon even at a target on its own level (#1204)", "[surface_threats]") {
    // The elevation FLOOR, at the geometry that most needs it: a target at the launcher's own
    // altitude, where aiming straight at it would put the store back on the deck. The controller
    // must still command a climbing launch vector.
    ThreatFixture f;
    const EntityId sam = f.spawn("builtin:sam-site", 0.0, 0.0, 0.0, 2);
    f.spawn("builtin:debug-entity", 20000.0, 0.0, 0.0, 1);

    ai::SamEngagementController controller(*f.em, /*engageRangeM=*/30000.f, /*coneHalfAngleDeg=*/90.f,
                                           /*fireIntervalS=*/4.f, /*launchElevationMinDeg=*/35.f);
    f.wb->registerController(sam, std::make_unique<ai::SamEngagementController>(*f.em, 30000.f, 90.f, 4.f, 35.f));

    uint64_t t = 0;
    f.tick(120, t); // let the battery acquire and hold a lock

    // Sample the controller directly against the world the fixture built, so the assertion is about
    // the commanded vector rather than about where the missile happened to end up.
    const EntityState* st = f.em->get(sam);
    REQUIRE(st != nullptr);
    fl::AiTickContext ctx;
    ctx.contacts = f.wb->contactsFor(st->id.index);
    REQUIRE(ctx.contacts != nullptr);

    bool sawLaunch = false;
    for (uint64_t i = 0; i < 300 && !sawLaunch; ++i) {
        const ControlInput ctrl = controller.sample(*st, i, 1.0 / 60.0, ctx);
        if (!ctrl.release)
            continue;
        sawLaunch = true;
        REQUIRE(ctrl.hasAimDir);
        // The fixture's world is the y = 0 plane, so local up is +Y and the elevation is aimDir.y.
        const float elevSin = ctrl.aimDir[1];
        CHECK(elevSin > 0.f);                                                         // never into the dirt
        CHECK(elevSin >= std::sin(35.f * std::numbers::pi_v<float> / 180.f) - 1e-3f); // at or above the floor
    }
    CHECK(sawLaunch);
}

// ── launch designation (#1208) ────────────────────────────────────────────────────────────────────
//
// #1204 got the store off the rail. It did not get it to the target: the controller designated with
// its own 30 km / +/-90 deg engagement geometry and then threw the id away, so `executeFireRequest`
// re-designated from scratch off `bodyForward(quat)` -- the emplacement's HORIZONTAL nose -- through
// the server's boresight cone (15 km / +/-15 deg). Measured from the issue, target at 3 km altitude:
//
//     horizontal range   elevation off the nose   designated?
//      2.6 km             ~49 deg                 no  - outside the cone
//      8   km             ~21 deg                 no
//     11.6 km             ~15 deg                 marginal
//     20   km             ~8.5 deg                inside the cone, outside the 15 km range cap
//
// Everywhere else the store launched with NO designation and flew ballistically: 0 kills across
// altitude 1-9 km, engagement range 4-30 km, target speed 100-320 m/s, SARH and IR alike, with
// closest approaches of 3.7-24.7 km. Aircraft were unaffected because fighter.lua puts the nose on
// the target before firing, which satisfies the nose cone by construction.
//
// The two cases below are the two ways the old fallback failed -- angle and range -- and they only
// mean something because the fixture's designator now carries the SHIPPED cone (see ThreatFixture).

TEST_CASE("a ground SAM kills a target its own nose cone would never designate (#1208)", "[surface_threats]") {
    SECTION("well outside the boresight cone: 49 degrees above the nose") {
        // 2.6 km out, 3 km up. Inside the launcher's engagement geometry, nowhere near its nose.
        const EngagementResult r = runSamEngagement(/*samAltM=*/0.0, /*rangeM=*/2600.0,
                                                    /*targetAltAboveSamM=*/3000.0, /*ticks=*/1800);
        CHECK(r.peakMissiles >= 1); // it always shot -- the trigger was never the problem
        CHECK(r.targetHit);         // and now it arrives
    }
    SECTION("inside the cone but beyond the boresight designation range") {
        // 20 km out, 3 km up: ~8.5 deg off the nose, so the angle gate passed and the 15 km range
        // cap refused it anyway.
        const EngagementResult r = runSamEngagement(0.0, 20000.0, 3000.0, 2400);
        CHECK(r.peakMissiles >= 1);
        CHECK(r.targetHit);
    }
}

TEST_CASE("SamEngagementController publishes the target it designated (#1208)", "[surface_threats]") {
    ThreatFixture f;
    const EntityId sam = f.spawn("builtin:sam-site", 0.0, 0.0, 0.0, 2);
    const EntityId victim = f.spawn("builtin:debug-entity", 2600.0, 3000.0, 0.0, 1);
    ai::SamEngagementController controller(*f.em);
    f.wb->registerController(sam, std::make_unique<ai::SamEngagementController>(*f.em));

    uint64_t t = 0;
    f.tick(120, t); // let the battery acquire and hold a track

    const EntityState* st = f.em->get(sam);
    REQUIRE(st != nullptr);
    fl::AiTickContext ctx;
    ctx.contacts = f.wb->contactsFor(st->id.index);
    REQUIRE(ctx.contacts != nullptr);

    CHECK_FALSE(controller.designatedTarget().valid()); // nothing sampled yet
    controller.sample(*st, 0, 1.0 / 60.0, ctx);
    CHECK(controller.designatedTarget() == victim); // the id the controller chose, kept

    // A controller that sees nothing publishes nothing: a designation never outlives its engagement.
    fl::AiTickContext blind;
    controller.sample(*st, 1, 1.0 / 60.0, blind);
    CHECK_FALSE(controller.designatedTarget().valid());
}

TEST_CASE("a plain controller designates nothing, so the fire path decides (#1208)", "[surface_threats]") {
    // The seam defaults OFF, the same way ControlInput::hasAimDir does: every controller that does
    // not override designatedTarget() leaves the shooter's own look-axis designation in charge.
    struct SilentController : fl::IEntityController {
        fl::ControlInput sample(const fl::EntityState&, uint64_t, double, const fl::AiTickContext&) override {
            return {};
        }
    };
    SilentController c;
    CHECK_FALSE(c.designatedTarget().valid());
}

TEST_CASE("an aircraft's shot still leaves along its nose (#1204 regression guard)", "[surface_threats]") {
    // The aim seam defaults OFF: ControlInput::hasAimDir is false for every controller that does not
    // set it, so a nose-mounted store is unaffected by all of the above. This pins that the SAM's
    // launch vector did not become everyone's.
    const ControlInput fresh{};
    CHECK_FALSE(fresh.hasAimDir);
    CHECK(fresh.aimDir[0] == 0.f);
    CHECK(fresh.aimDir[1] == 0.f);
    CHECK(fresh.aimDir[2] == 0.f);
}
