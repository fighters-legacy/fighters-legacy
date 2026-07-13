// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "entity/EntityId.h"
#include "entity/EntityState.h" // full type required — used in std::function signature
#include "entity/IEntityController.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fl {
class EntityManager; // forward-declare; pointer/ref only in this header
} // namespace fl

namespace fl::ai {

// Condition evaluated at the end of each tick to test outgoing transitions.
//   self — current state of the controlled entity
//   em   — entity manager (for target lookups via em.get())
//   ctx  — the per-tick world view (spatial index, contacts, environment, difficulty); every
//          field may be null, meaning "not evaluated here" — see entity/AiTickContext.h
using Condition =
    std::function<bool(const fl::EntityState& self, const fl::EntityManager& em, const fl::AiTickContext& ctx)>;

// Produces a fresh child controller on state entry. Called once per state entry;
// the returned controller is owned until the state is exited or re-entered.
using ControllerFactory = std::function<std::unique_ptr<fl::IEntityController>()>;

// ---------------------------------------------------------------------------
// StateMachineController
//
// Sequences IEntityController child controllers based on Condition-gated transitions.
// Exactly one named state is active at a time. On each tick:
//   1. Delegate sample() to the active child controller (sample-first semantics:
//      the outgoing child drives this tick's control output).
//   2. Test outgoing transitions in priority (insertion) order. The first Condition
//      that returns true fires — the target state is entered, a fresh child is
//      constructed via its ControllerFactory, and the dwell timer is reset.
//
// minDwellSeconds on a transition prevents it from firing until the current state
// has been active for at least that many seconds (hysteresis, prevents oscillation).
//
// Example — patrol-attack-retreat:
//   auto sm = std::make_unique<StateMachineController>(em);
//   sm->addState("patrol",  [&wps]{ return std::make_unique<WaypointController>(wps, 500.f, 0.7f, true); });
//   sm->addState("engage",  [&em,tgt]{ return std::make_unique<PursuitController>(em, tgt, 0.85f, true); });
//   sm->addState("retreat", [&em,tgt]{ return std::make_unique<EvadeController>(em, tgt); });
//   sm->addTransition("patrol",  "engage",  ThreatWithinRange(tgt, 8000.f));
//   sm->addTransition("engage",  "retreat", HpBelow(0.25f));
//   sm->addTransition("engage",  "patrol",  ThreatBeyondRange(tgt, 12000.f), 2.f);
//   sm->addTransition("retreat", "engage",  And(Not(HpBelow(0.25f)), ThreatWithinRange(tgt, 6000.f)));
//   sm->setInitialState("patrol");
//
// Threading: sim-thread only (same contract as all IEntityController implementations).
// ---------------------------------------------------------------------------
class StateMachineController : public fl::IEntityController {
  public:
    explicit StateMachineController(const fl::EntityManager& entityManager);

    // Register a named state. factory() is called fresh on every entry; the controller
    // is destroyed on exit, automatically resetting any internal mutable state
    // (e.g. BreakTurnController phase timer, WaypointController waypoint index).
    // Calling addState with a duplicate name is a configuration error; the second call
    // is silently ignored (first registration wins).
    void addState(std::string name, ControllerFactory factory);

    // Add a priority-ordered outgoing transition from -> to, triggered when cond
    // returns true. Transitions for a state are tested in insertion order; the first
    // matching condition fires. minDwellSeconds > 0 suppresses the transition until
    // the state has been active for at least that many wall-clock seconds.
    // If `from` does not name a registered state, a warning is printed to stderr and
    // the call is a no-op.
    void addTransition(std::string from, std::string to, Condition cond, float minDwellSeconds = 0.f);

    // Set the initial active state and construct its child controller immediately.
    // Must be called before the first sample(). Prints a warning to stderr and leaves
    // the controller uninitialised if name is unknown.
    void setInitialState(const std::string& name);

    fl::ControlInput sample(const fl::EntityState& state, uint64_t tick, double dt,
                            const fl::AiTickContext& ctx = {}) override;

    // Name of the currently active state. Empty string if setInitialState() has not
    // been called or named an unknown state.
    [[nodiscard]] const std::string& currentState() const noexcept;

  private:
    struct Transition {
        std::string to;
        Condition cond;
        float minDwellSeconds{0.f};
    };
    struct State {
        std::string name;
        ControllerFactory factory;
        std::vector<Transition> transitions;
    };

    const fl::EntityManager& m_entityManager;
    std::vector<State> m_states;
    int m_activeIdx{-1};
    std::unique_ptr<fl::IEntityController> m_active;
    float m_dwellTime{0.f};

