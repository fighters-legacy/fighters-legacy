// SPDX-License-Identifier: GPL-3.0-or-later
//
// StateMachineController's wiring rules and the Condition vocabulary (#1145).
//
// The state machine is used through a handful of well-formed graphs elsewhere. This file covers what
// it does with a graph that is WRONG — a duplicate state, a transition from or to a name that does
// not exist, a state machine with no initial state — because those are authoring mistakes that reach
// the runtime, and the contract is that each is refused loudly and leaves a machine that still runs.
//
// It also walks the Condition helpers. Those are the vocabulary a behaviour is written in, and
// several of them have a deliberate two-mode design: with a contact table they answer from what the
// entity has SENSED, and without one they fall back to ground truth. A condition that quietly
// answered from ground truth when sensing was available would give the AI knowledge it did not earn.

#include <catch2/catch_test_macros.hpp>

#include "ILogger.h"
#include "ai/StateMachineController.h"
#include "entity/EntityDef.h"
#include "entity/EntityManager.h"
#include "entity/EntityState.h"
#include "entity/EntityTypeRegistry.h"
#include "sensor/SensorSystem.h"
#include "spatial/SpatialIndex.h"

#include <memory>
#include <string>
#include <vector>

using namespace fl;
using namespace fl::ai;

namespace {

struct NullLogger final : ILogger {
    void log(LogLevel, const char*, int, const char*) override {}
    void setMinLevel(LogLevel) override {}
    void flush() override {}
};

// A child controller that reports which state built it, through the throttle channel.
class TagController final : public fl::IEntityController {
  public:
    explicit TagController(float tag) : m_tag(tag) {}
    fl::ControlInput sample(const fl::EntityState&, uint64_t, double, const fl::AiTickContext&) override {
        fl::ControlInput in{};
        in.throttle = m_tag;
        return in;
    }

  private:
    float m_tag;
};

ControllerFactory tagged(float tag) {
    return [tag] { return std::make_unique<TagController>(tag); };
}

EntityDef basicDef(const char* id = "test:basic") {
    EntityDef d;
    d.id = id;
    d.name = "Basic";
    d.category = ObjectCategory::AirVehicle;
    d.maxHp = 100.f;
    return d;
}

struct World {
    NullLogger log;
    EntityTypeRegistry reg;
    EntityManager em;

    World() : em(log, reg) {
        reg.registerType(basicDef());
    }

    // SpatialIndex takes explicit inserts; mirror every live entity into it.
    void indexAll(SpatialIndex& si) const {
        si.clear();
        em.forEach([&si](const fl::EntityState& s) {
            if (!s.dead)
                si.insert(s.id.index, s.transform.pos);
        });
    }

    EntityId spawnAt(double x, double y, double z, uint8_t faction = 1) {
        EntityTransform t{};
        t.quat[3] = 1.f;
        t.pos[0] = x;
        t.pos[1] = y;
        t.pos[2] = z;
        const EntityId id = em.spawn("test:basic", t);
        if (EntityState* s = em.get(id))
            s->factionIndex = faction;
        return id;
    }
};

fl::AiTickContext emptyCtx() {
    return fl::AiTickContext{};
}

} // namespace

// ---------------------------------------------------------------------------
// Graphs that are wrong
// ---------------------------------------------------------------------------

TEST_CASE("StateMachineController: a machine with no initial state samples neutral (#1145)", "[ai][statemachine]") {
    // A graph the author never finished wiring. It must not dereference a null child every tick.
    World w;
    StateMachineController sm(w.em);
    sm.addState("patrol", tagged(0.5f));

    const EntityId id = w.spawnAt(0, 1000, 0);
    const fl::ControlInput in = sm.sample(*w.em.get(id), 1, 1.0 / 60.0, emptyCtx());
    CHECK(in.throttle == 0.f);
    CHECK(sm.currentState().empty());
}

TEST_CASE("StateMachineController: a duplicate state name keeps the first registration (#1145)", "[ai][statemachine]") {
    // Silently replacing it would mean the behaviour that runs depends on file order.
    World w;
    StateMachineController sm(w.em);
    sm.addState("patrol", tagged(0.25f));
    sm.addState("patrol", tagged(0.75f)); // ignored
    sm.setInitialState("patrol");

    const EntityId id = w.spawnAt(0, 1000, 0);
    CHECK(sm.sample(*w.em.get(id), 1, 1.0 / 60.0, emptyCtx()).throttle == 0.25f);
}

