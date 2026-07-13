// SPDX-License-Identifier: GPL-3.0-or-later
#include "ai/FormationController.h"

#include "ai/Guidance.h"
#include "entity/EntityManager.h"
#include "entity/EntityState.h"
#include "flight/LocalFrame.h"

#include <algorithm>
#include <cmath>

namespace fl::ai {

glm::vec3 formationSlotOffset(uint32_t slotIndex, const FormationParams& params) noexcept {
    // Rank 1 is the first pair of members, rank 2 the second, and so on — so the geometry is defined
    // for every slot index rather than for a fixed set of named positions. Even slots go right, odd
    // slots go left.
    const uint32_t rankIndex = slotIndex / 2u + 1u; // integer by intent: slots 0,1 -> rank 1; 2,3 -> rank 2; ...
    const auto rank = static_cast<float>(rankIndex);
    const float side = (slotIndex % 2u == 0u) ? 1.f : -1.f;
    return glm::vec3(params.lateralM * rank * side, // right
                     -params.aftM * rank,           // forward (negative = astern of the lead)
                     params.verticalM * rank);      // up (negative verticalM = stepped down)
}

FormationController::FormationController(const fl::EntityManager& entityManager, fl::EntityId leadId,
                                         uint32_t slotIndex, FormationParams params)
    : m_entityManager(entityManager), m_leadId(leadId), m_slotIndex(slotIndex), m_params(params) {}

glm::dvec3 FormationController::slotPoint(const fl::EntityState& lead) const noexcept {
    const glm::dvec3 leadPos(lead.transform.pos[0], lead.transform.pos[1], lead.transform.pos[2]);

    // Build the slot basis from the LEAD's heading and the LOCAL up at the lead's position — not
    // world axes, which are only "up" and "level" near the world origin.
    const glm::vec3 fwd = bodyForward(lead.transform.quat);
    const glm::vec3 up = fl::radialUp(leadPos, m_planetRadiusM);

    // Horizontal component of the lead's forward axis. A lead pointing straight up or down leaves no
    // heading to key the slot off, so fall back to a deterministic perpendicular rather than
    // producing a NaN basis.
    glm::vec3 fwdH = fwd - up * glm::dot(fwd, up);
    const float fwdHLen = glm::length(fwdH);
    if (fwdHLen > 1e-4f) {
        fwdH /= fwdHLen;
    } else {
        const glm::vec3 anyPerp = std::abs(up.x) < 0.9f ? glm::vec3(1.f, 0.f, 0.f) : glm::vec3(0.f, 0.f, 1.f);
        fwdH = glm::normalize(glm::cross(up, anyPerp));
    }
    const glm::vec3 right = glm::normalize(glm::cross(fwdH, up)); // fwd x up = right (Y-up, right-handed)

    const glm::vec3 off = formationSlotOffset(m_slotIndex, m_params);
    const glm::dvec3 offset = glm::dvec3(right) * static_cast<double>(off.x) +
                              glm::dvec3(fwdH) * static_cast<double>(off.y) +
                              glm::dvec3(up) * static_cast<double>(off.z);
    return leadPos + offset;
}

fl::ControlInput FormationController::sample(const fl::EntityState& state, uint64_t /*tick*/, double /*dt*/,
                                             const fl::AiTickContext& /*ctx*/) {
    fl::ControlInput ctrl{};

    const fl::EntityState* lead = m_entityManager.get(m_leadId);
    if (!lead || lead->dead) {
        return ctrl; // lead gone: coast, do not fly at a ghost
    }

    const glm::dvec3 ownPos(state.transform.pos[0], state.transform.pos[1], state.transform.pos[2]);
    const glm::dvec3 slot = slotPoint(*lead);
    const double slotArr[3] = {slot.x, slot.y, slot.z};

    // Steer at the slot exactly as PursuitController steers at a target — the difference is not the
    // steering, it is the throttle (below) and the fact that the aim point moves with the lead.
    const float headErr = horizontalHeadingError(state.transform.quat, state.transform.pos, slotArr, m_planetRadiusM);
    const float altErr =
        static_cast<float>(fl::localAltitude(slot, m_planetRadiusM) - fl::localAltitude(ownPos, m_planetRadiusM));
    const float pitchErr = pitchErrorFromAlt(state.transform.quat, state.transform.pos, altErr, m_planetRadiusM);

    ctrl.aileron = bankToTurnAileron(headErr);
    ctrl.rudder = coordinatedRudder(ctrl.aileron);
    ctrl.elevator = elevatorFromPitchError(pitchErr);

    // --- Closed-loop throttle: hold station, don't just chase. ---
    // Along-track error: how far the slot lies AHEAD of us along our own forward axis. Positive = we
    // are astern of the slot and must accelerate; negative = we have overrun it and must back off.
    const glm::dvec3 toSlot = slot - ownPos;
    const glm::vec3 ownFwd = bodyForward(state.transform.quat);
    const float alongTrackM = static_cast<float>(glm::dot(toSlot, glm::dvec3(ownFwd)));

    // Closure rate: how fast the gap is already closing under current velocity. Without this term the
    // controller commands full throttle right up to the slot and sails straight through it.
    const glm::vec3 ownVel(state.transform.vel[0], state.transform.vel[1], state.transform.vel[2]);
    const glm::vec3 leadVel(lead->transform.vel[0], lead->transform.vel[1], lead->transform.vel[2]);
    const glm::vec3 relVel = ownVel - leadVel;
    const float gapLen = static_cast<float>(glm::length(toSlot));
    const float closureMps = gapLen > 1e-3f ? glm::dot(relVel, glm::vec3(toSlot) / gapLen) : 0.f;

    const float throttle = m_params.throttleBase + m_params.rangeGain * alongTrackM - m_params.closureGain * closureMps;
    ctrl.throttle = std::clamp(throttle, m_params.minThrottle, m_params.maxThrottle);

    // A rejoin from far out is a stern chase, not station-keeping — light the burner to make it.
    ctrl.afterburner = gapLen > m_params.afterburnerErrorM;

    return ctrl;
}

} // namespace fl::ai
