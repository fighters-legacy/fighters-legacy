// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "entity/EntityDef.h"
#include "entity/EntityManager.h"
#include "entity/EntityTypeRegistry.h"
#include "sensor/SensorSystem.h"
#include "spatial/SpatialIndex.h"

#include "mock_hal.h"

#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace fl;
using namespace fl::sensor;

namespace {

constexpr float kMPerNm = 1852.f;
constexpr double kSimDt = 1.0 / 60.0;

// A radar: 60/30 search out to 40 nm, 30/20 track out to 30 nm, pod = 1 so the tests exercise
// geometry and lifecycle rather than the dice (which test_sensor_detection already covers).
std::shared_ptr<const SensorDef> makeRadar(float lockHoldS = 4.f) {
    SensorDef s;
    s.id = "t:radar";
    s.name = "Radar";
    s.type = SensorType::Radar;
    s.emitter = true;
    s.search = SensorLobe{60.f, 30.f, 0.f, 40.f * kMPerNm, 1.f};
    s.track = SensorLobe{30.f, 20.f, 0.f, 30.f * kMPerNm, 1.f};
    s.lockHoldS = lockHoldS;
    return std::make_shared<const SensorDef>(s);
}

EntityDef makeDef(std::string id, std::vector<std::string> sensors = {}) {
    EntityDef d;
    d.id = std::move(id);
    d.name = "T";
    d.category = ObjectCategory::AirVehicle;
    d.maxHp = 100.f;
    d.sensorIds = std::move(sensors);
    return d;
}

// A fixture holding a live EntityManager + registry + a SensorSystem whose resolver returns one
// radar for any id.
struct Fixture {
    MockLogger logger;
    EntityTypeRegistry registry;
    EntityManager em{logger, registry};
    SensorSystem sys{em, registry};
    SpatialIndex si;

    Fixture() {
        registry.registerType(makeDef("t:fighter", {"t:radar"}));
        sys.setResolver([](const std::string&) { return makeRadar(); });
        sys.recomputeSignatureScale();
    }

    EntityId spawn(double x, double y, double z, const float quat[4] = nullptr) {
        EntityTransform t{};
        t.pos[0] = x;
        t.pos[1] = y;
        t.pos[2] = z;
        if (quat) {
            for (int i = 0; i < 4; ++i)
                t.quat[i] = quat[i];
        }
        return em.spawn("t:fighter", t);
    }

    void rebuildIndex() {
        si.clear();
        em.forEach([this](const EntityState& s) { si.insert(s.id.index, s.transform.pos); });
    }

    // Runs one sensing check for every observer (stride 1 = every tick is a check tick), then the RWR
    // inversion pass — the exact order WorldBroadcaster::onTick runs them in.
    void check(uint64_t tick) {
        rebuildIndex();
        auto& work = sys.gatherDue(tick, 1u, kSimDt);
        for (const auto& w : work)
            sys.evaluateObserver(w, si, tick, SensingEnvironment{}, 1.f, 0.f);
        sys.updateReactions(tick, kSimDt, 0.f);
        sys.buildThreatWarnings(tick);
    }
};

} // namespace

TEST_CASE("SensorSystem: an entity detects a target inside its cone and not one behind it", "[sensor_system]") {
    Fixture f;
    const EntityId observer = f.spawn(0, 0, 0);             // nose along +X
    const EntityId ahead = f.spawn(10.0 * kMPerNm, 0, 0);   // dead ahead, 10 nm
    const EntityId behind = f.spawn(-10.0 * kMPerNm, 0, 0); // 10 nm astern

    f.sys.addObserver(observer.index, {"t:radar"}, 0.5f, 0.5f);
    f.check(1);

    const ContactTable* contacts = f.sys.contactsFor(observer.index);
    REQUIRE(contacts != nullptr);

    // THE POINT OF THE WHOLE SUBSYSTEM: the target behind is in range, alive, and known to the
    // EntityManager — and the observer does not have it. Ground truth is not reachable from a
    // contact table.
    CHECK(contacts->find(ahead) != nullptr);
    CHECK(contacts->find(behind) == nullptr);
}

TEST_CASE("SensorSystem: a target beyond the sensor's range is not detected", "[sensor_system]") {
    Fixture f;
    const EntityId observer = f.spawn(0, 0, 0);
    const EntityId farAway = f.spawn(60.0 * kMPerNm, 0, 0); // 60 nm — beyond the 40 nm search lobe

    f.sys.addObserver(observer.index, {"t:radar"}, 0.5f, 0.5f);
    f.check(1);

    CHECK(f.sys.contactsFor(observer.index)->find(farAway) == nullptr);
}

