// SPDX-License-Identifier: GPL-3.0-or-later
//
// AtcFacility: the edges of the sequencing FSM (#1145).
//
// test_atc_service.cpp covers the four headline behaviours from #673. This file covers what happens
// when the world stops cooperating: an aircraft that blows up on the runway, a pilot who parks and
// quits, a flight that requests the same thing twice, a request from an id whose slot has since been
// recycled.
//
// All of it matters for one reason. The runway is a mutex, and this FSM is the only thing that
// releases it. A path that leaves m_occupant set on a dead entity deadlocks the field forever — no
// aircraft ever departs again, and nothing in the log says why.

#include <catch2/catch_test_macros.hpp>

#include "ILogger.h"
#include "atc/AtcFacility.h"
#include "entity/EntityDef.h"
#include "entity/EntityManager.h"
#include "entity/EntityState.h"
#include "entity/EntityTypeRegistry.h"
#include "flight/Geodetic.h"
#include "mock_log.h"

#include <vector>

using namespace fl;
using namespace fl::atc;

namespace {

EntityDef basicDef() {
    EntityDef d;
    d.id = "test:basic";
    d.name = "Basic";
    d.category = ObjectCategory::AirVehicle;
    d.maxHp = 100.f;
    return d;
}

// A runway at the world origin, heading 090 (along +X), field elevation 0.
AtcFacility::Runway originRunway() {
    AtcFacility::Runway r;
    r.threshold = {0.0, 0.0, 0.0};
    r.oppositeEnd = {2500.0, 0.0, 0.0};
    r.centerlineDir = {1.0, 0.0, 0.0};
    r.headingDeg = 90.f;
    r.elevationM = 0.0;
    return r;
}

AtcFacility makeFacility(bool acceptsLandings = true) {
    return AtcFacility("test:field", "Test Tower", originRunway(), kEarthRadiusM, acceptsLandings,
                       [] { return FacilityPose{{0, 0, 0}, 90.f}; });
}

// A world with an entity manager and a convenient spawn-at-position helper.
struct World {
    NullLogger log;
    EntityTypeRegistry reg;
    EntityManager em;

    World() : em(log, reg) {
        reg.registerType(basicDef());
    }

