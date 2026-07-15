// SPDX-License-Identifier: GPL-3.0-or-later
#include "mission/MissionRuntime.h"

#include "entity/EntityManager.h"
#include "entity/EntityState.h"

#include <cstdlib>

namespace fl {

namespace {

// Extract the single argument of a `name(<arg>)` predicate/action, e.g. destroy(sam1) -> "sam1".
// Returns empty when the string is not of that shape.
std::string parenArg(const std::string& s, const char* name) {
    const std::string prefix = std::string(name) + "(";
    if (s.size() < prefix.size() + 1 || s.compare(0, prefix.size(), prefix) != 0 || s.back() != ')')
        return {};
    return s.substr(prefix.size(), s.size() - prefix.size() - 1);
}

} // namespace

MissionRuntime::MissionRuntime(const Mission& mission, std::vector<std::pair<std::string, EntityId>> objectEntities,
                               EntityManager& em, ActionDispatch dispatch)
    : m_triggers(mission.triggers), m_fired(mission.triggers.size(), false),
      m_objectEntities(std::move(objectEntities)), m_em(em), m_dispatch(std::move(dispatch)) {}

bool MissionRuntime::isObjectDestroyed(const std::string& objectId) const {
    for (const auto& [id, eid] : m_objectEntities) {
        if (id != objectId)
            continue;
        const EntityState* s = m_em.get(eid);
        return !s || s->dead; // gone or dead
    }
    return true; // never spawned (unknown id / spawn failed) -> treat as destroyed
}

bool MissionRuntime::evaluatePredicate(const std::string& on) const {
    if (on == "mission_start")
        return true; // fires on the first evaluation
    if (const std::string secs = parenArg(on, "timer"); !secs.empty()) {
        const double t = std::strtod(secs.c_str(), nullptr);
        return m_outcome.elapsedSeconds >= t;
    }
    if (const std::string obj = parenArg(on, "destroy"); !obj.empty())
        return isObjectDestroyed(obj);
    // Anything else (reach/zone extensions, or a Lua-only predicate per missions.md) never fires here.
    return false;
}

bool MissionRuntime::executeAction(const std::string& doAction) {
    if (doAction == "mission_success") {
        m_outcome.state = MissionState::Complete;
        return true;
    }
    if (doAction == "mission_failure") {
        m_outcome.state = MissionState::Failed;
        return true;
    }
    // Every other action (spawn / message / weather / music ...) goes through the injected dispatcher,
    // which fl-server points at the SAME validated command path as the admin console. No dispatcher =
    // logged-and-skipped by the caller; the evaluator never mutates the world itself.
    if (m_dispatch)
        m_dispatch(doAction);
    return false;
}

void MissionRuntime::step(uint64_t tickIndex) {
    if (!m_started) {
        m_started = true;
        m_startTick = tickIndex;
        m_lastEvalTick = tickIndex;
    }
    if (done())
        return;

    // Second-scale cadence: evaluate on the first tick, then once per evalIntervalTicks.
    const bool first = tickIndex == m_startTick;
    if (!first && (tickIndex - m_lastEvalTick) < m_evalIntervalTicks)
        return;
    m_lastEvalTick = tickIndex;

    m_outcome.elapsedSeconds = static_cast<double>(tickIndex - m_startTick) * m_simDt;

    for (std::size_t i = 0; i < m_triggers.size(); ++i) {
        if (m_fired[i])
            continue;
        if (!evaluatePredicate(m_triggers[i].on))
            continue;
        m_fired[i] = true;
        ++m_outcome.triggersFired;
        const bool terminal = executeAction(m_triggers[i].doAction);
        if (terminal) {
            if (m_onEnd)
                m_onEnd(m_outcome);
            return; // the mission has ended; stop evaluating the rest
        }
    }
}

} // namespace fl