TEST_CASE("SensorSystem: a contact promotes to Locked inside the track lobe", "[sensor_system]") {
    Fixture f;
    const EntityId observer = f.spawn(0, 0, 0);
    const EntityId target = f.spawn(10.0 * kMPerNm, 0, 0);

    f.sys.addObserver(observer.index, {"t:radar"}, 0.5f, 0.5f);
    f.check(1);

    const Contact* c = f.sys.contactsFor(observer.index)->find(target);
    REQUIRE(c != nullptr);
    CHECK(c->locked()); // dead ahead and inside both lobes, pod = 1
    CHECK(c->held());
}

TEST_CASE("SensorSystem: an entity with no declared sensors gets the builtin eyeball", "[sensor_system]") {
    // Honest sensing is the DEFAULT, not an opt-in: an entity that declares nothing gets eyes — not
    // omniscience, and not blindness.
    Fixture f;
    const EntityId observer = f.spawn(0, 0, 0);
    const EntityId near = f.spawn(2000.0, 0, 0);        // 2 km ahead: inside the eyeball's ~8 nm
    const EntityId far = f.spawn(30.0 * kMPerNm, 0, 0); // 30 nm: a radar would see it, an eye will not

    f.sys.addObserver(observer.index, /*sensorIds=*/{}, 1.0f, 0.5f); // skill 1 so the PoD roll lands fast

    // The eyeball's PoD is low by design, so give it several checks to acquire.
    for (uint64_t t = 1; t <= 60; ++t)
        f.check(t);

    const ContactTable* contacts = f.sys.contactsFor(observer.index);
    REQUIRE(contacts != nullptr);
    CHECK(contacts->find(near) != nullptr);
    CHECK(contacts->find(far) == nullptr); // an eyeball does not find a fighter at 30 nm

    const Contact* c = contacts->find(near);
    REQUIRE(c != nullptr);
    CHECK_FALSE(c->locked()); // and it never holds a lock — the eyeball is search-only
}

TEST_CASE("SensorSystem: losing the cone coasts on last-known state, then drops", "[sensor_system]") {
    Fixture f;
    const EntityId observer = f.spawn(0, 0, 0);
    const EntityId target = f.spawn(10.0 * kMPerNm, 0, 0);

    f.sys.addObserver(observer.index, {"t:radar"}, 0.5f, 0.5f);
    f.check(1);
    REQUIRE(f.sys.contactsFor(observer.index)->find(target)->locked());

    const double lastKnownX = f.sys.contactsFor(observer.index)->find(target)->lastKnownPos[0];

    // Teleport the target astern: out of the cone entirely.
    EntityState* ts = f.em.get(target);
    REQUIRE(ts != nullptr);
    ts->transform.pos[0] = -10.0 * kMPerNm;

    f.check(2);
    const Contact* c = f.sys.contactsFor(observer.index)->find(target);
    REQUIRE(c != nullptr);
    CHECK(c->state == ContactState::Coasting);

    // The coasting contact reports where the target WAS, not where it is. A consumer is not handed a
    // fresh position it has not earned.
    CHECK(c->lastKnownPos[0] == lastKnownX);

    // lock_hold_s = 4 s at 60 Hz checks: the contact survives a while, then drops.
    for (uint64_t t = 3; t < 3 + 4 * 60 + 2; ++t)
        f.check(t);
    CHECK(f.sys.contactsFor(observer.index)->find(target) == nullptr);
}

TEST_CASE("SensorSystem: the reaction delay gates `reacted`, not detection itself", "[sensor_system]") {
    // A rookie SEES you at the same moment an ace does — what a rookie does is take longer to act on
    // it. A difficulty knob that made the rookie see less would be a lie.
    Fixture f;
    const EntityId observer = f.spawn(0, 0, 0);
    const EntityId target = f.spawn(10.0 * kMPerNm, 0, 0);

    f.sys.addObserver(observer.index, {"t:radar"}, 0.5f, /*reaction=*/0.5f); // 0.5 = unity

    constexpr float kReactionTimeS = 1.f;
    f.rebuildIndex();
    auto& work = f.sys.gatherDue(1, 1u, kSimDt);
    for (const auto& w : work)
        f.sys.evaluateObserver(w, f.si, 1, SensingEnvironment{}, 1.f, kReactionTimeS);
    f.sys.updateReactions(1, kSimDt, kReactionTimeS);

    const Contact* c = f.sys.contactsFor(observer.index)->find(target);
    REQUIRE(c != nullptr);
    CHECK(c->held());        // detected immediately...
    CHECK_FALSE(c->reacted); // ...but not yet acted upon

    // 1 s at 60 Hz = 60 ticks.
    for (uint64_t t = 2; t <= 62; ++t) {
        f.rebuildIndex();
        auto& w2 = f.sys.gatherDue(t, 1u, kSimDt);
        for (const auto& w : w2)
            f.sys.evaluateObserver(w, f.si, t, SensingEnvironment{}, 1.f, kReactionTimeS);
        f.sys.updateReactions(t, kSimDt, kReactionTimeS);
    }
    CHECK(f.sys.contactsFor(observer.index)->find(target)->reacted);
}