TEST_CASE("StateMachineController: a transition from an unknown state is dropped (#1145)", "[ai][statemachine]") {
    World w;
    StateMachineController sm(w.em);
    sm.addState("patrol", tagged(0.25f));
    sm.addTransition("typo_patrol", "patrol", Always()); // from a state that does not exist
    sm.setInitialState("patrol");

    const EntityId id = w.spawnAt(0, 1000, 0);
    CHECK(sm.sample(*w.em.get(id), 1, 1.0 / 60.0, emptyCtx()).throttle == 0.25f);
    CHECK(sm.currentState() == "patrol");
}

TEST_CASE("StateMachineController: a transition to an unknown state does not move (#1145)", "[ai][statemachine]") {
    // It is consumed rather than falling through to the next transition, so a typo cannot silently
    // promote the transition below it.
    World w;
    StateMachineController sm(w.em);
    sm.addState("patrol", tagged(0.25f));
    sm.addState("engage", tagged(0.9f));
    sm.addTransition("patrol", "typo_engage", Always());
    sm.addTransition("patrol", "engage", Always()); // would fire if the broken one fell through
    sm.setInitialState("patrol");

    const EntityId id = w.spawnAt(0, 1000, 0);
    sm.sample(*w.em.get(id), 1, 1.0 / 60.0, emptyCtx());
    CHECK(sm.currentState() == "patrol");
}

TEST_CASE("StateMachineController: setting an unknown initial state leaves the machine idle (#1145)",
          "[ai][statemachine]") {
    World w;
    StateMachineController sm(w.em);
    sm.addState("patrol", tagged(0.25f));
    sm.setInitialState("nonexistent");
    CHECK(sm.currentState().empty());
}

TEST_CASE("StateMachineController: a self-transition is a no-op and lets later ones be tested (#1145)",
          "[ai][statemachine]") {
    // Re-entering the same state would rebuild the child and wipe its internal progress — a waypoint
    // controller would restart at waypoint zero every tick.
    World w;
    StateMachineController sm(w.em);
    sm.addState("patrol", tagged(0.25f));
    sm.addState("engage", tagged(0.9f));
    sm.addTransition("patrol", "patrol", Always()); // self
    sm.addTransition("patrol", "engage", Always()); // must still be reached
    sm.setInitialState("patrol");

    const EntityId id = w.spawnAt(0, 1000, 0);
    sm.sample(*w.em.get(id), 1, 1.0 / 60.0, emptyCtx());
    CHECK(sm.currentState() == "engage");
}

TEST_CASE("StateMachineController: the outgoing child drives the tick it leaves on (#1145)", "[ai][statemachine]") {
    // Sample-first semantics. Switching output on the same tick as the transition would emit a
    // control input from a child that has not seen the world yet.
    World w;
    StateMachineController sm(w.em);
    sm.addState("patrol", tagged(0.25f));
    sm.addState("engage", tagged(0.9f));
    sm.addTransition("patrol", "engage", Always());
    sm.setInitialState("patrol");

    const EntityId id = w.spawnAt(0, 1000, 0);
    CHECK(sm.sample(*w.em.get(id), 1, 1.0 / 60.0, emptyCtx()).throttle == 0.25f); // the outgoing child
    CHECK(sm.currentState() == "engage");
    CHECK(sm.sample(*w.em.get(id), 2, 1.0 / 60.0, emptyCtx()).throttle == 0.9f);
}

TEST_CASE("StateMachineController: minimum dwell holds a transition back (#1145)", "[ai][statemachine]") {
    // Hysteresis. Without it, a condition sitting on its threshold flips the machine every tick and
    // the aircraft never actually does either behaviour.
    World w;
    StateMachineController sm(w.em);
    sm.addState("patrol", tagged(0.25f));
    sm.addState("engage", tagged(0.9f));
    sm.addTransition("patrol", "engage", Always(), /*minDwellSeconds=*/2.0f);
    sm.setInitialState("patrol");

    const EntityId id = w.spawnAt(0, 1000, 0);
    for (int i = 0; i < 60; ++i) // 1 second
        sm.sample(*w.em.get(id), static_cast<uint64_t>(i), 1.0 / 60.0, emptyCtx());
    CHECK(sm.currentState() == "patrol");

    for (int i = 0; i < 90; ++i) // past two seconds total
        sm.sample(*w.em.get(id), static_cast<uint64_t>(60 + i), 1.0 / 60.0, emptyCtx());
    CHECK(sm.currentState() == "engage");
}

