// SPDX-License-Identifier: GPL-3.0-or-later
//
// Bot turret gunner + per-instance skill (#966/#971): a defensive gunner acquires HONESTLY off the
// airframe's contact table, slews its turret onto the lead, and fires — and a higher rolled skill
// measurably tightens the aim and shortens the reaction. WB-level, like the surface-threat tests,
// because the whole point is sense -> designate -> slew -> fire through a real tick.

#include "ILogger.h"
#include "ai/PerInstanceSkill.h"
#include "ai/SeatControllers.h"
#include "entity/EntityManager.h"
#include "entity/EntityTypeRegistry.h"
#include "entity/IEntityController.h"
#include "net/WorldBroadcaster.h"
#include "weapon/WeaponRegistry.h"

#include "mock_network.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <set>

using namespace fl;

namespace {

struct NullLog : ILogger {
    void log(LogLevel, const char*, int, const char*) override {}
    void setMinLevel(LogLevel) override {}
    void flush() override {}
};

struct NeutralController : IEntityController {
    ControlInput sample(const EntityState&, uint64_t, double, const AiTickContext&) override {
        return ControlInput{};
    }
};

WeaponDef makeGun() {
    WeaponDef d;
    d.id = "test:gun";
    d.name = "Test Gun";
    d.type = WeaponType::Gun;
    d.category = WeaponCategory::AirToAir;
    d.performance.rateOfFireRpm = 1500.f;
    d.performance.maxRangeM = 2000.f;
    d.warhead.damage = 15.f;
    d.load.rounds = 4000;
    d.load.massKg = 0.f;
    return d;
}

// A bomber whose tail-gunner seat aims a full-traverse turret mounting a gun on station 1. The pilot
// seat is fly-only (never fires), so all damage is the gunner's. `skill` sets the gunner's rolled
// skill (the factory uses defaultSkill as a point range).
EntityDef makeGunBomberDef(float skill) {
    EntityDef d;
    d.id = "test:gunbomber";
    d.name = "Gun Bomber";
    d.category = ObjectCategory::AirVehicle;
    d.maxHp = 300.f;

    Hardpoint hp;
    hp.slot = 1;
    hp.allowed = {"test:gun"};
    hp.defaultWeapon = "test:gun";
    d.hardpoints = {hp};

    TurretDef t;
    t.id = "tail";
    t.azMinDeg = -180.f;
    t.azMaxDeg = 180.f;
    t.elMinDeg = -85.f;
    t.elMaxDeg = 85.f;
    t.slewRateDegS = 180.f; // fast so the reaction delay, not the slew, dominates the fire timing
    t.stations = {1};
    d.turrets = {t};

    SeatDef pilot;
    pilot.role = "pilot";
    pilot.capabilities = withCapability(CrewCapabilityMask{0}, CrewCapability::Fly);
    SeatDef gunner;
    gunner.role = "tail-gunner";
    gunner.capabilities = withCapability(CrewCapabilityMask{0}, CrewCapability::Fire);
    gunner.turret = "tail";
    gunner.botSpec = "gunner";
    gunner.defaultSkill = skill;
    d.crew = {pilot, gunner};
    return d;
}

// A plain target type the bomber can see and shoot.
EntityDef makeTargetDef() {
    EntityDef d;
    d.id = "test:target";
    d.name = "Target";
    d.category = ObjectCategory::AirVehicle;
    d.maxHp = 100000.f; // huge so it never dies during the window — we measure DAMAGE DEALT
    return d;
}

struct GunnerFixture {
    NullLog logger;
    TrackingNetwork net;
    EntityTypeRegistry registry;
    WeaponRegistry weapons;
    std::unique_ptr<EntityManager> em;
    std::unique_ptr<WorldBroadcaster> wb;
    EntityId targetId;
    EntityId bomberId;

    explicit GunnerFixture(float gunnerSkill) {
        weapons.registerWeapon(makeGun());
        registry.registerType(makeGunBomberDef(gunnerSkill));
        registry.registerType(makeTargetDef());

        em = std::make_unique<EntityManager>(logger, registry);
        wb = std::make_unique<WorldBroadcaster>(*em, registry, net, logger);
        wb->setWeaponRegistry(&weapons);
        wb->setGroundElevation(0.f);
        wb->setSensorCheckHz(60.f); // sense every tick — this is about the engagement, not cadence
        const EntityManager& emRef = *em;
        wb->setSeatControllerFactory(
            [&emRef](const SeatDef& sd, uint8_t i,
                     const WorldBroadcaster::SeatBotContext& ctx) -> std::unique_ptr<ISeatController> {
                return fl::ai::makeSeatController(sd, i, emRef, ctx.skillMin, ctx.skillMax, ctx.missionSeed);
            });

        // Bomber (faction 1) parked at the origin facing +X; a hostile target (faction 2) parked 150 m
        // dead ahead, inside the eyeball's forward cone and the turret's reach. Both on the ground, so
        // the gun's hitscan (a ray, not a falling projectile) resolves cleanly. At this range even a
        // low-skill gunner's biased aim still lands inside the target's hit radius (so first-hit timing
        // isolates the reaction delay), while an ace's tighter cone lands more rounds over the window.
        bomberId = spawnParked("test:gunbomber", 0.0, 0.0, /*faction=*/1, std::make_unique<NeutralController>());
        targetId = spawnParked("test:target", 150.0, 0.0, /*faction=*/2, std::make_unique<NeutralController>());
    }