    EntityId spawnAt(double x, double agl, double z = 0.0) {
        EntityTransform t{};
        t.quat[3] = 1.f;
        t.pos[0] = x;
        t.pos[1] = agl;
        t.pos[2] = z;
        EntityId id = em.spawn("test:basic", t);
        return id;
    }
};

bool sent(const std::vector<RadioTransmission>& out, AtcPhrase p, EntityId to) {
    for (const auto& t : out)
        if (t.phrase == p && t.target == to)
            return true;
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

TEST_CASE("AtcFacility: the hold-short pose sits behind the threshold on the runway heading (#1145)", "[atc]") {
    // This is where an AI departure spawns. Putting it in FRONT of the threshold would spawn traffic
    // on the runway it is waiting to be cleared onto.
    const AtcFacility fac = makeFacility();
    const FacilityPose p = fac.holdShortPose();
    CHECK(p.headingDeg == 90.f);
    CHECK(p.origin.x < 0.0); // behind the threshold, against the centreline direction
    CHECK(glm::length(p.origin - fac.runway().threshold) > 1.0);
    CHECK(fac.id() == "test:field");
    CHECK(fac.speaker() == "Test Tower");
    CHECK(fac.acceptsLandings());
}

// ---------------------------------------------------------------------------
// Request idempotence and validity
// ---------------------------------------------------------------------------

TEST_CASE("AtcFacility: a repeated takeoff request does not queue the flight twice (#1145)", "[atc]") {
    // An AI composition polls its clearance and re-requests every tick until it is granted. If each
    // call appended, one aircraft would hold the runway once per tick it spent waiting.
    World w;
    const EntityId a = w.spawnAt(0, 0);
    AtcFacility fac = makeFacility();

    fac.requestTakeoff(a);
    fac.requestTakeoff(a);
    fac.requestTakeoff(a);
    CHECK(fac.departureQueueDepth() == 1u);
    CHECK(fac.clearanceState(a) == ClearanceState::HoldShort);
}

TEST_CASE("AtcFacility: a re-request never downgrades a clearance already granted (#1145)", "[atc]") {
    // The aircraft is rolling. Answering its next poll with "hold short" would abort a takeoff in
    // progress, which is the one thing ATC must never say to an aircraft on its takeoff roll.
    World w;
    const EntityId a = w.spawnAt(0, 0);
    AtcFacility fac = makeFacility();
    std::vector<RadioTransmission> out;

    fac.requestTakeoff(a);
    fac.update(w.em, 60, out);
    REQUIRE(fac.clearanceState(a) == ClearanceState::ClearedTakeoff);

    fac.requestTakeoff(a);
    CHECK(fac.clearanceState(a) == ClearanceState::ClearedTakeoff);
    CHECK(fac.departureQueueDepth() == 0u); // it is the occupant, not a queue member
}

TEST_CASE("AtcFacility: an invalid flight id is ignored by every entry point (#1145)", "[atc]") {
    World w;
    AtcFacility fac = makeFacility();
    std::vector<RadioTransmission> out;
    const EntityId nobody{};

    fac.requestTakeoff(nobody);
    fac.requestLanding(nobody);
    fac.declareInbound(nobody, out);

    CHECK(fac.departureQueueDepth() == 0u);
    CHECK(fac.arrivalCount() == 0u);
    CHECK(out.empty());
}

TEST_CASE("AtcFacility: a field that takes no landings refuses arrivals silently (#1145)", "[atc]") {
    // A departure-only strip. Refusing has to be silent here rather than a transmission, because the
    // caller (AtcService) is what decides whether to try another field.
    World w;
    const EntityId a = w.spawnAt(5000, 300);
    AtcFacility fac = makeFacility(/*acceptsLandings=*/false);
    std::vector<RadioTransmission> out;

    fac.requestLanding(a);
    fac.declareInbound(a, out);

    CHECK(fac.arrivalCount() == 0u);
    CHECK(fac.clearanceState(a) == ClearanceState::None);
    CHECK(out.empty());
    CHECK_FALSE(fac.acceptsLandings());
}

TEST_CASE("AtcFacility: declaring inbound acknowledges on the radio (#1145)", "[atc]") {
    World w;
    const EntityId a = w.spawnAt(20000, 2000);
    AtcFacility fac = makeFacility();
    std::vector<RadioTransmission> out;

    fac.declareInbound(a, out);
    CHECK(fac.clearanceState(a) == ClearanceState::Inbound);
    CHECK(fac.arrivalCount() == 1u);
    REQUIRE(out.size() == 1u);
    CHECK(out[0].phrase == AtcPhrase::ContactApproach);
    CHECK(out[0].speaker == "Test Tower");
    CHECK_FALSE(out[0].voiceKey.empty()); // the TTS binding key travels with the line

    // Declaring twice does not re-sequence the flight.
    fac.declareInbound(a, out);
    CHECK(fac.arrivalCount() == 1u);
}

TEST_CASE("AtcFacility: a landing request does not undo a landing clearance (#1145)", "[atc]") {
    World w;
    const EntityId a = w.spawnAt(2000, 150);
    AtcFacility fac = makeFacility();
    std::vector<RadioTransmission> out;

    fac.requestLanding(a);
    CHECK(fac.clearanceState(a) == ClearanceState::Pattern);
    fac.update(w.em, 60, out);
    REQUIRE(fac.clearanceState(a) == ClearanceState::ClearedToLand);

    fac.requestLanding(a); // the AI polls again on short final
    CHECK(fac.clearanceState(a) == ClearanceState::ClearedToLand);
}

// ---------------------------------------------------------------------------
// Recycled entity slots
// ---------------------------------------------------------------------------

TEST_CASE("AtcFacility: a clearance does not transfer to the next flight in the slot (#1145)", "[atc]") {
    // Clearances are keyed by pool INDEX for lookup speed, and the pool recycles indices. Without
    // the generation check, a fresh aircraft spawning into a departed flight's slot would inherit
    // its takeoff clearance and roll without ever being cleared.
    World w;
    const EntityId a = w.spawnAt(0, 0);
    AtcFacility fac = makeFacility();
    std::vector<RadioTransmission> out;

    fac.requestTakeoff(a);
    fac.update(w.em, 60, out);
    REQUIRE(fac.clearanceState(a) == ClearanceState::ClearedTakeoff);

    w.em.kill(a);
    w.em.onTick(1.0 / 60.0, 61); // reap

    const EntityId b = w.spawnAt(0, 0);
    REQUIRE(b.index == a.index); // the slot was reused, which is the whole point
    REQUIRE(b.generation != a.generation);
    CHECK(fac.clearanceState(b) == ClearanceState::None);
}

// ---------------------------------------------------------------------------
// Losing the occupant
// ---------------------------------------------------------------------------

TEST_CASE("AtcFacility: an occupant that dies on the runway releases it (#1145)", "[atc]") {
    // Shot down, crashed, or disconnected on the roll. The runway is a mutex and this is the only
    // path that unlocks it — leaving it held deadlocks the field for the rest of the session.
    World w;
    const EntityId a = w.spawnAt(0, 0);
    const EntityId b = w.spawnAt(0, 0);
    AtcFacility fac = makeFacility();
    std::vector<RadioTransmission> out;

    fac.requestTakeoff(a);
    fac.requestTakeoff(b);
    fac.update(w.em, 60, out);
    REQUIRE(fac.occupant() == a);

    w.em.kill(a);
    w.em.onTick(1.0 / 60.0, 61);

    out.clear();
    fac.update(w.em, 120, out);
    CHECK(fac.occupant() == b); // the field kept operating
    CHECK(fac.clearanceState(b) == ClearanceState::ClearedTakeoff);
}

TEST_CASE("AtcFacility: dead flights are pruned from both queues and the clearance table (#1145)", "[atc]") {
    World w;
    const EntityId dep = w.spawnAt(0, 0);
    const EntityId arr = w.spawnAt(9000, 600);
    AtcFacility fac = makeFacility();
    std::vector<RadioTransmission> out;

    fac.requestTakeoff(dep);
    fac.requestLanding(arr);
    REQUIRE(fac.departureQueueDepth() == 1u);
    REQUIRE(fac.arrivalCount() == 1u);

    w.em.kill(dep);
    w.em.kill(arr);
    w.em.onTick(1.0 / 60.0, 61);

    fac.update(w.em, 120, out);
    CHECK(fac.departureQueueDepth() == 0u);
    CHECK(fac.arrivalCount() == 0u);
    CHECK(fac.clearanceState(dep) == ClearanceState::None);
    CHECK(fac.clearanceState(arr) == ClearanceState::None);
    CHECK_FALSE(fac.runwayOccupied());
}

TEST_CASE("AtcFacility: removeFlight forgets a flight everywhere at once (#1145)", "[atc]") {
    // A player quitting mid-roll. Anything left behind is either a phantom in the queue or a held
    // runway.
    World w;
    const EntityId a = w.spawnAt(0, 0);
    AtcFacility fac = makeFacility();
    std::vector<RadioTransmission> out;

    fac.requestTakeoff(a);
    fac.update(w.em, 60, out);
    REQUIRE(fac.occupant() == a);

    fac.removeFlight(a);
    CHECK_FALSE(fac.runwayOccupied());
    CHECK(fac.clearanceState(a) == ClearanceState::None);
    CHECK(fac.departureQueueDepth() == 0u);

    // Removing a flight ATC never heard of is a no-op, not a crash.
    fac.removeFlight(w.spawnAt(100, 0));
    fac.removeFlight(EntityId{});
}

// ---------------------------------------------------------------------------
// Occupancy release
// ---------------------------------------------------------------------------

TEST_CASE("AtcFacility: a stuck departure is released by the deadlock backstop (#1145)", "[atc]") {
    // An aircraft that never climbs — out of fuel on the roll, a stalled AI, an aborted takeoff that
    // nobody cleaned up. Without the timeout it holds the runway for the rest of the session.
    World w;
    const EntityId stuck = w.spawnAt(0, 0);
    const EntityId next = w.spawnAt(0, 0);
    AtcFacility fac = makeFacility();
    std::vector<RadioTransmission> out;

    fac.requestTakeoff(stuck);
    fac.requestTakeoff(next);
    fac.update(w.em, 100, out);
    REQUIRE(fac.occupant() == stuck);

    // Still sitting there, still at zero AGL, well past the backstop.
    fac.update(w.em, 100 + 5401, out);
    CHECK(fac.clearanceState(stuck) == ClearanceState::Departed);
    CHECK(fac.occupant() == next);
}

TEST_CASE("AtcFacility: an arrival releases the runway once it has stopped (#1145)", "[atc]") {
    // Landed is not the same as clear. The lander keeps the runway while it is still rolling out at
    // speed, and only gives it up once it is slow and on the ground.
    World w;
    const EntityId lander = w.spawnAt(500, 5);
    AtcFacility fac = makeFacility();
    std::vector<RadioTransmission> out;

    fac.requestLanding(lander);
    fac.update(w.em, 60, out);
    REQUIRE(fac.occupant() == lander);
    REQUIRE(fac.clearanceState(lander) == ClearanceState::ClearedToLand);

    // Rolling out fast: still ours.
    w.em.get(lander)->transform.vel[0] = 60.f;
    fac.update(w.em, 120, out);
    CHECK(fac.occupant() == lander);
    CHECK(fac.clearanceState(lander) == ClearanceState::ClearedToLand);

    // Stopped and on the ground: the landing is complete, so the runway is released and the flight
    // is retired from the arrival sequence in the same step (#1149).
    w.em.get(lander)->transform.vel[0] = 0.5f;
    w.em.get(lander)->transform.pos[1] = 2.0;
    out.clear();
    fac.update(w.em, 180, out);
    CHECK_FALSE(fac.runwayOccupied());
    CHECK(fac.arrivalCount() == 0u);
    CHECK(fac.clearanceState(lander) == ClearanceState::Landed);
    CHECK(sent(out, AtcPhrase::TaxiToParking, lander));
}

TEST_CASE("AtcFacility: a landed flight is retired and the runway goes to the departure (#1149)", "[atc]") {
    // The defect this replaces: nothing removed a flight once it had landed, so on the same update()
    // that released its occupancy it was once more the nearest arrival, was re-cleared to land, and
    // re-took the runway — for the rest of the session, since arrivals beat departures. The
    // occupancy timeout could not save it; the re-clear happened on the tick of the release.
    World w;
    const EntityId lander = w.spawnAt(500, 3);
    const EntityId dep = w.spawnAt(0, 0);
    AtcFacility fac = makeFacility();
    std::vector<RadioTransmission> out;

    fac.requestLanding(lander);
    fac.requestTakeoff(dep);
    fac.update(w.em, 60, out);
    REQUIRE(fac.occupant() == lander);
    REQUIRE(fac.clearanceState(dep) == ClearanceState::HoldShort);

    // Down, stopped, and going nowhere: retired on this step, and told to taxi.
    w.em.get(lander)->transform.vel[0] = 0.f;
    w.em.get(lander)->transform.pos[1] = 2.0;
    out.clear();
    fac.update(w.em, 120, out);
    CHECK(fac.arrivalCount() == 0u);
    CHECK(fac.clearanceState(lander) == ClearanceState::Landed);
    CHECK(sent(out, AtcPhrase::TaxiToParking, lander));
    CHECK_FALSE(sent(out, AtcPhrase::ClearedToLand, lander));

    // With the arrival list empty the runway is free, so the waiting departure finally goes.
    CHECK(fac.occupant() == dep);
    CHECK(fac.clearanceState(dep) == ClearanceState::ClearedTakeoff);
    CHECK(sent(out, AtcPhrase::ClearedTakeoff, dep));
}

TEST_CASE("AtcFacility: a landed flight stays landed and stays quiet (#1149)", "[atc]") {
    // Landed is terminal, not a one-step blink: it survives every later step so a caller polling
    // clearanceState() can actually act on it, and the taxi line is transmitted once, not per tick.
    World w;
    const EntityId lander = w.spawnAt(500, 3);
    AtcFacility fac = makeFacility();
    std::vector<RadioTransmission> out;

    fac.requestLanding(lander);
    fac.update(w.em, 60, out);
    w.em.get(lander)->transform.vel[0] = 0.f;
    w.em.get(lander)->transform.pos[1] = 2.0;
    fac.update(w.em, 120, out);
    REQUIRE(fac.clearanceState(lander) == ClearanceState::Landed);

    for (int step = 0; step < 4; ++step) {
        out.clear();
        fac.update(w.em, 180 + 60ull * static_cast<uint64_t>(step), out);
        CHECK(fac.clearanceState(lander) == ClearanceState::Landed);
        CHECK(fac.arrivalCount() == 0u);
        CHECK_FALSE(fac.runwayOccupied());
        CHECK(out.empty()); // no re-clearance, no repeated taxi line
    }
}

TEST_CASE("AtcFacility: a landed flight can request takeoff again (#1149)", "[atc]") {
    // Turnaround: the aircraft parks, refuels and goes again. A terminal clearance is not a live one,
    // so the new request must reset it — otherwise the flight queues while still reading `landed`.
    World w;
    const EntityId flight = w.spawnAt(500, 3);
    AtcFacility fac = makeFacility();
    std::vector<RadioTransmission> out;

    fac.requestLanding(flight);
    fac.update(w.em, 60, out);
    w.em.get(flight)->transform.vel[0] = 0.f;
    w.em.get(flight)->transform.pos[1] = 2.0;
    fac.update(w.em, 120, out);
    REQUIRE(fac.clearanceState(flight) == ClearanceState::Landed);

    fac.requestTakeoff(flight);
    CHECK(fac.clearanceState(flight) == ClearanceState::HoldShort);
    CHECK(fac.departureQueueDepth() == 1u);

    out.clear();
    fac.update(w.em, 180, out);
    CHECK(fac.clearanceState(flight) == ClearanceState::ClearedTakeoff);
    CHECK(fac.occupant() == flight);
}

TEST_CASE("AtcFacility: the next arrival is cleared once the one ahead has landed (#1149)", "[atc]") {
    // The queue has to keep moving. Retiring the lander promotes whoever was behind it.
    World w;
    const EntityId first = w.spawnAt(2000, 200);
    const EntityId second = w.spawnAt(5000, 400); // behind it, but inside the clearance range
    AtcFacility fac = makeFacility();
    std::vector<RadioTransmission> out;

    fac.requestLanding(first);
    fac.requestLanding(second);
    fac.update(w.em, 60, out);
    REQUIRE(fac.occupant() == first);
    REQUIRE(fac.clearanceState(second) == ClearanceState::Pattern);

    w.em.get(first)->transform.vel[0] = 0.f;
    w.em.get(first)->transform.pos[0] = 500.0;
    w.em.get(first)->transform.pos[1] = 2.0;
    out.clear();
    fac.update(w.em, 120, out);

    CHECK(fac.arrivalCount() == 1u);
    CHECK(fac.occupant() == second);
    CHECK(fac.clearanceState(second) == ClearanceState::ClearedToLand);
    CHECK(sent(out, AtcPhrase::ClearedToLand, second));
}

TEST_CASE("AtcFacility: the arrival backstop never claims a landing (#1149)", "[atc]") {
    // The arrival budget is a backstop for the runway mutex, not an assertion that anyone landed. An
    // aircraft still airborne when it blows it — a slow final, a hover, a touch-and-go climbing away
    // — must never be stamped Landed or retired as if it were down.
    World w;
    const EntityId slow = w.spawnAt(4000, 300);
    AtcFacility fac = makeFacility();
    std::vector<RadioTransmission> out;

    fac.requestLanding(slow);
    fac.update(w.em, 100, out);
    REQUIRE(fac.occupant() == slow);

    out.clear();
    fac.update(w.em, 100 + 14401, out);
    CHECK(fac.clearanceState(slow) != ClearanceState::Landed);
    CHECK(fac.arrivalCount() == 1u); // still on the approach
    CHECK_FALSE(sent(out, AtcPhrase::TaxiToParking, slow));
}

// ---------------------------------------------------------------------------
// Starvation: an arrival that never lands (#1154)
// ---------------------------------------------------------------------------

TEST_CASE("AtcFacility: an arrival that never lands is waved off so a departure can go (#1154)", "[atc]") {
    // The motivating case: a hover over the threshold. It holds a landing clearance, so releasing the
    // runway on the backstop achieved nothing — it was still the nearest arrival and re-took it on
    // the same tick, and since arrivals beat departures the field never reopened.
    World w;
    const EntityId hover = w.spawnAt(1500, 60);
    const EntityId dep = w.spawnAt(0, 0);
    AtcFacility fac = makeFacility();
    std::vector<RadioTransmission> out;

    fac.requestLanding(hover);
    fac.requestTakeoff(dep);
    fac.update(w.em, 100, out);
    REQUIRE(fac.occupant() == hover);
    REQUIRE(fac.clearanceState(dep) == ClearanceState::HoldShort);

    // Still up there, still going nowhere, well past the arrival budget.
    out.clear();
    fac.update(w.em, 100 + 14401, out);

    CHECK(fac.clearanceState(hover) == ClearanceState::GoAround);
    CHECK(sent(out, AtcPhrase::GoAround, hover));
    CHECK(fac.clearanceState(hover) != ClearanceState::Landed);
    CHECK(fac.arrivalCount() == 1u); // waved off, not retired — it may still come back

    // And the point of all of it: the departure finally goes, even though the hoverer is still
    // sitting inside the departure-hold range.
    CHECK(fac.occupant() == dep);
    CHECK(fac.clearanceState(dep) == ClearanceState::ClearedTakeoff);
}

TEST_CASE("AtcFacility: a slow arrival with nobody waiting keeps the runway (#1154)", "[atc]") {
    // Nobody is starved when nobody is waiting, and a slow aircraft still working the approach has
    // done nothing wrong. Waving it off on the budget alone would punish a legitimate long final.
    World w;
    const EntityId slow = w.spawnAt(4000, 300);
    AtcFacility fac = makeFacility();
    std::vector<RadioTransmission> out;

    fac.requestLanding(slow);
    fac.update(w.em, 100, out);
    REQUIRE(fac.occupant() == slow);

    for (int step = 0; step < 3; ++step) {
        out.clear();
        fac.update(w.em, 100 + 14401 + 14401ull * static_cast<uint64_t>(step), out);
        CHECK(fac.clearanceState(slow) == ClearanceState::ClearedToLand);
        CHECK(fac.occupant() == slow);
        CHECK(out.empty()); // no wave-off, and no re-transmitted clearance
    }
}

TEST_CASE("AtcFacility: a waved-off flight yields the sequence to the arrival behind it (#1154)", "[atc]") {
    // The hold-off is what makes a wave-off mean something: while it runs, the flight is skipped when
    // picking the runway, so the aircraft queued behind it actually gets in.
    World w;
    const EntityId hover = w.spawnAt(1500, 60);
    const EntityId next = w.spawnAt(4000, 300);
    AtcFacility fac = makeFacility();
    std::vector<RadioTransmission> out;

    fac.requestLanding(hover);
    fac.update(w.em, 100, out);
    REQUIRE(fac.occupant() == hover);

    fac.requestLanding(next);
    out.clear();
    fac.update(w.em, 100 + 14401, out);

    CHECK(fac.clearanceState(hover) == ClearanceState::GoAround);
    CHECK(fac.occupant() == next); // the nearer aircraft is skipped, the one behind it lands
    CHECK(fac.clearanceState(next) == ClearanceState::ClearedToLand);
    CHECK(sent(out, AtcPhrase::ClearedToLand, next));
}

TEST_CASE("AtcFacility: the hold-off expires and a waved-off flight is sequenced again (#1154)", "[atc]") {
    // A wave-off is not a ban. The flight re-enters the sequence once the window is up — which is
    // also the accepted steady state for an aircraft that simply refuses to leave: it cycles the
    // runway with everyone else instead of owning it.
    World w;
    const EntityId hover = w.spawnAt(1500, 60);
    const EntityId dep = w.spawnAt(0, 0);
    AtcFacility fac = makeFacility();
    std::vector<RadioTransmission> out;

    fac.requestLanding(hover);
    fac.requestTakeoff(dep);
    fac.update(w.em, 100, out);
    const uint64_t wavedAt = 100 + 14401;
    fac.update(w.em, wavedAt, out);
    REQUIRE(fac.clearanceState(hover) == ClearanceState::GoAround);

    // Inside the window: still skipped, so the departure keeps the runway it was given.
    fac.update(w.em, wavedAt + 3600, out);
    CHECK(fac.occupant() == dep);

    // Past it: eligible again, and with the departure long gone it is cleared to land.
    w.em.get(dep)->transform.pos[1] = 200.0; // departure climbed clear
    out.clear();
    fac.update(w.em, wavedAt + 7201, out);
    CHECK(fac.occupant() == hover);
    CHECK(fac.clearanceState(hover) == ClearanceState::ClearedToLand);
    CHECK(sent(out, AtcPhrase::ClearedToLand, hover));
}

TEST_CASE("AtcFacility: a short-final wave-off is not re-cleared the instant the runway frees (#1154)", "[atc]") {
    // The existing short-final wave-off gets the same hold-off. Without it the flight was re-cleared
    // to land on the very next step, while it was still climbing away from the go-around it was just
    // given — a clearance no pilot could act on.
    World w;
    const EntityId dep = w.spawnAt(0, 0);
    AtcFacility fac = makeFacility();
    std::vector<RadioTransmission> out;

    fac.requestTakeoff(dep);
    fac.update(w.em, 60, out);
    REQUIRE(fac.occupant() == dep);

    const EntityId arr = w.spawnAt(1500, 80); // short final, runway held by the departure
    fac.requestLanding(arr);
    fac.update(w.em, 120, out);
    REQUIRE(fac.clearanceState(arr) == ClearanceState::GoAround);

    // The departure climbs out and the runway frees on the same step.
    w.em.get(dep)->transform.pos[1] = 200.0;
    out.clear();
    fac.update(w.em, 180, out);
    CHECK(fac.clearanceState(arr) == ClearanceState::GoAround);
    CHECK_FALSE(fac.runwayOccupied());
    CHECK(out.empty());

    // Once the window is up it is sequenced normally again.
    out.clear();
    fac.update(w.em, 120 + 7201, out);
    CHECK(fac.clearanceState(arr) == ClearanceState::ClearedToLand);
    CHECK(fac.occupant() == arr);
}

TEST_CASE("AtcFacility: an arrival still airborne over the field does not count as stopped (#1145)", "[atc]") {
    // Velocity alone is not enough — a hovering helicopter and a stopped aeroplane read the same in
    // speed. The AGL term is what separates them.
    World w;
    const EntityId heli = w.spawnAt(200, 400);
    AtcFacility fac = makeFacility();
    std::vector<RadioTransmission> out;

    fac.requestLanding(heli);
    fac.update(w.em, 60, out);
    REQUIRE(fac.occupant() == heli);

    w.em.get(heli)->transform.vel[0] = 0.1f; // stationary, but 400 m up
    fac.update(w.em, 120, out);
    CHECK(fac.occupant() == heli);
    CHECK(fac.clearanceState(heli) != ClearanceState::Landed);
}

// ---------------------------------------------------------------------------
// Sequencing between arrivals and departures
// ---------------------------------------------------------------------------

TEST_CASE("AtcFacility: arrivals are sequenced nearest to the threshold first (#1145)", "[atc]") {
    // Sequencing by request order would put a flight 40 km out ahead of one on short final.
    World w;
    const EntityId far = w.spawnAt(30000, 3000);
    const EntityId near = w.spawnAt(4000, 300);
    AtcFacility fac = makeFacility();
    std::vector<RadioTransmission> out;

    fac.requestLanding(far); // requested first...
    fac.requestLanding(near);
    fac.update(w.em, 60, out);

    CHECK(fac.occupant() == near); // ...but the nearer one lands
    CHECK(fac.clearanceState(near) == ClearanceState::ClearedToLand);
    CHECK(fac.clearanceState(far) == ClearanceState::Pattern);
    CHECK(sent(out, AtcPhrase::ClearedToLand, near));
}

TEST_CASE("AtcFacility: an inbound arrival holds a waiting departure on the ground (#1145)", "[atc]") {
    // You cannot hold a landing, so the departure waits. The reverse would put an aircraft on the
    // runway in front of one committed to the approach.
    World w;
    const EntityId dep = w.spawnAt(0, 0);
    const EntityId arr = w.spawnAt(5000, 400); // inside the departure-hold range
    AtcFacility fac = makeFacility();
    std::vector<RadioTransmission> out;

    fac.requestTakeoff(dep);
    fac.requestLanding(arr);
    fac.update(w.em, 60, out);

    CHECK(fac.occupant() == arr);
    CHECK(fac.clearanceState(dep) == ClearanceState::HoldShort);
    CHECK(fac.departureQueueDepth() == 1u); // still queued, not dropped
}

TEST_CASE("AtcFacility: a departure goes once the arrival is far enough out (#1145, #1154)", "[atc]") {
    // A landing clearance is only issued from inside the range where a departure could not have got
    // out first. Beyond it the arrival is sequenced but does not own the runway — it used to claim it
    // from 40 km, blocking every departure for the whole approach (#1154).
    World w;
    const EntityId dep = w.spawnAt(0, 0);
    const EntityId arr = w.spawnAt(40000, 4000); // well beyond the hold range
    AtcFacility fac = makeFacility();
    std::vector<RadioTransmission> out;

    fac.requestTakeoff(dep);
    fac.requestLanding(arr);
    fac.update(w.em, 60, out);
    CHECK(fac.occupant() == dep);
    CHECK(fac.clearanceState(dep) == ClearanceState::ClearedTakeoff);
    CHECK(fac.clearanceState(arr) == ClearanceState::Pattern); // sequenced, not cleared
    CHECK(fac.arrivalCount() == 1u);
    CHECK_FALSE(sent(out, AtcPhrase::ClearedToLand, arr));

    // Once it is inside the range it takes priority again, as soon as the runway is free.
    w.em.get(arr)->transform.pos[0] = 4000.0;
    w.em.get(dep)->transform.pos[1] = 200.0; // departure has climbed clear
    out.clear();
    fac.update(w.em, 120, out);
    CHECK(fac.occupant() == arr);
    CHECK(fac.clearanceState(arr) == ClearanceState::ClearedToLand);
    CHECK(sent(out, AtcPhrase::ClearedToLand, arr));
}

TEST_CASE("AtcFacility: a go-around is transmitted once, not every tick (#1145)", "[atc]") {
    // The wave-off repeats for as long as the geometry holds. Re-transmitting each tick would bury
    // every other radio line under one aircraft's go-around.
    World w;
    const EntityId dep = w.spawnAt(0, 0);
    AtcFacility fac = makeFacility();
    std::vector<RadioTransmission> out;

    fac.requestTakeoff(dep);
    fac.update(w.em, 60, out);
    REQUIRE(fac.occupant() == dep);

    const EntityId arr = w.spawnAt(1500, 80); // inside short final, runway held by the departure
    fac.requestLanding(arr);
    out.clear();
    fac.update(w.em, 120, out);
    CHECK(fac.clearanceState(arr) == ClearanceState::GoAround);
    CHECK(sent(out, AtcPhrase::GoAround, arr));

    out.clear();
    fac.update(w.em, 180, out);
    CHECK(fac.clearanceState(arr) == ClearanceState::GoAround);
    CHECK(out.empty()); // already told
}

TEST_CASE("AtcFacility: holding departures does not hold arrivals (#1145)", "[atc]") {
    // `hold` is a departure freeze for range-clearing or an emergency. An aircraft already on
    // approach is not part of that decision.
    World w;
    const EntityId dep = w.spawnAt(0, 0);
    const EntityId arr = w.spawnAt(3500, 200);
    AtcFacility fac = makeFacility();
    std::vector<RadioTransmission> out;

    fac.setHoldDepartures(true);
    CHECK(fac.holding());
    fac.requestTakeoff(dep);
    fac.requestLanding(arr);
    fac.update(w.em, 60, out);

    CHECK(fac.occupant() == arr);
    CHECK(fac.clearanceState(arr) == ClearanceState::ClearedToLand);
    CHECK(fac.clearanceState(dep) == ClearanceState::HoldShort);
}