// ---------------------------------------------------------------------------
// The Condition vocabulary
// ---------------------------------------------------------------------------

TEST_CASE("Conditions: range tests against a target that is gone (#1145)", "[ai][statemachine]") {
    // A dead or reaped target is NOT "in range" and IS "beyond range". Answering the other way would
    // hold an aircraft in a pursuit of nothing.
    World w;
    const EntityId self = w.spawnAt(0, 1000, 0);
    const EntityId target = w.spawnAt(1000, 1000, 0, /*faction=*/2);
    const fl::EntityState& s = *w.em.get(self);
    const fl::AiTickContext ctx = emptyCtx();

    CHECK(ThreatWithinRange(target, 5000.f)(s, w.em, ctx));
    CHECK_FALSE(ThreatBeyondRange(target, 5000.f)(s, w.em, ctx));
    CHECK_FALSE(ThreatWithinRange(target, 500.f)(s, w.em, ctx));
    CHECK(ThreatBeyondRange(target, 500.f)(s, w.em, ctx));

    w.em.kill(target);
    w.em.onTick(1.0 / 60.0, 1);
    CHECK_FALSE(ThreatWithinRange(target, 5000.f)(s, w.em, ctx));
    CHECK(ThreatBeyondRange(target, 5000.f)(s, w.em, ctx));

    // An id that was never valid behaves the same.
    CHECK_FALSE(ThreatWithinRange(EntityId{}, 5000.f)(s, w.em, ctx));
}

TEST_CASE("Conditions: HpBelow reads a fraction of max, not an absolute (#1145)", "[ai][statemachine]") {
    World w;
    const EntityId self = w.spawnAt(0, 1000, 0);
    fl::EntityState& s = *w.em.get(self);
    const fl::AiTickContext ctx = emptyCtx();

    CHECK_FALSE(HpBelow(0.25f)(s, w.em, ctx)); // full health
    s.hp = 20.f;                               // of 100
    CHECK(HpBelow(0.25f)(s, w.em, ctx));
    CHECK_FALSE(HpBelow(0.1f)(s, w.em, ctx));
}

TEST_CASE("Conditions: a neutral entity has no enemies (#1145)", "[ai][statemachine]") {
    // Faction 0 is neutral. A neutral entity that "detected a hostile" would attack a world it has
    // no quarrel with — so the check short-circuits before any geometry runs.
    World w;
    SpatialIndex si;
    const EntityId self = w.spawnAt(0, 1000, 0, /*faction=*/0);
    w.spawnAt(100, 1000, 0, /*faction=*/2);
    w.indexAll(si);

    fl::AiTickContext ctx = emptyCtx();
    ctx.si = &si;
    const fl::EntityState& s = *w.em.get(self);

    CHECK_FALSE(AnyHostileEntityWithinRange(5000.f)(s, w.em, ctx));
    CHECK_FALSE(DetectedHostileWithinRange(5000.f)(s, w.em, ctx));
}

TEST_CASE("Conditions: without a spatial index a proximity test answers no (#1145)", "[ai][statemachine]") {
    // A null pointer in AiTickContext means "not evaluated here", not "empty". Answering yes on no
    // information would have every AI react to threats it cannot possibly know about.
    World w;
    const EntityId self = w.spawnAt(0, 1000, 0);
    w.spawnAt(10, 1000, 0, /*faction=*/2);
    const fl::EntityState& s = *w.em.get(self);

    CHECK_FALSE(AnyHostileEntityWithinRange(5000.f)(s, w.em, emptyCtx()));
    CHECK_FALSE(AnyHostileEntityWithinRangeOf(self, 5000.f)(s, w.em, emptyCtx()));
}

TEST_CASE("Conditions: an escort's threat test is geometry about the escortee (#1145)", "[ai][statemachine]") {
    // The escort asks "is anything hostile near the thing I am protecting", not "near me". Those are
    // different questions the moment the escort strays.
    World w;
    SpatialIndex si;
    const EntityId escort = w.spawnAt(0, 1000, 0, /*faction=*/1);
    const EntityId vip = w.spawnAt(50000, 1000, 0, /*faction=*/1);
    w.spawnAt(50500, 1000, 0, /*faction=*/2); // hostile, close to the VIP, far from the escort
    w.indexAll(si);

    fl::AiTickContext ctx = emptyCtx();
    ctx.si = &si;
    const fl::EntityState& s = *w.em.get(escort);

    CHECK(AnyHostileEntityWithinRangeOf(vip, 2000.f)(s, w.em, ctx));
    CHECK_FALSE(AnyHostileEntityWithinRange(2000.f)(s, w.em, ctx)); // nothing near the escort itself

    // Nothing left to protect once the VIP is gone.
    w.em.kill(vip);
    w.em.onTick(1.0 / 60.0, 1);
    w.indexAll(si);
    CHECK_FALSE(AnyHostileEntityWithinRangeOf(vip, 2000.f)(s, w.em, ctx));
    CHECK_FALSE(AnyHostileEntityWithinRangeOf(EntityId{}, 2000.f)(s, w.em, ctx));
}

