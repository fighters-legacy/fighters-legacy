// SPDX-License-Identifier: GPL-3.0-or-later
#include "ai/DynamicLoiterController.h"

#include "ai/Guidance.h"
#include "entity/EntityState.h"

namespace fl::ai {

DynamicLoiterController::DynamicLoiterController(const fl::EntityManager& entityManager, fl::EntityId targetId,
                                                 float radiusM, float throttle, LoiterDir dir)
    : m_entityManager(entityManager), m_targetId(targetId), m_radiusM(radiusM), m_throttle(throttle),
      m_targetSpeedMps(orbitSpeedForRadius(radiusM)), m_dir(dir) {}

fl::ControlInput DynamicLoiterController::sample(const fl::EntityState& state, uint64_t /*tick*/, double dt,
                                                 const fl::AiTickContext& ctx) {
    // Terrain does not negotiate (#1352). A nav-role floor: this controller is holding a
    // commanded altitude, so the deck is set low enough that a deliberately low-level route or
    // orbit is still flown rather than fought.
    fl::ControlInput deck{};
    if (terrainFloorRecovery(deck, state.transform.quat, state.transform.pos, state.transform.vel, ctx, kNavDeckAglM,
                             m_planetRadiusM))
        return deck;

    const fl::EntityState* target = m_entityManager.get(m_targetId);
    if (!target || target->dead)
        return fl::ControlInput{}; // neutral: nothing to escort (same contract as PursuitController)

    // The orbit centre follows the target's LIVE position -- this is the whole point vs
    // LoiterController -- and the escort holds the covered asset's own altitude. Those two are the
    // ONLY differences; the geometry and the three loops below them are orbitSteer's (#1265).
    const glm::dvec3 center(target->transform.pos[0], target->transform.pos[1], target->transform.pos[2]);
    const OrbitParams orbit{
        center,           m_radiusM,  static_cast<float>(fl::localAltitude(center, m_planetRadiusM)),
        m_targetSpeedMps, m_throttle, m_dir};

    fl::ControlInput ctrl{};
    const float curPitch = fl::pitchOf(
        state.transform.quat, glm::dvec3(state.transform.pos[0], state.transform.pos[1], state.transform.pos[2]),
        m_planetRadiusM);
    orbitSteer(ctrl, state.transform.quat, state.transform.pos, state.transform.vel, orbit, m_planetRadiusM,
               m_pitchRate.step(curPitch, dt));
    return ctrl;
}

} // namespace fl::ai