    EntityId spawnParked(const char* type, double x, double z, uint16_t faction,
                         std::unique_ptr<IEntityController> ctrl) {
        EntityTransform t{};
        t.pos[0] = x;
        t.pos[1] = 0.1;
        t.pos[2] = z;
        t.quat[3] = 1.f;
        const EntityId id = em->spawn(type, t);
        if (EntityState* st = em->get(id))
            st->factionIndex = faction;
        wb->registerController(id, std::move(ctrl), nullptr, /*initialAirspeed=*/0.f);
        return id;
    }

    // Ticks and returns the tick at which the target FIRST took damage (0 = never), plus total damage.
    void run(uint64_t ticks, uint64_t& firstHitTick, float& damageDealt) {
        firstHitTick = 0;
        const float startHp = em->get(targetId)->hp;
        for (uint64_t i = 1; i <= ticks; ++i) {
            wb->onTick(1.0 / 60.0, i);
            if (firstHitTick == 0 && em->get(targetId)->hp < startHp)
                firstHitTick = i;
        }
        damageDealt = startHp - em->get(targetId)->hp;
    }
};

} // namespace

TEST_CASE("PerInstanceSkill: the roll is deterministic, in range, and varies by seed (#971)", "[gunner][skill]") {
    // Deterministic for a fixed (mission seed, object, seat).
    const uint64_t seed = fl::ai::skillSeed(1234, /*object=*/7, /*seat=*/1);
    CHECK(fl::ai::rollPerInstanceSkill(seed, 0.2f, 0.9f) == fl::ai::rollPerInstanceSkill(seed, 0.2f, 0.9f));

    // Always within [min, max].
    for (uint32_t obj = 0; obj < 50; ++obj) {
        const float s = fl::ai::rollPerInstanceSkill(fl::ai::skillSeed(42, obj, 0), 0.3f, 0.8f);
        CHECK(s >= 0.3f);
        CHECK(s <= 0.8f);
    }

    // A degenerate range returns the point.
    CHECK(fl::ai::rollPerInstanceSkill(seed, 0.55f, 0.55f) == 0.55f);

    // Different instances (object ids) generally roll different skills — a flight is not uniform.
    std::set<float> rolls;
    for (uint32_t obj = 0; obj < 32; ++obj)
        rolls.insert(fl::ai::rollPerInstanceSkill(fl::ai::skillSeed(9, obj, 0), 0.f, 1.f));
    CHECK(rolls.size() > 20u); // lots of distinct values, not one repeated skill
}

TEST_CASE("Bot gunner: acquires and fires on a hostile it honestly detects (#971)", "[gunner]") {
    GunnerFixture fx(/*skill=*/0.9f);
    uint64_t firstHit = 0;
    float dmg = 0.f;
    fx.run(600, firstHit, dmg);
    CHECK(firstHit > 0); // the gunner slewed onto the target and opened fire
    CHECK(dmg > 0.f);    // rounds are landing on the bandit
}

TEST_CASE("Bot gunner: higher skill fires sooner and hits harder (#971)", "[gunner][skill]") {
    GunnerFixture ace(/*skill=*/0.9f);
    GunnerFixture rookie(/*skill=*/0.3f);

    uint64_t aceFirst = 0;
    uint64_t rookieFirst = 0;
    float aceDmg = 0.f;
    float rookieDmg = 0.f;
    ace.run(600, aceFirst, aceDmg);
    rookie.run(600, rookieFirst, rookieDmg);

    REQUIRE(aceFirst > 0);
    REQUIRE(rookieFirst > 0);
    CHECK(aceFirst < rookieFirst); // the ace reacts faster (shorter reaction delay)
    CHECK(aceDmg > rookieDmg);     // and its tighter aim lands more rounds over the same window
}

TEST_CASE("Human gunner: masked-input gun fire lands on the target via the gunner-keyed path (#979)",
          "[gunner][crew]") {
    // Replace the bot gunner with a HUMAN in the tail seat: bind peer, feed a MsgClientInput that aims
    // the turret at the target dead ahead (+X, where the turret rests) and holds the gun trigger. The
    // masked-input fire path (#972) plus the gunner-keyed lag-comp rewind (#979) must land rounds.
    GunnerFixture fx(/*skill=*/0.9f);
    constexpr uint32_t kGunnerPeer = 3;
    // A human gunner is an ADMITTED peer: onConnect then its MsgConnectRequest (#853). Since #1069
    // the dispatch preamble drops client->server messages from a peer that never completed the
    // handshake, so binding a seat to a peer id that never connected no longer feeds it input.
    fx.wb->onConnect(kGunnerPeer);
    {
        fl::MsgConnectRequest req{};
        req.requestedRole = static_cast<uint8_t>(fl::PeerRole::Observer); // spawns no aircraft of its own
        fx.wb->onReceive(kGunnerPeer, &req, sizeof(req));
    }
    REQUIRE(fx.wb->setSeatOccupant(fx.bomberId, /*seat=*/1, kGunnerPeer));

    fl::MsgClientInput inp{};
    inp.seqNum = 1;
    inp.viewAxis[0] = 1.f; // aim +X — straight at the target 150 m dead ahead (the turret's rest bore)
    inp.viewAxis[1] = 0.f;
    inp.viewAxis[2] = 0.f;
    inp.buttons = 0x01; // gun trigger (level)
    fx.wb->onReceive(kGunnerPeer, &inp, sizeof(inp));

    uint64_t firstHit = 0;
    float dmg = 0.f;
    fx.run(300, firstHit, dmg);
    CHECK(firstHit > 0); // the human gunner's masked fire reached the seat's gun
    CHECK(dmg > 0.f);    // and lands on the bandit (the gunner-keyed rewind path is exercised)
}
