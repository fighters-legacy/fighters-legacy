// SPDX-License-Identifier: GPL-3.0-or-later
#include "ILogger.h"
#include "atc/AtcFacility.h"
#include "atc/AtcService.h"
#include "entity/EntityDef.h"
#include "entity/EntityManager.h"
#include "entity/EntityState.h"
#include "entity/EntityTypeRegistry.h"
#include "flight/Geodetic.h"
#include "mock_log.h"
#include "world/AirportRegistry.h"
#include "world/BuiltinAirport.h"

#include <catch2/catch_test_macros.hpp>
#include <vector>

using namespace fl;

namespace {

fl::EntityDef basicDef() {
    fl::EntityDef d;
    d.id = "test:basic";
    d.name = "Basic";
    d.category = fl::ObjectCategory::AirVehicle;
    d.maxHp = 100.f;
    return d;
}

// A runway at the world origin, heading 090 (along +X), field elevation 0.
fl::atc::AtcFacility::Runway originRunway() {
    fl::atc::AtcFacility::Runway r;
    r.threshold = {0.0, 0.0, 0.0};
    r.oppositeEnd = {2500.0, 0.0, 0.0};
    r.centerlineDir = {1.0, 0.0, 0.0};
    r.headingDeg = 90.f;
    r.elevationM = 0.0;
    return r;
}

fl::atc::AtcFacility makeFacility(bool acceptsLandings = true) {
    return fl::atc::AtcFacility("test:field", "Test Tower", originRunway(), fl::kEarthRadiusM, acceptsLandings,
                                [] { return fl::atc::FacilityPose{{0, 0, 0}, 90.f}; });
}

} // namespace

TEST_CASE("AtcFacility: two departures are sequenced with disjoint runway occupancy", "[atc]") {
    // #673 criterion 1, as a deterministic unit test.
    NullLogger log;
    fl::EntityTypeRegistry reg;
    reg.registerType(basicDef());
    fl::EntityManager em(log, reg);

    fl::EntityTransform t{};
    t.quat[3] = 1.f; // on the ground at the origin (AGL 0)
    fl::EntityId a = em.spawn("test:basic", t);
    fl::EntityId b = em.spawn("test:basic", t);

    auto fac = makeFacility();
    std::vector<fl::atc::RadioTransmission> outbox;
    fac.requestTakeoff(a);
    fac.requestTakeoff(b);

    // First ATC step: A takes the runway, B keeps holding short.
    fac.update(em, /*tick=*/60, outbox);
    CHECK(fac.clearanceState(a) == fl::atc::ClearanceState::ClearedTakeoff);
    CHECK(fac.clearanceState(b) == fl::atc::ClearanceState::HoldShort);
    CHECK(fac.occupant() == a);
    // A "cleared for takeoff" transmission was emitted for A only.
    REQUIRE(outbox.size() == 1);
    CHECK(outbox[0].phrase == fl::atc::AtcPhrase::ClearedTakeoff);
    CHECK(outbox[0].target == a);

    // B stays held while A owns the runway (no overlap).
    fac.update(em, 90, outbox);
    CHECK(fac.clearanceState(b) == fl::atc::ClearanceState::HoldShort);
    CHECK(fac.occupant() == a);

    // A climbs out; the runway frees and B is cleared next — strictly sequential.
    em.get(a)->transform.pos[1] = 80.0; // AGL 80 > release threshold
    outbox.clear();
    fac.update(em, 120, outbox);
    CHECK(fac.clearanceState(a) == fl::atc::ClearanceState::Departed);
    CHECK(fac.clearanceState(b) == fl::atc::ClearanceState::ClearedTakeoff);
    CHECK(fac.occupant() == b);
    REQUIRE(outbox.size() == 1);
    CHECK(outbox[0].target == b);
}

TEST_CASE("AtcFacility: holdDepartures freezes the queue", "[atc]") {
    NullLogger log;
    fl::EntityTypeRegistry reg;
    reg.registerType(basicDef());
    fl::EntityManager em(log, reg);
    fl::EntityTransform t{};
    t.quat[3] = 1.f;
    fl::EntityId a = em.spawn("test:basic", t);

    auto fac = makeFacility();
    std::vector<fl::atc::RadioTransmission> outbox;
    fac.setHoldDepartures(true);
    fac.requestTakeoff(a);
    fac.update(em, 60, outbox);
    CHECK(fac.clearanceState(a) == fl::atc::ClearanceState::HoldShort);
    CHECK_FALSE(fac.runwayOccupied());

    // Release the hold: the departure is now cleared.
    fac.setHoldDepartures(false);
    fac.update(em, 120, outbox);
    CHECK(fac.clearanceState(a) == fl::atc::ClearanceState::ClearedTakeoff);
}

