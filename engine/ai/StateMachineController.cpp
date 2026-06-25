// SPDX-License-Identifier: GPL-3.0-or-later
#include "ai/StateMachineController.h"

#include "entity/EntityManager.h"
#include "entity/EntityState.h"
#include "spatial/SpatialIndex.h"

#include <cstdio>
#include <utility>

namespace fl::ai {

// ---------------------------------------------------------------------------
// StateMachineController
// ---------------------------------------------------------------------------

StateMachineController::StateMachineController(const fl::EntityManager& entityManager)
    : m_entityManager(entityManager) {}

int StateMachineController::findState(const std::string& name) const noexcept {
    for (int i = 0; i < static_cast<int>(m_states.size()); ++i) {
        if (m_states[i].name == name) {
            return i;
        }
    }
    return -1;
}

void StateMachineController::enterState(int idx) {
    m_activeIdx = idx;
    m_dwellTime = 0.f;
    m_active = (idx >= 0) ? m_states[idx].factory() : nullptr;
}

void StateMachineController::addState(std::string name, ControllerFactory factory) {
    if (findState(name) >= 0) {
        std::fprintf(stderr, "[AI WARN] StateMachineController::addState: duplicate state '%s' ignored\n",
                     name.c_str());
        return;
    }
    m_states.push_back({std::move(name), std::move(factory), {}});
}

void StateMachineController::addTransition(std::string from, std::string to, Condition cond, float minDwellSeconds) {
    int idx = findState(from);
    if (idx < 0) {
        std::fprintf(
            stderr,
            "[AI WARN] StateMachineController::addTransition: unknown state '%s' — transition to '%s' ignored\n",
            from.c_str(), to.c_str());
        return;
    }
    m_states[idx].transitions.push_back({std::move(to), std::move(cond), minDwellSeconds});
}

void StateMachineController::setInitialState(const std::string& name) {
    int idx = findState(name);
    if (idx < 0) {
        std::fprintf(stderr, "[AI WARN] StateMachineController::setInitialState: unknown state '%s'\n", name.c_str());
        return;
    }
    enterState(idx);
}

const std::string& StateMachineController::currentState() const noexcept {
    static const std::string kEmpty{};
    if (m_activeIdx < 0) {
        return kEmpty;
    }
    return m_states[m_activeIdx].name;
}

fl::ControlInput StateMachineController::sample(const fl::EntityState& state, uint64_t tick, double dt,
                                                const fl::SpatialIndex* si) {
    if (m_activeIdx < 0 || !m_active) {
        return {};
    }

    m_dwellTime += static_cast<float>(dt);

    fl::ControlInput inp = m_active->sample(state, tick, dt, si);

    for (const Transition& tr : m_states[m_activeIdx].transitions) {
        if (m_dwellTime < tr.minDwellSeconds) {
            continue;
        }
        if (!tr.cond(state, m_entityManager, si)) {
            continue;
        }
        int nextIdx = findState(tr.to);
        if (nextIdx < 0) {
            std::fprintf(stderr, "[AI WARN] StateMachineController::sample: transition target '%s' not found\n",
                         tr.to.c_str());
            break; // consumed; unknown target
        }
        if (nextIdx == m_activeIdx) {
            continue; // self-transition: no-op; let later transitions be tested
        }
        enterState(nextIdx);
        break;
    }

    return inp;
}

// ---------------------------------------------------------------------------
// Built-in Condition helpers
// ---------------------------------------------------------------------------

Condition ThreatWithinRange(fl::EntityId targetId, float rangeM) {
    return
        [targetId, rangeM](const fl::EntityState& self, const fl::EntityManager& em, const fl::SpatialIndex*) -> bool {
            const fl::EntityState* target = em.get(targetId);
            if (!target || target->dead) {
                return false;
            }
            double dx = target->transform.pos[0] - self.transform.pos[0];
            double dy = target->transform.pos[1] - self.transform.pos[1];
            double dz = target->transform.pos[2] - self.transform.pos[2];
            double distSq = dx * dx + dy * dy + dz * dz;
            return distSq <= static_cast<double>(rangeM) * static_cast<double>(rangeM);
        };
}

Condition ThreatBeyondRange(fl::EntityId targetId, float rangeM) {
    return
        [targetId, rangeM](const fl::EntityState& self, const fl::EntityManager& em, const fl::SpatialIndex*) -> bool {
            const fl::EntityState* target = em.get(targetId);
            if (!target || target->dead) {
                return true;
            }
            double dx = target->transform.pos[0] - self.transform.pos[0];
            double dy = target->transform.pos[1] - self.transform.pos[1];
            double dz = target->transform.pos[2] - self.transform.pos[2];
            double distSq = dx * dx + dy * dy + dz * dz;
            return distSq > static_cast<double>(rangeM) * static_cast<double>(rangeM);
        };
}

Condition HpBelow(float fraction) {
    return [fraction](const fl::EntityState& self, const fl::EntityManager&, const fl::SpatialIndex*) -> bool {
        if (self.maxHp <= 0.f) {
            return false;
        }
        return (self.hp / self.maxHp) < fraction;
    };
}

Condition AnyEntityWithinRange(float rangeM) {
    return [rangeM](const fl::EntityState& self, const fl::EntityManager&, const fl::SpatialIndex* si) -> bool {
        if (!si) {
            return false;
        }
        bool found = false;
        const double rangeSq = static_cast<double>(rangeM) * static_cast<double>(rangeM);
        // queryRadius is conservative (cell-level): exact distance check required.
        si->queryRadius(self.transform.pos, static_cast<double>(rangeM), [&](uint32_t idx, const double* pos) {
            if (idx == self.id.index) {
                return;
            }
            double dx = pos[0] - self.transform.pos[0];
            double dy = pos[1] - self.transform.pos[1];
            double dz = pos[2] - self.transform.pos[2];
            if (dx * dx + dy * dy + dz * dz <= rangeSq) {
                found = true;
            }
        });
        return found;
    };
}

Condition Always() {
    return [](const fl::EntityState&, const fl::EntityManager&, const fl::SpatialIndex*) -> bool { return true; };
}

Condition And(Condition a, Condition b) {
    return [a = std::move(a), b = std::move(b)](const fl::EntityState& self, const fl::EntityManager& em,
                                                const fl::SpatialIndex* si) -> bool {
        return a(self, em, si) && b(self, em, si);
    };
}

Condition Or(Condition a, Condition b) {
    return [a = std::move(a), b = std::move(b)](const fl::EntityState& self, const fl::EntityManager& em,
                                                const fl::SpatialIndex* si) -> bool {
        return a(self, em, si) || b(self, em, si);
    };
}

Condition Not(Condition a) {
    return [a = std::move(a)](const fl::EntityState& self, const fl::EntityManager& em,
                              const fl::SpatialIndex* si) -> bool { return !a(self, em, si); };
}

} // namespace fl::ai