TEST_CASE("SensorSystem: a radar in EMCON (not emitting) is blind, search and track alike", "[sensor_system]") {
    // The EMCON fix (#526): a radar sees NOTHING it does not first illuminate. Before #526 a
    // non-emitting radar's search lobe still detected — a passive-radar free lunch that never made
    // sense. Silence is silence: no strobes, no locks, nothing.
    Fixture f;
    const EntityId observer = f.spawn(0, 0, 0);
    const EntityId target = f.spawn(10.0 * kMPerNm, 0, 0);

    f.sys.addObserver(observer.index, {"t:radar"}, 0.5f, 0.5f);
    f.sys.setEmitting(observer.index, false); // == radar Silent mode
    f.check(1);

    CHECK(f.sys.contactsFor(observer.index)->find(target) == nullptr);
    CHECK(f.sys.radarMode(observer.index) == RadarMode::Silent); // setEmitting(false) is Silent
}

TEST_CASE("SensorSystem: radar Search mode reports a bearing but never a lock", "[sensor_system]") {
    // Search sweeps and finds; it does not hold a firing-quality track. A target found in Search must
    // be locked (STT) before a radar missile has a solution.
    Fixture f;
    const EntityId observer = f.spawn(0, 0, 0);
    const EntityId target = f.spawn(10.0 * kMPerNm, 0, 0); // inside both lobes, pod = 1

    f.sys.addObserver(observer.index, {"t:radar"}, 0.5f, 0.5f);
    f.sys.setRadarMode(observer.index, RadarMode::Search);
    f.check(1);

    const Contact* c = f.sys.contactsFor(observer.index)->find(target);
    REQUIRE(c != nullptr);
    CHECK(c->held());
    CHECK(c->state == ContactState::Detected);
    CHECK_FALSE(c->locked());      // Search offers no firing solution...
    CHECK_FALSE(c->firingQuality); // ...and certainly not a firing-quality one
}

TEST_CASE("SensorSystem: radar TWS locks but the lock is not firing-quality", "[sensor_system]") {
    // Track-while-scan reaches Locked but spreads its energy: a hostile RWR reads it as a scan, and a
    // SARH shot cannot ride it. Only STT is firing-quality.
    Fixture f;
    const EntityId observer = f.spawn(0, 0, 0);
    const EntityId target = f.spawn(10.0 * kMPerNm, 0, 0);

    f.sys.addObserver(observer.index, {"t:radar"}, 0.5f, 0.5f);
    f.sys.setRadarMode(observer.index, RadarMode::Tws);
    f.check(1);

    const Contact* c = f.sys.contactsFor(observer.index)->find(target);
    REQUIRE(c != nullptr);
    CHECK(c->locked());
    CHECK_FALSE(c->firingQuality); // TWS: a soft lock
}

TEST_CASE("SensorSystem: radar STT holds a firing-quality lock on one target and ignores the rest", "[sensor_system]") {
    Fixture f;
    const EntityId observer = f.spawn(0, 0, 0);
    const EntityId a = f.spawn(8.0 * kMPerNm, 0, 0);      // dead ahead
    const EntityId b = f.spawn(9.0 * kMPerNm, 0, 1000.0); // also in the cone, slightly off

    f.sys.addObserver(observer.index, {"t:radar"}, 0.5f, 0.5f);
    f.sys.setRadarMode(observer.index, RadarMode::Stt);
    f.sys.setDesignatedTarget(observer.index, a);
    f.check(1);

    const ContactTable* tbl = f.sys.contactsFor(observer.index);
    const Contact* ca = tbl->find(a);
    REQUIRE(ca != nullptr);
    CHECK(ca->firingQuality); // the beam is dedicated to A
    CHECK(ca->locked());

    // The radar is dedicated to A, so it does not hold B on radar at all (B could still be seen by a
    // passive sensor, but this observer has only the radar).
    CHECK(tbl->find(b) == nullptr);
}

