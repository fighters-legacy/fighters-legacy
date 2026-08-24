// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "ILogger.h"
#include "content/ContentBootstrap.h"
#include "entity/DeckDef.h"
#include "entity/EntityManager.h"
#include "entity/EntityTypeRegistry.h"
#include "entity/IEntityController.h"
#include "flight/BuiltinFlightModel.h"
#include "flight/FlightIntegrator.h"
#include "flight/FlightModelParser.h"
#include "flight/ForceModelSelect.h"
#include "mock_log.h"
#include "mock_network.h"
#include "net/WorldBroadcaster.h"

#include <cmath>
#include <memory>
#include <string>

using namespace fl;

namespace {

// A controller that returns a fixed input every tick.
struct ConstCtl : IEntityController {
    ControlInput in{};
    explicit ConstCtl(float throttle = 0.f) {
        in.throttle = throttle;
    }
    ControlInput sample(const EntityState&, uint64_t, double, const AiTickContext&) override {
        return in;
    }
};

// A deliberately gutless fixed-wing model: the catapult, not the engine, must supply the launch
// energy — so a catapult test that reaches flying speed proves the catapult did it.
std::shared_ptr<FlightModelData> gutlessAircraft() {
    static const std::string toml = R"(
[aircraft]
name        = "Gutless"
type        = "fighter"
engine_type = "turbofan"

[flight_model]
mass_kg      = 10000.0
wing_area_m2 = 35.0
wingspan_m   = 10.0
mac_m        = 3.5
fuel_kg      = 2000.0
ixx_kg_m2    = 10000.0
iyy_kg_m2    = 70000.0
izz_kg_m2    = 78000.0

[aero.cl_table]
alpha  = [-5.0, 0.0, 5.0, 10.0, 15.0, 18.0, 20.0, 25.0]
mach   = [0.3, 0.9]
values = [-0.2,-0.24, 0.05,0.07, 0.4,0.52, 0.75,0.97, 1.05,1.36, 1.18,1.52, 1.1,1.42, 0.85,1.1]

[aero.drag_polar]
cd0 = 0.018
k = 0.14
speedbrake_cd = 0.08
gear_cd = 0.03

[aero.moments]
cm_alpha = -0.7
cm_q = -10.0
cm_de = -1.0
cl_beta = -0.08
cl_p = -0.4
cl_da = 0.07
cn_beta = 0.1
cn_r = -0.12
cn_dr = -0.05

[aero.limits]
alpha_stall_deg = 18.0
max_g_structural = 8.0
min_g_structural = -3.0
max_mach = 1.6

[aero.controls]
max_elevator_deg = 25.0
max_aileron_deg = 20.0
max_rudder_deg = 30.0

[engine]
fuel_flow_idle_kg_s = 0.01
fuel_flow_mil_kg_s = 0.1
fuel_flow_ab_kg_s = 0.3
spool_time_s = 1.0

[engine.mil_thrust]
mach   = [0.0, 0.9]
alt_km = [0.0, 12.0]
values = [0.5, 0.3, 0.5, 0.3]
)";
    return std::make_shared<FlightModelData>(parseFlightModel(toml));
}

EntityDef airDef(const char* id = "test:air") {
    EntityDef d;
    d.id = id;
    d.name = "Air";
    d.category = ObjectCategory::AirVehicle;
    d.maxHp = 100.f;
    return d;
}

} // namespace

// ── DeckDef pure math ────────────────────────────────────────────────────────

