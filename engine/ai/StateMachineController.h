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
class SpatialIndex;  // forward-declare; pointer only in this header
} // namespace fl

namespace fl::ai {

// Condition evaluated at the end of each tick to test outgoing transitions.
//   self — current state of the controlled entity
//   em   — entity manager (for target lookups via em.get())
//   si   — spatial index rebuilt this tick by WorldBroadcaster; nullptr in tests
using Condition =
    std::function<bool(const fl::EntityState& self, const fl::EntityManager& em, const fl::SpatialIndex* si)>;

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
                            const fl::SpatialIndex* si = nullptr) override;

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
// All helpers capture only ids/scalars; em and si arrive as parameters at
// call time via StateMachineController::sample().
// ---------------------------------------------------------------------------

// True when targetId is alive and within rangeM (3-D Euclidean distance) of self.
Condition ThreatWithinRange(fl::EntityId targetId, float rangeM);

// True when targetId is dead, invalid, or farther than rangeM from self.
Condition ThreatBeyondRange(fl::EntityId targetId, float rangeM);

// True when self.hp / self.maxHp < fraction. Never fires when maxHp == 0.
Condition HpBelow(float fraction);

// True when any entity other than self is found within rangeM via the SpatialIndex.
// Returns false when si == nullptr (e.g. in tests without a spatial index).
Condition AnyEntityWithinRange(float rangeM);

// Always returns true. Useful as a final fallback transition.
Condition Always();

// Logical combinators. Conditions are moved into the returned lambda.
Condition And(Condition a, Condition b);
Condition Or(Condition a, Condition b);
Condition Not(Condition a);

} // namespace fl::ai
