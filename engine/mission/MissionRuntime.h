// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The mission objective / trigger evaluator (#633). It runs server-authoritatively at a second-scale
// cadence (NOT every 60 Hz tick): each trigger's `on` predicate is tested in declaration order, and the
// first time it becomes true its `do` action fires exactly once. mission_success / mission_failure drive
// a single objective state machine (Active -> Complete / Failed); other actions are handed to an
// injected dispatcher (fl-server routes them through the same validated command path as the admin
// console, so a mission cannot do anything an operator could not).
//
// Determinism: triggers fire in file order, edge-once, off a fixed sim-dt derived tick clock — no
// wall-clock, no RNG. That is what lets the #856 headless harness assert a mission's outcome.
//
// Lives in the ENGINE (engine-mission) so the single-player mission lifecycle (#634) drives the same
// evaluator the dedicated server does. It reads entity liveness through EntityManager (already an
// engine-mission dependency); it never reaches the network or fl-server.

#include "entity/EntityId.h"
#include "mission/Mission.h"

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace fl {

class EntityManager;

enum class MissionState : uint8_t { Active, Complete, Failed };

struct MissionOutcome {
    MissionState state{MissionState::Active};
    double elapsedSeconds{0.0};
    uint32_t triggersFired{0};
};

class MissionRuntime {
  public:
    // A non-terminal `do` action (spawn / message / weather / music ...) the runtime cannot resolve
    // itself. fl-server wires this to the admin command dispatch; unset = the action is logged-and-
    // skipped (the evaluator never spawns or mutates the world directly).
    using ActionDispatch = std::function<void(std::string_view)>;
    // Fired once, when the objective state first becomes terminal (Complete or Failed).
    using EndHook = std::function<void(const MissionOutcome&)>;

    // `objectEntities` is MissionSetupResult::objectEntities (mission object id -> spawned EntityId).
    MissionRuntime(const Mission& mission, std::vector<std::pair<std::string, EntityId>> objectEntities,
                   EntityManager& em, ActionDispatch dispatch = {});

    // Call once per sim tick with the monotonically increasing tick index. Evaluates triggers only
    // every `evalIntervalTicks` (and on the first call), so predicate checks run at ~1 Hz, not 60 Hz.
    void step(uint64_t tickIndex);

    // Bind a mission object id to a live entity, or unbind it with an invalid EntityId (#884). The
    // caller (fl-server) wires this to the connect handshake so a pilot claiming a player slot registers
    // its spawned aircraft under the slot's id, and destroy(<slot>) tracks the real aircraft instead of
    // reading "never spawned -> destroyed" from t=0. Sim-thread only.
    void registerObjectEntity(const std::string& objectId, EntityId eid);

    [[nodiscard]] const MissionOutcome& outcome() const noexcept {
        return m_outcome;
    }
    [[nodiscard]] bool done() const noexcept {
        return m_outcome.state != MissionState::Active;
    }

    void setSimDt(double dt) noexcept {
        m_simDt = dt;
    }
    void setEvalIntervalTicks(uint32_t ticks) noexcept {
        m_evalIntervalTicks = ticks == 0 ? 1u : ticks;
    }
    void setOnEnd(EndHook hook) {
        m_onEnd = std::move(hook);
    }

  private:
    // True when the object named by a `destroy(<id>)` predicate is gone: no live entity, or the entity
    // is dead. An id we never spawned (spawn failed / unknown) counts as destroyed.
    [[nodiscard]] bool isObjectDestroyed(const std::string& objectId) const;
    // Evaluate one trigger's `on` predicate at the current elapsed time. Unknown/unsupported predicates
    // never fire here (they are left to Lua per missions.md; the parser passed them through).
    [[nodiscard]] bool evaluatePredicate(const std::string& on) const;
    // Execute one trigger's `do` action. Returns true if it drove the objective to a terminal state.
    bool executeAction(const std::string& doAction);

    std::vector<MissionTrigger> m_triggers;
    std::vector<bool> m_fired; // parallel to m_triggers; edge-once
    std::vector<std::pair<std::string, EntityId>> m_objectEntities;
    EntityManager& m_em;
    ActionDispatch m_dispatch;
    EndHook m_onEnd;

    MissionOutcome m_outcome;
    double m_simDt{1.0 / 60.0};
    uint32_t m_evalIntervalTicks{60}; // ~1 s at 60 Hz
    uint64_t m_startTick{0};
    bool m_started{false};
    uint64_t m_lastEvalTick{0};
};

} // namespace fl