    [[nodiscard]] int findState(const std::string& name) const noexcept;
    void enterState(int idx);
};

// ---------------------------------------------------------------------------
// Built-in Condition helpers
//
// All helpers capture only ids/scalars; em and ctx arrive as parameters at
// call time via StateMachineController::sample().
// ---------------------------------------------------------------------------

// True when targetId is alive and within rangeM (3-D Euclidean distance) of self.
Condition ThreatWithinRange(fl::EntityId targetId, float rangeM);

// True when targetId is dead, invalid, or farther than rangeM from self.
Condition ThreatBeyondRange(fl::EntityId targetId, float rangeM);

// True when self.hp / self.maxHp < fraction. Never fires when maxHp == 0.
Condition HpBelow(float fraction);

// True when any entity other than self is found within rangeM via the SpatialIndex.
// Returns false when ctx.si == nullptr (e.g. in tests without a spatial index).
Condition AnyEntityWithinRange(float rangeM);

// True when any HOSTILE entity (per fl::areFactionsHostile against self's factionIndex) is
// found within rangeM via the SpatialIndex. Same-faction friendlies and neutral (faction 0)
// entities are ignored, so an escort no longer triggers on the entity it is protecting.
// Returns false when ctx.si == nullptr or self is neutral (faction 0 has no enemies).
Condition AnyHostileEntityWithinRange(float rangeM);

// True when any entity HOSTILE TO SELF is within rangeM of `anchorId` — geometry about the anchor,
// hostility about self. This is the wingman's `cover_me` trigger (#610): the threats that matter are
// the ones closing on the entity being PROTECTED, not the ones near the escort. Judging hostility
// against self (not against the anchor) is what lets a neutral-faction anchor still be covered by a
// factioned wingman, and keeps the semantics identical to every other condition here.
// AnyHostileEntityWithinRange is the degenerate case where the anchor is self.
// Returns false when ctx.si == nullptr, self is neutral (faction 0 has no enemies), or the anchor is
// dead/invalid.
Condition AnyHostileEntityWithinRangeOf(fl::EntityId anchorId, float rangeM);

// ── Sensing-gated conditions (#690) ─────────────────────────────────────────
//
// These are the honest versions of the conditions above: they read ctx.contacts — what the entity
// has actually DETECTED — instead of querying the world. An AI built on these cannot react to
// something it has not seen, which is the entire point of #670.
//
// NULL-CONTACTS SEMANTICS (normative, and what keeps every existing test valid): when
// `ctx.contacts == nullptr`, sensing was NOT EVALUATED (a unit test, a headless caller), and each of
// these behaves as its ground-truth ancestor. A null table is not "sees nothing"; an EMPTY table is.

// True when the entity has a REACTED hostile contact whose last-known position is within rangeM.
//
// Three things it deliberately does NOT do: it does not see through the entity's sensor cones (a
// bandit behind a visual-only unit is simply not in the table); it does not fire before the reaction
// delay has elapsed (`Contact::reacted` — seeing is not the same as noticing); and it does not drop
// a contact the instant a target beams or masks — a coasting contact still counts, at its LAST-KNOWN
// position, until its coast expires. That last one is why the AI does not twitch back to patrol the
// moment a target's radar return fades.
//
// Falls back to AnyHostileEntityWithinRange when sensing was not evaluated.
Condition DetectedHostileWithinRange(float rangeM);

// True when the entity holds a reacted contact on targetId, within rangeM of its last-known
// position. The specific-target gate for pursuit/evade wiring.
// Falls back to ThreatWithinRange when sensing was not evaluated.
Condition DetectsThreatWithinRange(fl::EntityId targetId, float rangeM);

// True when the entity has NO contact on targetId at all — it never detected it, or the track has
// dropped after coasting. The "I have lost him" edge.
// Falls back to ThreatBeyondRange(targetId, rangeM) when sensing was not evaluated.
Condition LostContact(fl::EntityId targetId, float rangeM);

// True when the entity holds a firing-quality track (a Locked contact) on anything.
// Returns false when sensing was not evaluated — a controller cannot claim a lock it never took.
Condition HasLockedContact();

// True when the contact table is empty — the sensors ran and found nothing. The "go back to patrol"
// edge. Pair it with a transition `minDwellSeconds` if you want the AI to stay committed for a while
// before giving up; the sensor's own `lock_hold_s` coast already provides the short-term hysteresis,
// which is why this needs no grace timer of its own (a timer here would be a second, invented,
// version of a mechanism the content already authors).
// Returns false when sensing was not evaluated (an AI with no sensors must not conclude "all clear").
Condition NoContacts();

// Always returns true. Useful as a final fallback transition.
Condition Always();

// Logical combinators. Conditions are moved into the returned lambda.
Condition And(Condition a, Condition b);
Condition Or(Condition a, Condition b);
Condition Not(Condition a);

} // namespace fl::ai
