// SPDX-License-Identifier: GPL-3.0-or-later
#include "ai/SwarmController.h"

#include "ai/Guidance.h"
#include "entity/AiTickContext.h"
#include "entity/EntityState.h"
#include "flight/LocalFrame.h"
#include "spatial/SpatialIndex.h"

#include <algorithm>
#include <cmath>

namespace fl::ai {

SwarmController::SwarmController(const fl::EntityManager& entityManager, const glm::dvec3& migrationPoint,
                                 SwarmParams params)
    : m_entityManager(entityManager), m_migrationPoint(migrationPoint), m_hasPoint(true), m_params(params) {}

SwarmController::SwarmController(const fl::EntityManager& entityManager, fl::EntityId anchor, SwarmParams params)
    : m_entityManager(entityManager), m_anchor(anchor), m_params(params) {}

fl::ControlInput SwarmController::sample(const fl::EntityState& state, uint64_t /*tick*/, double /*dt*/,
                                         const fl::AiTickContext& ctx) {
    const glm::dvec3 ownPos(state.transform.pos[0], state.transform.pos[1], state.transform.pos[2]);
    const glm::vec3 ownVel(state.transform.vel[0], state.transform.vel[1], state.transform.vel[2]);

    // ── Migration goal: a moving anchor entity, or a fixed point. ─────────────────────────────
    bool hasGoal = false;
    glm::dvec3 goal{};
    if (m_anchor.valid()) {
        if (const fl::EntityState* anchor = m_entityManager.get(m_anchor); anchor && !anchor->dead) {
            goal = glm::dvec3(anchor->transform.pos[0], anchor->transform.pos[1], anchor->transform.pos[2]);
            hasGoal = true;
        }
    } else if (m_hasPoint) {
        goal = m_migrationPoint;
        hasGoal = true;
    }

    // ── Boids accumulation over flockmates (same type + faction, alive, not self). ────────────
    const double radiusSq = static_cast<double>(m_params.neighborRadiusM) * m_params.neighborRadiusM;
    glm::vec3 separation(0.f);
    glm::vec3 flockVel(0.f);
    glm::dvec3 flockCentre(0.0);
    uint32_t neighbors = 0;

    auto consider = [&](const fl::EntityState& other) {
        if (neighbors >= m_params.maxNeighbors)
            return;
        if (other.dead || other.id.index == state.id.index)
            return;
        if (other.typeIndex != state.typeIndex || other.factionIndex != state.factionIndex)
            return;
        const glm::dvec3 otherPos(other.transform.pos[0], other.transform.pos[1], other.transform.pos[2]);
        const glm::dvec3 delta = ownPos - otherPos;
        const double distSq = glm::dot(delta, delta);
        if (distSq > radiusSq || distSq < 1e-6)
            return; // outside the exact 3D radius (the cell query is conservative), or coincident
        ++neighbors;
        flockCentre += otherPos;
        flockVel += glm::vec3(other.transform.vel[0], other.transform.vel[1], other.transform.vel[2]);
        const float dist = static_cast<float>(std::sqrt(distSq));
        if (dist < m_params.separationRadiusM) {
            // Inverse-distance repulsion: the closer the flockmate, the harder the push.
            separation += glm::vec3(delta) * (1.f / std::max(dist * dist, 1.f));
        }
    };

    if (ctx.si) {
        ctx.si->queryRadius(state.transform.pos, static_cast<double>(m_params.neighborRadiusM),
                            [&](uint32_t idx, const double* /*pos*/) {
                                if (const fl::EntityState* other = m_entityManager.getByIndex(idx))
                                    consider(*other);
                            });
    } else {
        // No spatial index in this context (tests, headless harnesses): correct, just not cheap.
        m_entityManager.forEach([&](const fl::EntityState& other) { consider(other); });
    }

    // ── Compose the desired direction. ────────────────────────────────────────────────────────
    glm::vec3 desired(0.f);
    if (neighbors > 0) {
        flockCentre /= static_cast<double>(neighbors);
        const glm::vec3 toCentre(flockCentre - ownPos);
        if (glm::dot(toCentre, toCentre) > 1e-6f)
            desired += m_params.cohesionWeight * glm::normalize(toCentre);
        if (glm::dot(flockVel, flockVel) > 1e-6f)
            desired += m_params.alignmentWeight * glm::normalize(flockVel);
        if (glm::dot(separation, separation) > 1e-12f)
            desired += m_params.separationWeight * glm::normalize(separation);
    }
    if (hasGoal) {
        const glm::vec3 toGoal(goal - ownPos);
        if (glm::dot(toGoal, toGoal) > 1e-6f)
            desired += m_params.migrationWeight * glm::normalize(toGoal);
    }

    fl::ControlInput ctrl{};
    ctrl.throttle = m_params.cruiseThrottle;
    if (glm::dot(desired, desired) < 1e-8f)
        return ctrl; // alone with no goal: hold heading at cruise power

    // ── Steer at a lookahead point along the desired direction (the shared bank-to-turn path). ─
    const glm::dvec3 aim = ownPos + glm::dvec3(glm::normalize(desired)) * static_cast<double>(m_params.lookaheadM);
    const double aimArr[3] = {aim.x, aim.y, aim.z};
    const float headErr = horizontalHeadingError(state.transform.quat, state.transform.pos, aimArr, m_planetRadiusM);
    const float altErr =
        static_cast<float>(fl::localAltitude(aim, m_planetRadiusM) - fl::localAltitude(ownPos, m_planetRadiusM));
    const float pitchErr = pitchErrorFromAlt(state.transform.quat, state.transform.pos, altErr, m_planetRadiusM);

    ctrl.aileron = bankToTurnAileron(headErr);
    ctrl.rudder = coordinatedRudder(ctrl.aileron);
    ctrl.elevator = elevatorFromPitchError(pitchErr);

    // Loose speed matching: a flock that cannot match speeds cannot hold together. Around the
    // cruise baseline, nudge toward the flock's mean speed.
    if (neighbors > 0) {
        const float ownSpd = glm::length(ownVel);
        const float flockSpd = glm::length(flockVel / static_cast<float>(neighbors));
        ctrl.throttle = std::clamp(m_params.cruiseThrottle + m_params.speedGain * (flockSpd - ownSpd), 0.2f, 1.f);
    }
    return ctrl;
}

} // namespace fl::ai
