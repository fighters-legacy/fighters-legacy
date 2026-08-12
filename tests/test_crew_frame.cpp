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
#include "net/GameProtocol.h" // MsgClientInput (seat-scoped input #972)
#include "net/SeatInput.h"    // seatInputRouting / clampSeatStation (#972)
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
    gunner.damageHp = 50.f; // #978: the gunner seat is a damageable HP pool
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
        fl::WorldQueries q_wb;
        q_wb.seatControllerFactory = [gunnerFireAfter](const SeatDef&, uint8_t,
                                                       const SeatBotContext&) -> std::unique_ptr<ISeatController> {
            auto g = std::make_unique<StubGunner>();
            g->fireAfter = gunnerFireAfter;
            return g;
        };
        wb = std::make_unique<WorldBroadcaster>(*em, registry, net, logger, nullptr, std::move(q_wb));
        wb->setWeaponRegistry(&weapons);
        wb->setGroundElevation(0.f);
    }

    // Admit a peer the way a real one arrives (#853): onConnect, then its MsgConnectRequest. Joining
    // as an Observer spawns no aircraft of its own, so the seat binding is the only thing that gives
    // this peer an entity — which is exactly the human-gunner case. Required since #1069, where the
    // dispatch preamble drops every client->server message from a peer that has not completed the
    // handshake; before that a test could send input from a peer that had never connected at all.
    void admitPeer(uint32_t peerId) {
        wb->onConnect(peerId);
        MsgConnectRequest req{};
        req.requestedRole = static_cast<uint8_t>(PeerRole::Observer);
        wb->onReceive(peerId, &req, sizeof(req));
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
    // deliberately NO seat-controller factory in the queries

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

TEST_CASE("Seat input routing: capability mask decides which channels a seat drives (#972)", "[crew]") {
    using C = CrewCapability;
    const CrewCapabilityMask pilotCaps = withCapability(withCapability(CrewCapabilityMask{0}, C::Fly), C::Fire);
    const CrewCapabilityMask gunnerCaps = withCapability(CrewCapabilityMask{0}, C::Fire);

    // A Fly+Fire pilot drives flight and fire; without a turret, viewAxis is a look direction, not aim.
    const SeatInputRouting pilot = seatInputRouting(pilotCaps, /*aimsTurret=*/false);
    CHECK(pilot.driveFlight);
    CHECK(pilot.driveFire);
    CHECK_FALSE(pilot.aimTurret);

    // A Fire-only gunner NEVER drives flight — its elevator/throttle are masked off server-side. With a
    // turret, viewAxis becomes the turret aim command.
    const SeatInputRouting gunner = seatInputRouting(gunnerCaps, /*aimsTurret=*/true);
    CHECK_FALSE(gunner.driveFlight);
    CHECK(gunner.driveFire);
    CHECK(gunner.aimTurret);

    // A Fire seat with no turret does not treat viewAxis as an aim command.
    CHECK_FALSE(seatInputRouting(gunnerCaps, /*aimsTurret=*/false).aimTurret);

    // Station selection clamps to the seat's own partition — a gunner can't select the pilot's stores.
    CHECK(clampSeatStation(255, 2) == 255); // "keep" always passes
    CHECK(clampSeatStation(0, 2) == 0);
    CHECK(clampSeatStation(1, 2) == 1);
    CHECK(clampSeatStation(2, 2) == 255); // out of range -> keep
    CHECK(clampSeatStation(0, 0) == 255); // a seat with no stations rejects every selection
}

TEST_CASE("Crewed frame: a human gunner fires from its masked input, along its own aim (#972)", "[crew]") {
    // Bind a human peer to the NON-fly gunner seat, feed it a MsgClientInput that looks straight up and
    // holds fire (with flight fields set, which MUST be ignored), and verify the turret fires along the
    // human's aim — the seat-scoped input routing end to end.
    CrewFixture fx(/*gunnerFireAfter=*/90);
    const EntityId bomber = fx.spawnBomber(0.0);

    constexpr uint32_t kGunnerPeer = 7;
    fx.admitPeer(kGunnerPeer); // a human gunner is an admitted peer (#1069 gates un-admitted input)
    REQUIRE(fx.wb->setSeatOccupant(bomber, /*seat=*/1, kGunnerPeer));

    // Phase 1: look straight up (+Y), hold fire, with flight fields set (which MUST be masked off since
    // the gunner has no Fly capability). Let the turret slew fully onto +Y before opening fire.
    fl::MsgClientInput aim{};
    aim.seqNum = 1;
    aim.viewAxis[0] = 0.f;
    aim.viewAxis[1] = 1.f; // +Y: unmistakably not the airframe nose (+X)
    aim.viewAxis[2] = 0.f;
    aim.throttle = 1.0f; // flight fields — ignored server-side (a gunner does not fly the aircraft)
    aim.elevator = 1.0f; //
    fx.wb->onReceive(kGunnerPeer, &aim, sizeof(aim));
    fx.tick(90); // slew the turret onto +Y

    // Phase 2: hold fire (bit 2) — the store ripples along the now-slewed turret bore.
    fl::MsgClientInput fire = aim;
    fire.seqNum = 2;
    fire.buttons = 0x04;        // fire selected store
    fire.selectedStation = 255; // keep the seat's default station
    fx.wb->onReceive(kGunnerPeer, &fire, sizeof(fire));
    fx.tick(30);

    const auto vels = fx.projectileVels();
    REQUIRE(!vels.empty()); // the HUMAN gunner fired (its masked input reached the seat's fire channel)
    for (const glm::vec3& v : vels) {
        const float len = glm::length(v);
        REQUIRE(len > 1.f);
        CHECK(v.y / len > 0.9f);           // along the gunner's +Y aim (the turret bore)
        CHECK(std::abs(v.x) / len < 0.2f); // not the airframe nose
    }
}

TEST_CASE("Crewed frame: vacating a human seat resumes its bot (#972)", "[crew]") {
    // A human takes the gunner seat, then leaves; the authored bot must resume so the seat is not
    // silently disarmed. Fire the bot before and after the human tenancy.
    CrewFixture fx(/*gunnerFireAfter=*/90);
    const EntityId bomber = fx.spawnBomber(0.0);
    constexpr uint32_t kGunnerPeer = 9;

    fx.admitPeer(kGunnerPeer);
    REQUIRE(fx.wb->setSeatOccupant(bomber, 1, kGunnerPeer));
    fx.wb->clearSeatOccupant(kGunnerPeer); // human leaves immediately
    fx.tick(120);                          // the bot (StubGunner) slews then fires again after tick 90

    CHECK(!fx.projectileVels().empty()); // the bot resumed and fired
}

TEST_CASE("Crewed frame: a hit that knocks out the gunner seat silences its fire (#978)", "[crew]") {
    // The bomber's tail-gunner seat is a 50 hp damageable pool and is the ONLY damageable target (no
    // fixed [damage.subsystems]), so a warhead deterministically routes to it. A blast > 50 hp knocks
    // it out — the gunner must stop firing while the airframe (300 hp) survives.
    CrewFixture fx(/*gunnerFireAfter=*/90);
    const EntityId bomber = fx.spawnBomber(0.0);
    fx.tick(120); // the gunner slews + fires
    REQUIRE(!fx.projectileVels().empty());

    // Detonate a blast right on the bomber, past the 50 hp seat pool.
    const EntityState* st = fx.em->get(bomber);
    REQUIRE(st != nullptr);
    double blastPos[3] = {st->transform.pos[0], st->transform.pos[1] + 1.0, st->transform.pos[2]};
    BlastSpec blast{15.f, 80.f, false}; // 80 dmg > the 50 hp seat pool
    fx.wb->applyWarheadAt(blastPos, blast, EntityId::null());

    REQUIRE(fx.em->get(bomber) != nullptr); // the airframe survives (300 hp)

    // Clear the field and confirm the gunner fires no more (its seat is knocked out).
    fx.em->forEach([&](const EntityState& e) {
        if (!e.dead && e.typeIndex == fx.registry.indexById(projectileTypeId(*fx.weapons.byIndex(0)).c_str()))
            fx.em->kill(e.id);
    });
    fx.tick(120);
    CHECK(fx.projectileVels().empty()); // the knocked-out seat is silent
}

TEST_CASE("Crewed frame: knocking out the Fly seat silences the pilot's guns (#978)", "[crew]") {
    // A bomber whose PILOT (Fly+Fire) seat is a damageable pool and fires station 0 forward. Flown by a
    // controller that holds the trigger; spawned high so the nose-fired rockets survive. Knocking out
    // the Fly seat must zero the pilot input — the guns fall silent.
    NullLog logger;
    TrackingNetwork net;
    EntityTypeRegistry registry;
    WeaponRegistry weapons;
    weapons.registerWeapon(makeRocket());

    EntityDef d = makeBomberDef();
    d.crew[0].damageHp = 40.f; // the pilot (Fly+Fire) seat is damageable
    d.crew[1].damageHp = 0.f;  // the gunner seat is NOT, so the blast deterministically hits the pilot
    // Rebuild the registry with this modified def.
    registry.registerType(d);
    EntityDef proj;
    proj.id = projectileTypeId(*weapons.byIndex(0));
    proj.name = "p";
    proj.category = ObjectCategory::Projectile;
    proj.maxHp = 1.f;
    registry.registerType(proj);

    EntityManager em(logger, registry);
    fl::WorldQueries q_wb;
    q_wb.seatControllerFactory = [](const SeatDef&, uint8_t,
                                    const SeatBotContext&) -> std::unique_ptr<ISeatController> { return nullptr; };
    WorldBroadcaster wb(em, registry, net, logger, nullptr, std::move(q_wb));
    wb.setWeaponRegistry(&weapons);
    wb.setGroundElevation(0.f);
    // No gunner bot — only the pilot's fire matters here.

    struct FiringPilot : IEntityController {
        ControlInput sample(const EntityState&, uint64_t, double, const AiTickContext&) override {
            ControlInput c;
            c.release = true; // ripple the station-0 rockets forward
            return c;
        }
    };

    EntityTransform t{};
    t.pos[1] = 3000.0; // high, so nose-fired rockets fly free instead of hitting the ground
    t.quat[3] = 1.f;
    const EntityId bomber = em.spawn("test:bomber", t);
    wb.registerController(bomber, std::make_unique<FiringPilot>(), nullptr, 0.f);

    const uint32_t projType = registry.indexById(projectileTypeId(*weapons.byIndex(0)).c_str());
    auto liveProjectiles = [&]() {
        int n = 0;
        em.forEach([&](const EntityState& e) {
            if (!e.dead && e.typeIndex == projType)
                ++n;
        });
        return n;
    };

    uint64_t tick = 0;
    for (int i = 0; i < 30; ++i)
        wb.onTick(1.0 / 60.0, ++tick);
    REQUIRE(liveProjectiles() > 0); // the pilot is firing

    // Knock out the pilot (Fly) seat.
    const EntityState* st = em.get(bomber);
    double blastPos[3] = {st->transform.pos[0], st->transform.pos[1], st->transform.pos[2]};
    BlastSpec blast{15.f, 60.f, false};
    wb.applyWarheadAt(blastPos, blast, EntityId::null());

    // Clear the field; the silenced pilot fires no more.
    em.forEach([&](const EntityState& e) {
        if (!e.dead && e.typeIndex == projType)
            em.kill(e.id);
    });
    for (int i = 0; i < 60; ++i)
        wb.onTick(1.0 / 60.0, ++tick);
    CHECK(liveProjectiles() == 0); // the Fly seat is down — no pilot input, no guns
}

TEST_CASE("Crewed frame: a mission crew config can empty a bot seat, silencing its fire (#976)", "[crew]") {
    // The default bomber has a bot gunner that fires. A mission crew: block override that spawns seat 1
    // empty must silence it — no projectiles ever leave.
    CrewFixture fx(/*gunnerFireAfter=*/90);
    const EntityId bomber = fx.spawnBomber(0.0);

    WorldBroadcaster::CrewSpawnConfig cfg;
    cfg.missionSeed = 12345u;
    WorldBroadcaster::CrewSeatSpawnOverride ov;
    ov.seatIndex = 1;
    ov.empty = true;
    cfg.seats.push_back(ov);
    fx.wb->applyCrewSpawnConfig(bomber, cfg);

    fx.tick(120);
    CHECK(fx.projectileVels().empty()); // the emptied seat contributes no fire (bot resume is covered by #972)
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