TEST_CASE("SensorSystem: STT with a stale designation auto-picks the nearest target", "[sensor_system]") {
    Fixture f;
    const EntityId observer = f.spawn(0, 0, 0);
    const EntityId near = f.spawn(5.0 * kMPerNm, 0, 0);
    const EntityId far = f.spawn(12.0 * kMPerNm, 0, 500.0);

    f.sys.addObserver(observer.index, {"t:radar"}, 0.5f, 0.5f);
    f.sys.setRadarMode(observer.index, RadarMode::Stt);
    // No designation set — a bare STT must not be silently dead; it grabs the nearest thing.
    f.check(1);

    const ContactTable* tbl = f.sys.contactsFor(observer.index);
    const Contact* cn = tbl->find(near);
    REQUIRE(cn != nullptr);
    CHECK(cn->firingQuality);
    CHECK(tbl->find(far) == nullptr); // the beam went to the nearest
}

TEST_CASE("SensorSystem: RWR hears an emitter that is painting you, and not one that is not", "[sensor_system]") {
    // The RWR is the inverse of a contact: A holds B on radar (its beam is on B), so B's receiver
    // lights up naming A. B is not emitting, so A's RWR stays quiet.
    Fixture f;
    // A at origin looking +X (default quat), B ahead looking back at A (-X).
    const float faceMinusX[4] = {0.f, 1.f, 0.f, 0.f}; // 180° about Y: nose along -X
    const EntityId a = f.spawn(0, 0, 0);
    const EntityId b = f.spawn(10.0 * kMPerNm, 0, 0, faceMinusX);

    f.sys.addObserver(a.index, {"t:radar"}, 0.5f, 0.5f);
    f.sys.addObserver(b.index, {"t:radar"}, 0.5f, 0.5f);
    f.sys.setRadarMode(a.index, RadarMode::Stt); // A hard-locks B
    f.sys.setDesignatedTarget(a.index, b);
    f.sys.setRadarMode(b.index, RadarMode::Silent); // B is dark
    f.check(1);

    const ThreatWarningSet* bThreats = f.sys.threatsFor(b.index);
    REQUIRE(bThreats != nullptr);
    REQUIRE(bThreats->size() == 1u);
    CHECK(bThreats->threats[0].emitterId.index == a.index);
    CHECK(bThreats->threats[0].level == ThreatLevel::Lock); // STT == a lock tone
    CHECK(bThreats->anyLock());

    // A is silent, so nothing is painting it: an empty receiver is a real fact, not "not evaluated".
    const ThreatWarningSet* aThreats = f.sys.threatsFor(a.index);
    REQUIRE(aThreats != nullptr);
    CHECK(aThreats->empty());
}

TEST_CASE("SensorSystem: the missile-threat provider raises a Launch that outranks a lock (#960)", "[sensor_system]") {
    // A guided missile in the air on B is not derivable from any contact table — the provider seam
    // (WorldBroadcaster wires it to the projectile pool) folds it into B's RWR as a Launch, the worst
    // level. A launch aimed at a non-observer target has no receiver to warn and is dropped.
    Fixture f;
    const float faceMinusX[4] = {0.f, 1.f, 0.f, 0.f};
    const EntityId a = f.spawn(0, 0, 0);
    const EntityId b = f.spawn(10.0 * kMPerNm, 0, 0, faceMinusX);

    f.sys.addObserver(a.index, {"t:radar"}, 0.5f, 0.5f);
    f.sys.addObserver(b.index, {"t:radar"}, 0.5f, 0.5f);
    f.sys.setRadarMode(a.index, RadarMode::Stt); // A also holds a firing-quality lock on B
    f.sys.setDesignatedTarget(a.index, b);
    f.sys.setRadarMode(b.index, RadarMode::Silent);

    f.sys.setMissileThreatProvider([&](const SensorSystem::ThreatSink& sink) {
        ThreatWarning w;
        w.emitterId = a; // stand-in for the inbound missile
        w.emitterTypeIndex = 0;
        w.channel = SensorType::Radar;
        w.level = ThreatLevel::Launch;
        sink(b.index, w);     // B carries an RWR -> warned
        sink(0xFFFFFFFFu, w); // no such observer -> silently ignored
    });
    f.check(1);

    const ThreatWarningSet* bThreats = f.sys.threatsFor(b.index);
    REQUIRE(bThreats != nullptr);
    CHECK(bThreats->anyLaunch());
    CHECK(bThreats->anyLock()); // the STT lock is still present too
    CHECK(bThreats->worst() == ThreatLevel::Launch);

    // A is silent, so nothing paints it and no missile is on it: an empty receiver.
    const ThreatWarningSet* aThreats = f.sys.threatsFor(a.index);
    REQUIRE(aThreats != nullptr);
    CHECK(aThreats->empty());
}