TEST_CASE("AtcFacility: an arrival on short final waves off a departure occupying the runway", "[atc]") {
    NullLogger log;
    fl::EntityTypeRegistry reg;
    reg.registerType(basicDef());
    fl::EntityManager em(log, reg);

    fl::EntityTransform ground{};
    ground.quat[3] = 1.f;
    fl::EntityId dep = em.spawn("test:basic", ground);

    // Arrival 2 km out on final (inside the short-final range), airborne.
    fl::EntityTransform air{};
    air.pos[0] = -2000.0;
    air.pos[1] = 150.0;
    air.quat[3] = 1.f;
    fl::EntityId arr = em.spawn("test:basic", air);

    auto fac = makeFacility();
    std::vector<fl::atc::RadioTransmission> outbox;
    fac.requestTakeoff(dep);
    fac.update(em, 60, outbox); // dep takes the runway (no arrival yet)
    REQUIRE(fac.occupant() == dep);

    // Now the arrival appears on short final while the runway is occupied -> go around.
    outbox.clear();
    fac.requestLanding(arr);
    fac.update(em, 120, outbox);
    CHECK(fac.clearanceState(arr) == fl::atc::ClearanceState::GoAround);
    REQUIRE(outbox.size() == 1);
    CHECK(outbox[0].phrase == fl::atc::AtcPhrase::GoAround);
}

TEST_CASE("AtcFacility: a non-landing surface refuses arrivals", "[atc]") {
    NullLogger log;
    fl::EntityTypeRegistry reg;
    reg.registerType(basicDef());
    fl::EntityManager em(log, reg);
    fl::EntityTransform air{};
    air.pos[0] = -1000.0;
    air.pos[1] = 100.0;
    air.quat[3] = 1.f;
    fl::EntityId arr = em.spawn("test:basic", air);

    auto fac = makeFacility(/*acceptsLandings=*/false);
    std::vector<fl::atc::RadioTransmission> outbox;
    fac.requestLanding(arr);
    fac.update(em, 60, outbox);
    CHECK(fac.clearanceState(arr) == fl::atc::ClearanceState::None); // never sequenced
}

TEST_CASE("AtcService: lazy facilities, nearest resolution, and scramble", "[atc]") {
    NullLogger log;
    fl::EntityTypeRegistry reg;
    reg.registerType(basicDef());
    fl::EntityManager em(log, reg);

    fl::AirportRegistry airports;
    airports.load({fl::builtinAirfield()}, fl::kEarthRadiusM, nullptr);
    REQUIRE(airports.count() == 1);
    const fl::ResolvedAirport* field = airports.byId("builtin:airfield");
    REQUIRE(field != nullptr);

    fl::atc::AtcService svc(em, airports, fl::kEarthRadiusM);
    CHECK(svc.activeFacilityCount() == 0); // nothing built until traffic appears

    // Scramble invokes the spawn handler once per requested aircraft, with a hold-short pose near the
    // runway threshold.
    int spawnCalls = 0;
    fl::atc::AtcService::DepartureSpawn lastSpawn;
    svc.setSpawnHandler([&](const fl::atc::AtcService::DepartureSpawn& s) {
        ++spawnCalls;
        lastSpawn = s;
    });
    CHECK(svc.scramble("builtin:airfield", "test:basic", 2));
    CHECK(spawnCalls == 2);
    CHECK(lastSpawn.typeId == "test:basic");
    CHECK(lastSpawn.facilityId == "builtin:airfield");
    // Hold-short is close to the runway threshold.
    const fl::ResolvedRunway& rw = field->runways.front();
    const double dx = lastSpawn.holdShort.origin.x - rw.threshold.x;
    const double dz = lastSpawn.holdShort.origin.z - rw.threshold.z;
    CHECK(dx * dx + dz * dz < 100.0 * 100.0);
    CHECK(svc.activeFacilityCount() == 1); // the facility was created on demand

    // Scramble on an unknown airport fails.
    CHECK_FALSE(svc.scramble("nope:field", "test:basic", 1));

    // Nearest-facility resolution: a takeoff request with no explicit facility resolves to the only
    // airport and lands the flight in its departure queue.
    fl::EntityTransform t{};
    t.pos[0] = field->worldPos.x + 50.0;
    t.pos[2] = field->worldPos.z;
    t.quat[3] = 1.f;
    fl::EntityId f = em.spawn("test:basic", t);
    svc.requestTakeoff(f); // empty facility -> nearest
    svc.tick(em, 60);
    CHECK(svc.clearanceState(f) == fl::atc::ClearanceState::ClearedTakeoff);
    auto tx = svc.drainTransmissions();
    REQUIRE(tx.size() == 1);
    CHECK(tx[0].phrase == fl::atc::AtcPhrase::ClearedTakeoff);
}