TEST_CASE("Deck: deckLocalPoint transforms into the ship frame", "[carrier][deck]") {
    DeckDef deck; // 330 x 75, height 20
    const double ship[3] = {100.0, 0.0, 200.0};
    const float identity[4] = {0.f, 0.f, 0.f, 1.f};

    const double onDeck[3] = {110.0, 20.0, 205.0};
    auto p = deckLocalPoint(onDeck, ship, identity, deck);
    CHECK(p.x == Catch::Approx(10.f));
    CHECK(p.y == Catch::Approx(20.f));
    CHECK(p.z == Catch::Approx(5.f));
    CHECK(p.inFootprint);

    const double offBeam[3] = {110.0, 20.0, 300.0}; // 100 m abeam — outside the 75 m width
    CHECK_FALSE(deckLocalPoint(offBeam, ship, identity, deck).inFootprint);

    // Yaw the ship 90 deg about +Y: its bow (+X body) now points at world -Z.
    const float halfSqrt2 = 0.70710678f;
    const float yawed[4] = {0.f, halfSqrt2, 0.f, halfSqrt2};
    const double aheadOfBow[3] = {100.0, 20.0, 150.0}; // 50 m toward world -Z
    auto q = deckLocalPoint(aheadOfBow, ship, yawed, deck);
    CHECK(q.x == Catch::Approx(50.f).margin(0.01));
    CHECK(std::abs(q.z) < 0.01f);
}

TEST_CASE("Deck: the floor applies on the deck, never under the bow", "[carrier][deck]") {
    DeckDef deck;
    DeckLocalPoint onDeck{10.f, 20.f, 0.f, true};
    CHECK(deckFloorApplies(onDeck, deck));
    DeckLocalPoint underBow{10.f, 5.f, 0.f, true}; // flying under the overhang at 5 m
    CHECK_FALSE(deckFloorApplies(underBow, deck));
    DeckLocalPoint offDeck{500.f, 20.f, 0.f, false};
    CHECK_FALSE(deckFloorApplies(offDeck, deck));
}

// ── vessel model (#38) ───────────────────────────────────────────────────────