TEST_CASE("Conditions: a friendly in range is not a threat (#1145)", "[ai][statemachine]") {
    World w;
    SpatialIndex si;
    const EntityId self = w.spawnAt(0, 1000, 0, /*faction=*/1);
    w.spawnAt(100, 1000, 0, /*faction=*/1); // same faction
    w.indexAll(si);

    fl::AiTickContext ctx = emptyCtx();
    ctx.si = &si;
    const fl::EntityState& s = *w.em.get(self);

    CHECK(AnyEntityWithinRange(5000.f)(s, w.em, ctx));              // something is there...
    CHECK_FALSE(AnyHostileEntityWithinRange(5000.f)(s, w.em, ctx)); // ...but it is one of ours
}

TEST_CASE("Conditions: altitude and ground-speed gates (#1145)", "[ai][statemachine]") {
    // The departure climb-out gate and the arrival rollout gate. Ground speed is HORIZONTAL, so a
    // descending aircraft is not "stopped".
    World w;
    const EntityId self = w.spawnAt(0, 3000, 0);
    fl::EntityState& s = *w.em.get(self);
    const fl::AiTickContext ctx = emptyCtx();

    CHECK(AboveAltitude(1000.f)(s, w.em, ctx));
    CHECK_FALSE(AboveAltitude(5000.f)(s, w.em, ctx));

    s.transform.vel[0] = 1.f;
    s.transform.vel[1] = -50.f; // dropping fast, but not moving over the ground
    s.transform.vel[2] = 0.f;
    CHECK(GroundSpeedBelow(5.f)(s, w.em, ctx));

    s.transform.vel[0] = 80.f;
    CHECK_FALSE(GroundSpeedBelow(5.f)(s, w.em, ctx));
}

TEST_CASE("Conditions: the logical combinators (#1145)", "[ai][statemachine]") {
    World w;
    const EntityId self = w.spawnAt(0, 1000, 0);
    const fl::EntityState& s = *w.em.get(self);
    const fl::AiTickContext ctx = emptyCtx();

    const Condition yes = Always();
    const Condition no = Not(Always());

    CHECK(yes(s, w.em, ctx));
    CHECK_FALSE(no(s, w.em, ctx));
    CHECK(And(Always(), Always())(s, w.em, ctx));
    CHECK_FALSE(And(Always(), Not(Always()))(s, w.em, ctx));
    CHECK_FALSE(And(Not(Always()), Always())(s, w.em, ctx)); // the short-circuit arm
    CHECK(Or(Not(Always()), Always())(s, w.em, ctx));
    CHECK(Or(Always(), Not(Always()))(s, w.em, ctx));
    CHECK_FALSE(Or(Not(Always()), Not(Always()))(s, w.em, ctx));
}

// ---------------------------------------------------------------------------
// The sensing-gated conditions (#690)
// ---------------------------------------------------------------------------

TEST_CASE("Conditions: with no contact table the sensing gates fall back to ground truth (#1145)",
          "[ai][statemachine][sensing]") {
    // A null contact table means sensing was not evaluated on this path — the pre-#690 behaviour —
    // rather than "the sensors ran and saw nothing".
    World w;
    SpatialIndex si;
    const EntityId self = w.spawnAt(0, 1000, 0, /*faction=*/1);
    const EntityId bandit = w.spawnAt(1000, 1000, 0, /*faction=*/2);
    w.indexAll(si);

    fl::AiTickContext ctx = emptyCtx();
    ctx.si = &si; // but no contacts
    const fl::EntityState& s = *w.em.get(self);

    CHECK(DetectedHostileWithinRange(5000.f)(s, w.em, ctx));
    CHECK(DetectsThreatWithinRange(bandit, 5000.f)(s, w.em, ctx));
    CHECK_FALSE(LostContact(bandit, 5000.f)(s, w.em, ctx));
}

