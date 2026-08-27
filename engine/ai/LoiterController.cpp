// SPDX-License-Identifier: GPL-3.0-or-later
#include "ai/LoiterController.h"

#include "ai/Guidance.h"
#include "entity/EntityState.h"

namespace fl::ai {

LoiterController::LoiterController(glm::dvec3 center, float radiusM, float altitudeM, float throttle, LoiterDir dir)
    : m_center(center), m_radiusM(radiusM), m_altitudeM(altitudeM), m_throttle(throttle),
      m_targetSpeedMps(orbitSpeedForRadius(radiusM)), m_dir(dir) {}

fl::ControlInput LoiterController::sample(const fl::EntityState& state, uint64_t /*tick*/, double dt,
                                          const fl::AiTickContext& ctx) {
    fl::ControlInput ctrl{};

    // Stepped BEFORE the deck check, so the estimator keeps its continuity on the ticks the recovery
    // takes over -- a backward difference that skips samples reports a rate that never happened.
    const float curPitch = fl::pitchOf(
        state.transform.quat, glm::dvec3(state.transform.pos[0], state.transform.pos[1], state.transform.pos[2]),
        m_planetRadiusM);
    const float pitchRate = m_pitchRate.step(curPitch, dt);

    // Terrain does not negotiate (#1352). The deck is checked FIRST and outranks whatever
    // geometry this controller was about to fly; below it the only job is to still be
    // airborne next tick.
    if (terrainFloorRecovery(ctrl, state.transform.quat, state.transform.pos, state.transform.vel, ctx, kNavDeckAglM,
                             m_planetRadiusM, pitchRate))
        return ctrl;

    const OrbitParams orbit{m_center, m_radiusM, m_altitudeM, m_targetSpeedMps, m_throttle, m_dir};
    orbitSteer(ctrl, state.transform.quat, state.transform.pos, state.transform.vel, orbit, m_planetRadiusM, pitchRate);
    return ctrl;
}

} // namespace fl::ai