TEST_CASE("SensorSystem: a searching radar raises a strobe, not a lock, on the target's RWR", "[sensor_system]") {
    Fixture f;
    const float faceMinusX[4] = {0.f, 1.f, 0.f, 0.f};
    const EntityId a = f.spawn(0, 0, 0);
    const EntityId b = f.spawn(10.0 * kMPerNm, 0, 0, faceMinusX);

    f.sys.addObserver(a.index, {"t:radar"}, 0.5f, 0.5f);
    f.sys.addObserver(b.index, {"t:radar"}, 0.5f, 0.5f);
    f.sys.setRadarMode(a.index, RadarMode::Search); // scanning, not locked
    f.sys.setRadarMode(b.index, RadarMode::Silent);
    f.check(1);

    const ThreatWarningSet* bThreats = f.sys.threatsFor(b.index);
    REQUIRE(bThreats != nullptr);
    REQUIRE(bThreats->size() == 1u);
    CHECK(bThreats->threats[0].level == ThreatLevel::Search); // a strobe, not a lock
    CHECK_FALSE(bThreats->anyLock());
}

TEST_CASE("SensorSystem: contacts are identical regardless of evaluation order", "[sensor_system]") {
    // SERIAL-EQUIVALENCE. WorldBroadcaster runs this pass data-parallel, so the same server must not
    // make different decisions depending on how many cores it happens to have. The dice are seeded
    // from (observer, target, tick, slot, lobe), never drawn from shared state — so evaluating the
    // observers in reverse order must produce byte-identical tables.
    auto run = [](bool reverse) {
        Fixture f;
        std::vector<EntityId> ids;
        for (int i = 0; i < 8; ++i) {
            // A ring of aircraft, all with sensors, all seeing each other from different angles.
            const double a = i * (2.0 * 3.14159265358979 / 8.0);
            ids.push_back(f.spawn(std::cos(a) * 5000.0, 0.0, std::sin(a) * 5000.0));
        }
        for (const EntityId& id : ids)
            f.sys.addObserver(id.index, {"t:radar"}, 0.5f, 0.5f);

        for (uint64_t t = 1; t <= 5; ++t) {
            f.rebuildIndex();
            auto& work = f.sys.gatherDue(t, 1u, kSimDt);
            if (reverse) {
                for (auto it = work.rbegin(); it != work.rend(); ++it)
                    f.sys.evaluateObserver(*it, f.si, t, SensingEnvironment{}, 1.f, 0.f);
            } else {
                for (const auto& w : work)
                    f.sys.evaluateObserver(w, f.si, t, SensingEnvironment{}, 1.f, 0.f);
            }
            f.sys.updateReactions(t, kSimDt, 0.f);
        }

        // Flatten every observer's table into a comparable string.
        std::string out;
        for (const EntityId& id : ids) {
            const ContactTable* tbl = f.sys.contactsFor(id.index);
            out += std::to_string(id.index) + ":";
            for (const Contact& c : *tbl) {
                out += std::to_string(c.id.index) + "/" + std::to_string(static_cast<int>(c.state)) + "/" +
                       std::to_string(c.lastSeenTick) + ",";
            }
            out += ";";
        }
        return out;
    };

    const std::string forward = run(false);
    const std::string reverse = run(true);
    CHECK_FALSE(forward.empty());
    CHECK(forward == reverse);
}

TEST_CASE("SensorSystem: staggering spreads checks across the stride window", "[sensor_system]") {
    // At 10 Hz on a 60 Hz sim the stride is 6: an observer is due every sixth tick, and observers do
    // not all fire on the same one — otherwise one tick in six would carry the entire world's sensing
    // cost and the other five would idle.
    Fixture f;
    std::vector<EntityId> ids;
    for (int i = 0; i < 6; ++i)
        ids.push_back(f.spawn(1000.0 * i, 0, 0));
    for (const EntityId& id : ids)
        f.sys.addObserver(id.index, {"t:radar"}, 0.5f, 0.5f);

    int spread = 0;
    for (uint64_t t = 0; t < 6; ++t) {
        const std::size_t due = f.sys.gatherDue(t, 6u, kSimDt).size();
        CHECK(due <= ids.size());
        if (due > 0)
            ++spread;
    }
    CHECK(spread > 1); // work lands on more than one tick of the window

    // Every observer is checked exactly once per stride window.
    int total = 0;
    for (uint64_t t = 100; t < 106; ++t)
        total += static_cast<int>(f.sys.gatherDue(t, 6u, kSimDt).size());
    CHECK(total == static_cast<int>(ids.size()));
}