TEST_CASE("Vessel: [vessel] parses and the ship tops out at its declared speed", "[carrier][vessel]") {
    auto data = std::make_shared<FlightModelData>(parseFlightModel(R"(
[aircraft]
name = "Test Ship"
type = "vessel"

[flight_model]
mass_kg   = 1000000.0
fuel_kg   = 0.0
ixx_kg_m2 = 1.0e9
iyy_kg_m2 = 1.0e10
izz_kg_m2 = 1.0e10

[vessel]
max_thrust_n  = 2000000.0
max_speed_mps = 15.0
)"));
    REQUIRE(data->vessel.has_value());
    CHECK(data->isVessel());
    CHECK_FALSE(data->isFixedWing());

    FlightIntegrator fi(data);
    applyForceModelFor(fi, *data);
    FlightState s{};
    s.mass_kg = 1000000.f;
    s.throttle_actual = 1.f;
    fi.reset(s);
    ControlInput full{};
    full.throttle = 1.f;
    for (int i = 0; i < 60 * 120; ++i) // 2 min at full ahead
        fi.step(1.f / 60.f, full, {});
    CHECK(fi.state().vel_body[0] > 10.0); // it sails
    CHECK(fi.state().vel_body[0] < 16.5); // ...and tops out at ~max_speed, not beyond
}

TEST_CASE("Vessel: the rudder needs steerage way", "[carrier][vessel]") {
    auto data = std::make_shared<FlightModelData>(parseFlightModel(R"(
[aircraft]
name = "Test Ship"
type = "vessel"

[flight_model]
mass_kg   = 1000000.0
fuel_kg   = 0.0
ixx_kg_m2 = 1.0e9
iyy_kg_m2 = 1.0e10
izz_kg_m2 = 1.0e10

[vessel]
max_thrust_n  = 2000000.0
max_speed_mps = 15.0
turn_rate_deg_s = 2.0
)"));
    ControlInput helm{};
    helm.rudder = 1.f; // hard a-starboard

    // Under way: turns (nose right = negative omega[1]).
    FlightIntegrator underway(data);
    applyForceModelFor(underway, *data);
    FlightState s{};
    s.mass_kg = 1000000.f;
    s.vel_body[0] = 10.0;
    underway.reset(s);
    for (int i = 0; i < 300; ++i)
        underway.step(1.f / 60.f, helm, {});
    CHECK(underway.state().omega[1] < -0.005f);

    // Dead in the water: the helm does nothing.
    FlightIntegrator stopped(data);
    applyForceModelFor(stopped, *data);
    FlightState s2{};
    s2.mass_kg = 1000000.f;
    stopped.reset(s2);
    for (int i = 0; i < 300; ++i)
        stopped.step(1.f / 60.f, helm, {});
    CHECK(std::abs(stopped.state().omega[1]) < 0.002f);
}

TEST_CASE("Builtin carrier: def + vessel model are coherent", "[carrier]") {
    const EntityDef def = builtinCarrierDef();
    CHECK(def.id == "builtin:carrier");
    CHECK(def.category == ObjectCategory::NavalVehicle);
    CHECK(def.acceptsLandings);
    REQUIRE(def.deck.has_value());
    CHECK(def.deck->lengthM > 300.f);
    CHECK(def.flightModelAsset == "builtin:carrier-vessel");
    auto model = BuiltinCarrierVesselModel::get();
    CHECK(model->isVessel());
    REQUIRE(model->vessel.has_value());
}

// ── WorldBroadcaster deck physics ────────────────────────────────────────────

namespace {
struct CarrierWorld {
    NullLogger log;
    NullNetwork net;
    EntityTypeRegistry registry;
    EntityManager em;
    WorldBroadcaster wb;
    EntityId carrier;

    CarrierWorld() : em(log, registry), wb(em, registry, net, log) {
        registry.registerType(builtinCarrierDef());
        registry.registerType(airDef());
        wb.setGroundElevation(0.f);
        EntityTransform t{};
        t.quat[3] = 1.f;
        carrier = em.spawn("builtin:carrier", t);
    }

    EntityId spawnAir(double x, double y, double z, std::unique_ptr<IEntityController> ctl,
                      std::shared_ptr<FlightModelData> model, float airspeed) {
        EntityTransform t{};
        t.quat[3] = 1.f;
        t.pos[0] = x;
        t.pos[1] = y;
        t.pos[2] = z;
        EntityId id = em.spawn("test:air", t);
        wb.registerController(id, std::move(ctl), std::move(model), airspeed);
        return id;
    }

    void tick(int n) {
        for (int i = 0; i < n; ++i)
            wb.onTick(1.0 / 60.0, static_cast<uint64_t>(i + 1));
    }
};
} // namespace

TEST_CASE("Carrier: an aircraft dropped over the deck settles ON the deck, not the sea", "[carrier][wb]") {
    CarrierWorld w;
    // Stationary carrier at the origin; aircraft released 10 m above the deck plane (deck at 20 m).
    w.wb.registerController(w.carrier, std::make_unique<ConstCtl>(0.f), BuiltinCarrierVesselModel::get(), 0.f);
    const EntityId air = w.spawnAir(0.0, 30.0, 5.0, std::make_unique<ConstCtl>(0.f), gutlessAircraft(), 0.f);
    w.tick(600); // 10 s to settle
    const EntityState* st = w.em.get(air);
    REQUIRE(st != nullptr);
    CHECK(st->transform.pos[1] > 18.0); // on the deck plane (~20 m), NOT the sea at 0
    CHECK(st->transform.pos[1] < 22.0);
}

TEST_CASE("Carrier: a parked aircraft is carried by the steaming ship", "[carrier][wb]") {
    CarrierWorld w;
    // Carrier at full ahead; aircraft parked amidships-aft, engine idle.
    w.wb.registerController(w.carrier, std::make_unique<ConstCtl>(1.f), BuiltinCarrierVesselModel::get(), 0.f);
    const EntityId air = w.spawnAir(-50.0, 20.5, 0.0, std::make_unique<ConstCtl>(0.f), gutlessAircraft(), 0.f);
    w.tick(60 * 30); // 30 s: the carrier works up to several m/s
    const EntityState* ship = w.em.get(w.carrier);
    const EntityState* st = w.em.get(air);
    REQUIRE(ship != nullptr);
    REQUIRE(st != nullptr);
    CHECK(ship->transform.pos[0] > 20.0); // the ship went somewhere
    // The parked aircraft went WITH it: still within a deck length of the ship's origin.
    CHECK(std::abs(st->transform.pos[0] - (ship->transform.pos[0] - 50.0)) < 40.0);
}

// NOTE: no semicolons in TEST_CASE names — Catch2's CMake discovery script expands the test listing
// unquoted, so a ';' in a name splits the JSON as a CMake list and fails the build at discovery.
TEST_CASE("Carrier: the catapult throws a gutless aircraft to flying speed (parked off the stroke it stays put)",
          "[carrier][wb]") {
    // ON the stroke (local x 30..130), military power: the catapult does the work.
    {
        CarrierWorld w;
        w.wb.registerController(w.carrier, std::make_unique<ConstCtl>(0.f), BuiltinCarrierVesselModel::get(), 0.f);
        const EntityId air = w.spawnAir(40.0, 20.5, 0.0, std::make_unique<ConstCtl>(1.f), gutlessAircraft(), 0.f);
        w.tick(60 * 4); // the ~100 m stroke completes in under 3 s
        const EntityState* st = w.em.get(air);
        REQUIRE(st != nullptr);
        const float spd =
            std::sqrt(st->transform.vel[0] * st->transform.vel[0] + st->transform.vel[2] * st->transform.vel[2]);
        // A cat shot happens with the GEAR DOWN — and since #842 gear position is real drag, so the
        // end-of-stroke speed is a little lower than when the gear was aerodynamically invisible.
        CHECK(spd > 45.f); // shot off the bow — its own 1 kN of thrust could never do this
    }
    // AFT of the stroke at the same power: nothing shoots it, and 1 kN cannot accelerate 10 t.
    {
        CarrierWorld w;
        w.wb.registerController(w.carrier, std::make_unique<ConstCtl>(0.f), BuiltinCarrierVesselModel::get(), 0.f);
        const EntityId air = w.spawnAir(-60.0, 20.5, 0.0, std::make_unique<ConstCtl>(1.f), gutlessAircraft(), 0.f);
        w.tick(60 * 8);
        const EntityState* st = w.em.get(air);
        REQUIRE(st != nullptr);
        const float spd =
            std::sqrt(st->transform.vel[0] * st->transform.vel[0] + st->transform.vel[2] * st->transform.vel[2]);
        CHECK(spd < 10.f);
    }
}

TEST_CASE("Carrier: a touchdown in the wires at trap speed is dragged to a stop", "[carrier][wb]") {
    CarrierWorld w;
    w.wb.registerController(w.carrier, std::make_unique<ConstCtl>(0.f), BuiltinCarrierVesselModel::get(), 0.f);
    // Rolling onto the wires: 1 m above the deck plane, 60 m/s, just short of the wire zone
    // (zone is x in [-130, -90]).
    const EntityId air = w.spawnAir(-140.0, 21.0, 0.0, std::make_unique<ConstCtl>(0.f), gutlessAircraft(), 60.f);
    w.tick(60 * 6);
    const EntityState* st = w.em.get(air);
    REQUIRE(st != nullptr);
    const float spd =
        std::sqrt(st->transform.vel[0] * st->transform.vel[0] + st->transform.vel[2] * st->transform.vel[2]);
    CHECK(spd < 3.f);                   // stopped in the wires...
    CHECK(st->transform.pos[0] < 60.0); // ...well before running off the bow (140 m of deck remained)
    CHECK(st->transform.pos[1] > 18.0); // and still ON the deck
}
