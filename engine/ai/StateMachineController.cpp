// SPDX-License-Identifier: GPL-3.0-or-later
#include "ai/StateMachineController.h"

#include "entity/EntityManager.h"
#include "entity/EntityState.h"
#include "flight/LocalFrame.h" // localAltitude / radialUp for the flight-regime conditions (#701)
#include "sensor/SensorSystem.h"
#include "spatial/SpatialIndex.h"
#include "world/FactionRegistry.h" // hostile()/areFactionsHostile — coalition-aware via ctx.factions (#632)

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
    if (m_active) {
        // Forward the local-level planet radius to the freshly-constructed child so its guidance
        // math is correct far from the world origin (children are recreated on every state entry).
        m_active->setPlanetRadius(m_planetRadiusM);
    }
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
                                                const fl::AiTickContext& ctx) {
    if (m_activeIdx < 0 || !m_active) {
        return {};
    }

    m_dwellTime += static_cast<float>(dt);

    fl::ControlInput inp = m_active->sample(state, tick, dt, ctx);

    for (const Transition& tr : m_states[m_activeIdx].transitions) {
        if (m_dwellTime < tr.minDwellSeconds) {
            continue;
        }
        if (!tr.cond(state, m_entityManager, ctx)) {
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
        [targetId, rangeM](const fl::EntityState& self, const fl::EntityManager& em, const fl::AiTickContext&) -> bool {
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
        [targetId, rangeM](const fl::EntityState& self, const fl::EntityManager& em, const fl::AiTickContext&) -> bool {
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
    return [fraction](const fl::EntityState& self, const fl::EntityManager&, const fl::AiTickContext&) -> bool {
        if (self.maxHp <= 0.f) {
            return false;
        }
        return (self.hp / self.maxHp) < fraction;
    };
}

Condition AnyEntityWithinRange(float rangeM) {
    return [rangeM](const fl::EntityState& self, const fl::EntityManager&, const fl::AiTickContext& ctx) -> bool {
        if (!ctx.si) {
            return false;
        }
        bool found = false;
        const double rangeSq = static_cast<double>(rangeM) * static_cast<double>(rangeM);
        // queryRadius is conservative (cell-level): exact distance check required.
        ctx.si->queryRadius(self.transform.pos, static_cast<double>(rangeM), [&](uint32_t idx, const double* pos) {
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

Condition AnyHostileEntityWithinRange(float rangeM) {
    return [rangeM](const fl::EntityState& self, const fl::EntityManager& em, const fl::AiTickContext& ctx) -> bool {
        if (!ctx.si || self.factionIndex == 0) {
            return false; // no spatial index, or a neutral entity has no enemies
        }
        bool found = false;
        const double rangeSq = static_cast<double>(rangeM) * static_cast<double>(rangeM);
        // queryRadius is conservative (cell-level): exact distance + faction check required.
        ctx.si->queryRadius(self.transform.pos, static_cast<double>(rangeM), [&](uint32_t idx, const double* pos) {
            if (found || idx == self.id.index) {
                return;
            }
            const fl::EntityState* other = em.getByIndex(idx);
            if (!other || !fl::hostile(ctx.factions, self.factionIndex, other->factionIndex)) {
                return;
            }
            const double dx = pos[0] - self.transform.pos[0];
            const double dy = pos[1] - self.transform.pos[1];
            const double dz = pos[2] - self.transform.pos[2];
            if (dx * dx + dy * dy + dz * dz <= rangeSq) {
                found = true;
            }
        });
        return found;
    };
}

Condition AnyHostileEntityWithinRangeOf(fl::EntityId anchorId, float rangeM) {
    return [anchorId, rangeM](const fl::EntityState& self, const fl::EntityManager& em,
                              const fl::AiTickContext& ctx) -> bool {
        if (!ctx.si || self.factionIndex == 0) {
            return false; // no spatial index, or a neutral entity has no enemies
        }
        const fl::EntityState* anchor = em.get(anchorId);
        if (!anchor || anchor->dead) {
            return false; // nothing left to protect
        }

        bool found = false;
        const double rangeSq = static_cast<double>(rangeM) * static_cast<double>(rangeM);
        // Geometry about the ANCHOR (the entity being protected); hostility about SELF (the escort).
        // queryRadius is conservative (cell-level), so the exact distance test is still required.
        ctx.si->queryRadius(anchor->transform.pos, static_cast<double>(rangeM), [&](uint32_t idx, const double* pos) {
            if (found || idx == self.id.index || idx == anchor->id.index) {
                return;
            }
            const fl::EntityState* other = em.getByIndex(idx);
            if (!other || other->dead || !fl::hostile(ctx.factions, self.factionIndex, other->factionIndex)) {
                return;
            }
            const double dx = pos[0] - anchor->transform.pos[0];
            const double dy = pos[1] - anchor->transform.pos[1];
            const double dz = pos[2] - anchor->transform.pos[2];
            if (dx * dx + dy * dy + dz * dz <= rangeSq) {
                found = true;
            }
        });
        return found;
    };
}

// ── Sensing-gated conditions (#690) ─────────────────────────────────────────

namespace {

// Distance from self to a contact's LAST-KNOWN position. Not to where the target is — to where this
// entity last saw it, which is all it is entitled to know.
[[nodiscard]] double distSqToContact(const fl::EntityState& self, const fl::sensor::Contact& c) noexcept {
    const double dx = c.lastKnownPos[0] - self.transform.pos[0];
    const double dy = c.lastKnownPos[1] - self.transform.pos[1];
    const double dz = c.lastKnownPos[2] - self.transform.pos[2];
    return dx * dx + dy * dy + dz * dz;
}

} // namespace

Condition DetectedHostileWithinRange(float rangeM) {
    auto groundTruth = AnyHostileEntityWithinRange(rangeM);
    return [rangeM, groundTruth](const fl::EntityState& self, const fl::EntityManager& em,
                                 const fl::AiTickContext& ctx) -> bool {
        if (!ctx.contacts)
            return groundTruth(self, em, ctx); // sensing not evaluated: the pre-#690 behavior
        if (self.factionIndex == 0)
            return false; // a neutral entity has no enemies

        const double rangeSq = static_cast<double>(rangeM) * static_cast<double>(rangeM);
        for (const fl::sensor::Contact& c : *ctx.contacts) {
            if (!c.reacted)
                continue; // seen, but not yet noticed — the reaction delay is not a formality
            if (!fl::hostile(ctx.factions, self.factionIndex, c.factionIndex))
                continue;
            if (distSqToContact(self, c) <= rangeSq)
                return true;
        }
        return false;
    };
}

Condition DetectsThreatWithinRange(fl::EntityId targetId, float rangeM) {
    auto groundTruth = ThreatWithinRange(targetId, rangeM);
    return [targetId, rangeM, groundTruth](const fl::EntityState& self, const fl::EntityManager& em,
                                           const fl::AiTickContext& ctx) -> bool {
        if (!ctx.contacts)
            return groundTruth(self, em, ctx);

        const fl::sensor::Contact* c = ctx.contacts->find(targetId);
        if (!c || !c->held() || !c->reacted)
            return false;

        const double rangeSq = static_cast<double>(rangeM) * static_cast<double>(rangeM);
        return distSqToContact(self, *c) <= rangeSq;
    };
}

Condition LostContact(fl::EntityId targetId, float rangeM) {
    auto groundTruth = ThreatBeyondRange(targetId, rangeM);
    return [targetId, rangeM, groundTruth](const fl::EntityState& self, const fl::EntityManager& em,
                                           const fl::AiTickContext& ctx) -> bool {
        if (!ctx.contacts)
            return groundTruth(self, em, ctx);

        const fl::sensor::Contact* c = ctx.contacts->find(targetId);
        if (!c || !c->held())
            return true; // never found it, or the coast ran out — he is gone

        const double rangeSq = static_cast<double>(rangeM) * static_cast<double>(rangeM);
        return distSqToContact(self, *c) > rangeSq;
    };
}

Condition HasLockedContact() {
    return [](const fl::EntityState&, const fl::EntityManager&, const fl::AiTickContext& ctx) -> bool {
        if (!ctx.contacts)
            return false; // no sensing ⇒ no lock. A controller cannot claim a track it never took.
        for (const fl::sensor::Contact& c : *ctx.contacts) {
            if (c.locked())
                return true;
        }
        return false;
    };
}

Condition NoContacts() {
    return [](const fl::EntityState&, const fl::EntityManager&, const fl::AiTickContext& ctx) -> bool {
        if (!ctx.contacts)
            return false; // an entity with no sensing must not conclude "all clear"
        return ctx.contacts->empty();
    };
}

Condition AboveAltitude(float altM) {
    return [altM](const fl::EntityState& self, const fl::EntityManager&, const fl::AiTickContext&) -> bool {
        const glm::dvec3 pos(self.transform.pos[0], self.transform.pos[1], self.transform.pos[2]);
        return fl::localAltitude(pos, fl::kEarthRadiusM) > static_cast<double>(altM);
    };
}

Condition GroundSpeedBelow(float speedMps) {
    return [speedMps](const fl::EntityState& self, const fl::EntityManager&, const fl::AiTickContext&) -> bool {
        const glm::dvec3 pos(self.transform.pos[0], self.transform.pos[1], self.transform.pos[2]);
        return fl::horizontalGroundSpeed(self.transform.vel, pos, fl::kEarthRadiusM) < speedMps;
    };
}

Condition Always() {
    return [](const fl::EntityState&, const fl::EntityManager&, const fl::AiTickContext&) -> bool { return true; };
}

Condition And(Condition a, Condition b) {
    return [a = std::move(a), b = std::move(b)](const fl::EntityState& self, const fl::EntityManager& em,
                                                const fl::AiTickContext& ctx) -> bool {
        return a(self, em, ctx) && b(self, em, ctx);
    };
}

Condition Or(Condition a, Condition b) {
    return [a = std::move(a), b = std::move(b)](const fl::EntityState& self, const fl::EntityManager& em,
                                                const fl::AiTickContext& ctx) -> bool {
        return a(self, em, ctx) || b(self, em, ctx);
    };
}

Condition Not(Condition a) {
    return [a = std::move(a)](const fl::EntityState& self, const fl::EntityManager& em,
                              const fl::AiTickContext& ctx) -> bool { return !a(self, em, ctx); };
}

} // namespace fl::ai