TEST_CASE("SensorSystem: removing an observer drops its table", "[sensor_system]") {
    Fixture f;
    const EntityId observer = f.spawn(0, 0, 0);
    f.sys.addObserver(observer.index, {"t:radar"}, 0.5f, 0.5f);
    REQUIRE(f.sys.contactsFor(observer.index) != nullptr);

    f.sys.removeObserver(observer.index);
    CHECK(f.sys.contactsFor(observer.index) == nullptr); // null = "not evaluated", per AiTickContext
    CHECK(f.sys.observerCount() == 0u);
}

TEST_CASE("SensorSystem: avionics failure strips the suite to eyes and drops every track", "[sensor_system]") {
    Fixture f;
    const EntityId observer = f.spawn(0, 0, 0);
    const EntityId target = f.spawn(5.0 * kMPerNm, 0, 0); // dead ahead, well inside the radar

    f.sys.addObserver(observer.index, {"t:radar"}, 1.0f, 0.5f); // skill 1 so acquisition lands fast
    for (uint64_t t = 1;
         t <= 200 && !(f.sys.contactsFor(observer.index) && f.sys.contactsFor(observer.index)->find(target) != nullptr);
         ++t)
        f.check(t);
    REQUIRE(f.sys.contactsFor(observer.index)->find(target) != nullptr);
    REQUIRE(f.sys.emitting(observer.index));

    f.sys.setAvionicsFailed(observer.index);

    // Emissions stop, and the radar's held contact is gone — the eyes must re-acquire honestly.
    CHECK_FALSE(f.sys.emitting(observer.index));
    REQUIRE(f.sys.contactsFor(observer.index) != nullptr); // still an observer: eyes, not blindness
    CHECK(f.sys.contactsFor(observer.index)->empty());

    // Idempotent, and a non-observer is a no-op.
    f.sys.setAvionicsFailed(observer.index);
    f.sys.setAvionicsFailed(9999u);
}

// ── IFF / identification (#527) ──────────────────────────────────────────────

TEST_CASE("SensorSystem: a same-faction contact is identified as a Friend", "[sensor_system][iff]") {
    Fixture f;
    const EntityId observer = f.spawn(0, 0, 0);
    const EntityId wingman = f.spawn(10.0 * kMPerNm, 0, 0);
    f.em.get(observer)->factionIndex = 1;
    f.em.get(wingman)->factionIndex = 1; // same team

    f.sys.addObserver(observer.index, {"t:radar"}, 0.5f, 0.5f);
    f.check(1);

    const Contact* c = f.sys.contactsFor(observer.index)->find(wingman);
    REQUIRE(c != nullptr);
    CHECK(c->ident == Identification::Friend); // it squawked friendly, even on a bare radar contact
}

TEST_CASE("SensorSystem: a hostile radar blip is Unknown until it is locked", "[sensor_system][iff]") {
    // No coalition registry ⇒ the affiliation fallback: distinct non-zero factions are hostile. But a
    // soft (TWS) radar contact is not a positive ID, so it stays Unknown — the ROE-critical state.
    Fixture f;
    const EntityId observer = f.spawn(0, 0, 0);
    const EntityId bandit = f.spawn(10.0 * kMPerNm, 0, 0);
    f.em.get(observer)->factionIndex = 1;
    f.em.get(bandit)->factionIndex = 2;

    f.sys.addObserver(observer.index, {"t:radar"}, 0.5f, 0.5f); // default TWS
    f.check(1);

    const Contact* blip = f.sys.contactsFor(observer.index)->find(bandit);
    REQUIRE(blip != nullptr);
    CHECK(blip->state == ContactState::Locked);    // TWS reaches Locked...
    CHECK_FALSE(blip->firingQuality);              // ...but it is a soft lock...
    CHECK(blip->ident == Identification::Unknown); // ...so not a positive ID

    // Commit an STT lock: a firing-quality track positively identifies the hostile as a Foe.
    f.sys.setRadarMode(observer.index, RadarMode::Stt);
    f.sys.setDesignatedTarget(observer.index, bandit);
    f.check(2);
    const Contact* locked = f.sys.contactsFor(observer.index)->find(bandit);
    REQUIRE(locked != nullptr);
    CHECK(locked->firingQuality);
    CHECK(locked->ident == Identification::Foe);
}