TEST_CASE("Conditions: an unreacted contact is seen but not yet noticed (#1145)", "[ai][statemachine][sensing]") {
    // The reaction delay is not a formality. A contact the pilot has not reacted to yet must not
    // trigger a behaviour change, or the delay buys nothing.
    World w;
    const EntityId self = w.spawnAt(0, 1000, 0, /*faction=*/1);
    const EntityId bandit = w.spawnAt(1000, 1000, 0, /*faction=*/2);
    const fl::EntityState& s = *w.em.get(self);

    sensor::ContactTable table;
    sensor::Contact c{};
    c.id = bandit;
    c.factionIndex = 2;
    c.state = sensor::ContactState::Detected;
    c.reacted = false;
    c.lastKnownPos[0] = 1000.0;
    c.lastKnownPos[1] = 1000.0;
    table.contacts.push_back(c);

    fl::AiTickContext ctx = emptyCtx();
    ctx.contacts = &table;

    CHECK_FALSE(DetectedHostileWithinRange(5000.f)(s, w.em, ctx));
    CHECK_FALSE(DetectsThreatWithinRange(bandit, 5000.f)(s, w.em, ctx));

    table.contacts[0].reacted = true;
    CHECK(DetectedHostileWithinRange(5000.f)(s, w.em, ctx));
    CHECK(DetectsThreatWithinRange(bandit, 5000.f)(s, w.em, ctx));
}

TEST_CASE("Conditions: range is measured to where the target was last SEEN (#1145)", "[ai][statemachine][sensing]") {
    // The whole point of #690: the AI reasons about its own picture. A contact coasting at a stale
    // position is judged at that position, not at the target's true one.
    World w;
    const EntityId self = w.spawnAt(0, 1000, 0, /*faction=*/1);
    const EntityId bandit = w.spawnAt(200, 1000, 0, /*faction=*/2); // truly very close
    const fl::EntityState& s = *w.em.get(self);

    sensor::ContactTable table;
    sensor::Contact c{};
    c.id = bandit;
    c.factionIndex = 2;
    c.state = sensor::ContactState::Coasting;
    c.reacted = true;
    c.lastKnownPos[0] = 40000.0; // but last seen a long way off
    c.lastKnownPos[1] = 1000.0;
    table.contacts.push_back(c);

    fl::AiTickContext ctx = emptyCtx();
    ctx.contacts = &table;

    CHECK_FALSE(DetectsThreatWithinRange(bandit, 5000.f)(s, w.em, ctx));
    CHECK(LostContact(bandit, 5000.f)(s, w.em, ctx));
}

TEST_CASE("Conditions: a contact that was never held is lost (#1145)", "[ai][statemachine][sensing]") {
    World w;
    const EntityId self = w.spawnAt(0, 1000, 0, /*faction=*/1);
    const EntityId bandit = w.spawnAt(200, 1000, 0, /*faction=*/2);
    const fl::EntityState& s = *w.em.get(self);

    sensor::ContactTable empty; // the sensors ran and saw nothing
    fl::AiTickContext ctx = emptyCtx();
    ctx.contacts = &empty;

    CHECK(LostContact(bandit, 5000.f)(s, w.em, ctx));
    CHECK_FALSE(DetectsThreatWithinRange(bandit, 5000.f)(s, w.em, ctx));
    CHECK_FALSE(DetectedHostileWithinRange(5000.f)(s, w.em, ctx));
    CHECK(NoContacts()(s, w.em, ctx));
    CHECK_FALSE(HasLockedContact()(s, w.em, ctx));
}

TEST_CASE("Conditions: HasLockedContact wants a lock, not a detection (#1145)", "[ai][statemachine][sensing]") {
    World w;
    const EntityId self = w.spawnAt(0, 1000, 0, /*faction=*/1);
    const EntityId bandit = w.spawnAt(200, 1000, 0, /*faction=*/2);
    const fl::EntityState& s = *w.em.get(self);

    sensor::ContactTable table;
    sensor::Contact c{};
    c.id = bandit;
    c.factionIndex = 2;
    c.state = sensor::ContactState::Detected;
    c.reacted = true;
    table.contacts.push_back(c);

    fl::AiTickContext ctx = emptyCtx();
    ctx.contacts = &table;
    CHECK_FALSE(HasLockedContact()(s, w.em, ctx));
    CHECK_FALSE(NoContacts()(s, w.em, ctx)); // there IS a contact, just not a locked one

    table.contacts[0].state = sensor::ContactState::Locked;
    CHECK(HasLockedContact()(s, w.em, ctx));
}