TEST_CASE("SensorSystem: a neutral (faction 0) contact is Unknown, never Foe", "[sensor_system][iff]") {
    Fixture f;
    const EntityId observer = f.spawn(0, 0, 0);
    const EntityId neutral = f.spawn(10.0 * kMPerNm, 0, 0);
    f.em.get(observer)->factionIndex = 1;
    // neutral stays faction 0

    f.sys.addObserver(observer.index, {"t:radar"}, 0.5f, 0.5f);
    f.sys.setRadarMode(observer.index, RadarMode::Stt); // even a hard lock on a neutral is not a foe
    f.sys.setDesignatedTarget(observer.index, neutral);
    f.check(1);

    const Contact* c = f.sys.contactsFor(observer.index)->find(neutral);
    REQUIRE(c != nullptr);
    CHECK(c->ident == Identification::Unknown);
}

TEST_CASE("SensorSystem: a coalition resolver makes an allied faction a Friend", "[sensor_system][iff]") {
    // With a registry, factions 1 and 2 can be ALLIES (Friendly) even though they are distinct — the
    // affiliation fallback would call them hostile. The injected resolver decides.
    Fixture f;
    f.sys.setIffResolver([](uint16_t a, uint16_t b) {
        if (a == b || (a == 1 && b == 2) || (a == 2 && b == 1))
            return FactionRelation::Friendly; // 1 and 2 are coalition partners
        return FactionRelation::Hostile;
    });

    const EntityId observer = f.spawn(0, 0, 0);
    const EntityId ally = f.spawn(10.0 * kMPerNm, 0, 0);
    f.em.get(observer)->factionIndex = 1;
    f.em.get(ally)->factionIndex = 2;

    f.sys.addObserver(observer.index, {"t:radar"}, 0.5f, 0.5f);
    f.check(1);

    const Contact* c = f.sys.contactsFor(observer.index)->find(ally);
    REQUIRE(c != nullptr);
    CHECK(c->ident == Identification::Friend); // allied, so it squawks friendly
}

// ── team track fusion (#528 datalink) ────────────────────────────────────────

#include "sensor/TrackPicture.h"

namespace {
Contact makeContact(uint32_t idx, uint16_t gen, ContactState state, Identification ident, uint8_t mask, bool firing,
                    uint64_t lastSeen, double x) {
    Contact c;
    c.id = EntityId{idx, gen};
    c.typeIndex = 7;
    c.factionIndex = 2;
    c.state = state;
    c.ident = ident;
    c.sensorTypeMask = mask;
    c.firingQuality = firing;
    c.lastSeenTick = lastSeen;
    c.lastKnownPos[0] = x;
    return c;
}
} // namespace

TEST_CASE("TrackFuser: two observers seeing different targets produce both", "[track_fuser]") {
    ContactTable a, b;
    a.contacts.push_back(makeContact(10, 1, ContactState::Detected, Identification::Unknown, 0x4, false, 5, 100.0));
    b.contacts.push_back(makeContact(20, 1, ContactState::Locked, Identification::Foe, 0x4, false, 6, 200.0));

    TrackFuser f;
    f.add(a, /*ownSensor=*/true);
    f.add(b, /*ownSensor=*/false);
    const auto tracks = f.tracks();
    REQUIRE(tracks.size() == 2u);
    CHECK(tracks[0].id.index == 10u); // sorted by index
    CHECK(tracks[0].ownSensor);       // came from the own table
    CHECK(tracks[1].id.index == 20u);
    CHECK_FALSE(tracks[1].ownSensor); // datalink-only
}

TEST_CASE("TrackFuser: the same target from two observers merges to the best state", "[track_fuser]") {
    // A teammate holds a firing-quality lock the requester only has as a soft detect: the fused track
    // is the LOCK, marked own-sensor (the requester sees it too) — this is the whole point of a
    // datalink, you inherit your wingman's better track.
    ContactTable own, mate;
    own.contacts.push_back(makeContact(30, 1, ContactState::Detected, Identification::Unknown, 0x4, false, 5, 100.0));
    mate.contacts.push_back(makeContact(30, 1, ContactState::Locked, Identification::Foe, 0x1, true, 9, 150.0));

    TrackFuser f;
    f.add(own, true);
    f.add(mate, false);
    const auto tracks = f.tracks();
    REQUIRE(tracks.size() == 1u);
    CHECK(tracks[0].state == ContactState::Locked); // freshest / best
    CHECK(tracks[0].ident == Identification::Foe);  // a Foe anywhere makes it a Foe
    CHECK(tracks[0].firingQuality);                 // OR of contributors
    CHECK(tracks[0].ownSensor);                     // the requester holds it too
    CHECK((tracks[0].sensorTypeMask & 0x5) == 0x5); // OR of masks (0x4 | 0x1)
    CHECK(tracks[0].lastKnownPos[0] == 150.0);      // position from the freshest contributor
}

TEST_CASE("TrackFuser: a Foe from any contributor wins over Unknown, order-independently", "[track_fuser]") {
    ContactTable a, b;
    a.contacts.push_back(makeContact(40, 1, ContactState::Locked, Identification::Foe, 0x4, true, 9, 10.0));
    b.contacts.push_back(makeContact(40, 1, ContactState::Detected, Identification::Unknown, 0x4, false, 3, 20.0));

    TrackFuser f1;
    f1.add(a, false);
    f1.add(b, false);
    TrackFuser f2;
    f2.add(b, false); // reverse add order
    f2.add(a, false);
    REQUIRE(f1.tracks().size() == 1u);
    REQUIRE(f2.tracks().size() == 1u);
    CHECK(f1.tracks()[0].ident == Identification::Foe);
    CHECK(f2.tracks()[0].ident == Identification::Foe); // fusion is commutative
}

// ── ECM / ECCM burn-through (#529) ───────────────────────────────────────────

TEST_CASE("SensorSystem: a jamming target denies a radar lock beyond burn-through", "[sensor_system][ecm]") {
    // Noise jamming denies RANGE: the radar still gets a bearing strobe (Detected), but no
    // firing-quality lock until you close inside burn-through (20% of the 30 nm track range = 6 nm
    // with zero ECCM). This is the whole reason to know you are jammed.
    Fixture f;
    const EntityId observer = f.spawn(0, 0, 0);
    const EntityId bandit = f.spawn(10.0 * kMPerNm, 0, 0); // 10 nm — beyond the 6 nm burn-through
    f.em.get(bandit)->ecmActive = true;

    f.sys.addObserver(observer.index, {"t:radar"}, 0.5f, 0.5f); // default TWS (would normally lock)
    f.check(1);

    const Contact* c = f.sys.contactsFor(observer.index)->find(bandit);
    REQUIRE(c != nullptr);
    CHECK(c->held());
    CHECK(c->state == ContactState::Detected); // a strobe...
    CHECK_FALSE(c->locked());                  // ...but no lock through the noise

    // Close inside burn-through (4 nm): the skin return beats the jamming and the lock comes.
    f.em.get(bandit)->transform.pos[0] = 4.0 * kMPerNm;
    f.check(2);
    const Contact* close = f.sys.contactsFor(observer.index)->find(bandit);
    REQUIRE(close != nullptr);
    CHECK(close->locked());
}

TEST_CASE("SensorSystem: ECCM extends the burn-through range", "[sensor_system][ecm]") {
    // Same jamming target at 10 nm, but a high-ECCM radar (eccm 0.5 -> burn-through 70% of 30 nm =
    // 21 nm) burns through and locks where the eccm-0 radar could not.
    Fixture f;
    f.sys.setResolver([](const std::string&) {
        SensorDef s;
        s.id = "t:radar-eccm";
        s.type = SensorType::Radar;
        s.emitter = true;
        s.search = SensorLobe{60.f, 30.f, 0.f, 40.f * kMPerNm, 1.f};
        s.track = SensorLobe{30.f, 20.f, 0.f, 30.f * kMPerNm, 1.f};
        s.lockHoldS = 4.f;
        s.eccm = 0.5f;
        return std::make_shared<const SensorDef>(s);
    });
    const EntityId observer = f.spawn(0, 0, 0);
    const EntityId bandit = f.spawn(10.0 * kMPerNm, 0, 0);
    f.em.get(bandit)->ecmActive = true;

    f.sys.addObserver(observer.index, {"t:radar-eccm"}, 0.5f, 0.5f);
    f.check(1);

    const Contact* c = f.sys.contactsFor(observer.index)->find(bandit);
    REQUIRE(c != nullptr);
    CHECK(c->locked()); // the good radar burns through the jamming at this range
}
