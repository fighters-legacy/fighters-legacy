// SPDX-License-Identifier: GPL-3.0-or-later
#include "ai/SurfaceThreatControllers.h"

#include "ai/Guidance.h"
#include "ai/TargetView.h"
#include "ai/Threat.h"
#include "flight/BallisticLead.h"
#include "flight/LocalFrame.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace fl::ai {

namespace {
constexpr float degToRad(float d) noexcept {
    return d * std::numbers::pi_v<float> / 180.f;
}
} // namespace

SamEngagementController::SamEngagementController(const fl::EntityManager& entityManager, float engageRangeM,
                                                 float coneHalfAngleDeg, float fireIntervalS,
                                                 float launchElevationMinDeg)
    : m_entityManager(entityManager), m_engageRangeM(engageRangeM), m_coneHalfRad(degToRad(coneHalfAngleDeg)),
      m_fireIntervalTicks(static_cast<uint64_t>(std::max(1.f, fireIntervalS * 60.f))),
      m_launchElevationMinRad(degToRad(std::clamp(launchElevationMinDeg, 0.f, 89.f))) {}

fl::ControlInput SamEngagementController::sample(const fl::EntityState& state, uint64_t tick, double /*dt*/,
                                                 const fl::AiTickContext& ctx) {
    fl::ControlInput ctrl{}; // an emplacement: no throttle, no steering — only the trigger moves

    // Honest sensing (#863): engage only what this battery has actually detected. A null table means
    // sensing was not evaluated (no engagement); a zero look axis makes designateFromContacts use the
    // launcher's body-forward, so it fires into the arc the missile can service.
    if (!ctx.contacts)
        return ctrl;
    // The look axis is the emplacement's own facing (designateFromContacts needs a real axis — it does
    // NOT fall back to body-forward the way designateBoresightTarget does).
    const glm::vec3 nose = bodyForward(state.transform.quat);
    const float axis[3] = {nose.x, nose.y, nose.z};
    const fl::EntityId target =
        designateFromContacts(state, axis, ctx.contacts, m_engageRangeM, m_coneHalfRad, /*factions=*/nullptr);
    if (!target.valid())
        return ctrl;

    // A target is in the envelope: launch on the reload interval. release is edge-detected by
    // FireControl, so one pulse = one missile; the interval is longer than the launcher cooldown.
    if (!m_hasFired || (tick - m_lastFireTick) >= m_fireIntervalTicks) {
        ctrl.release = true;
        m_lastFireTick = tick;
        m_hasFired = true;
        setLaunchVector(ctrl, state, ctx, target);
    }
    return ctrl;
}

// Point the launcher at the designated contact, but never below `m_launchElevationMinRad` above the
// local horizon (#1204). An emplacement's airframe nose is horizontal, and a store that leaves along
// it starts at deck level: the projectile ground check ends it within a few steps unless the line of
// sight happens to be steep. The elevation floor is what a rail launcher does before it fires, and it
// buys the missile the clearance it needs before proportional navigation pulls it onto the LOS.
void SamEngagementController::setLaunchVector(fl::ControlInput& ctrl, const fl::EntityState& state,
                                              const fl::AiTickContext& ctx, fl::EntityId target) const {
    // The contact's last-known state, never ground truth — the battery aims where it BELIEVES the
    // target is, exactly as it decided to shoot on that belief.
    const TargetView tv = resolveTarget(m_entityManager, ctx, target);
    const glm::dvec3 ownPos{state.transform.pos[0], state.transform.pos[1], state.transform.pos[2]};
    const glm::vec3 up = fl::radialUp(ownPos, m_planetRadiusM);

    // Without a resolvable contact, fall back to the emplacement's own facing rather than to nothing:
    // a launcher that cannot resolve its target still elevates, which is the whole point.
    glm::vec3 want = bodyForward(state.transform.quat);
    if (tv.valid) {
        const glm::dvec3 tgtPos{tv.pos[0], tv.pos[1], tv.pos[2]};
        const glm::dvec3 toTarget = tgtPos - ownPos;
        if (glm::dot(toTarget, toTarget) > 1.0)
            want = glm::normalize(glm::vec3(toTarget));
    }

    // Decompose into the local horizontal and vertical components, then re-tilt to the floor when
    // the target sits at or below it. A target already above the floor is aimed at directly.
    const float sinEl = glm::clamp(glm::dot(want, up), -1.f, 1.f);
    if (sinEl < std::sin(m_launchElevationMinRad)) {
        glm::vec3 horiz = want - up * sinEl;
        const float hlen = glm::length(horiz);
        // Straight down (or straight up) leaves no horizontal reference: launch along the local
        // vertical, which is the vertical-launch case and is never worse than firing into the dirt.
        horiz = hlen > 1e-4f ? horiz / hlen : glm::vec3{0.f};
        want = glm::normalize(horiz * std::cos(m_launchElevationMinRad) + up * std::sin(m_launchElevationMinRad));
    }

    ctrl.hasAimDir = true;
    ctrl.aimDir[0] = want.x;
    ctrl.aimDir[1] = want.y;
    ctrl.aimDir[2] = want.z;
}

AaaFireController::AaaFireController(const fl::EntityManager& entityManager, float engageRangeM, float coneHalfAngleDeg,
                                     float muzzleVelMps, float lethalRadiusM)
    : m_entityManager(entityManager), m_engageRangeM(engageRangeM), m_coneHalfRad(degToRad(coneHalfAngleDeg)),
      m_muzzleVelMps(muzzleVelMps), m_lethalRadiusM(lethalRadiusM) {}

fl::ControlInput AaaFireController::sample(const fl::EntityState& state, uint64_t /*tick*/, double /*dt*/,
                                           const fl::AiTickContext& ctx) {
    fl::ControlInput ctrl{}; // fixed emplacement: only the trigger

    if (!ctx.contacts)
        return ctrl;
    // The look axis is the emplacement's own facing (designateFromContacts needs a real axis — it does
    // NOT fall back to body-forward the way designateBoresightTarget does).
    const glm::vec3 nose = bodyForward(state.transform.quat);
    const float axis[3] = {nose.x, nose.y, nose.z};
    const fl::EntityId target =
        designateFromContacts(state, axis, ctx.contacts, m_engageRangeM, m_coneHalfRad, /*factions=*/nullptr);
    if (!target.valid())
        return ctrl;

    // Lead the CONTACT's last-known state (not ground truth) — an AAA crew can only lead what it sees.
    const TargetView tv = resolveTarget(m_entityManager, ctx, target);
    if (!tv.valid)
        return ctrl;

    const glm::dvec3 ownPos{state.transform.pos[0], state.transform.pos[1], state.transform.pos[2]};
    const glm::vec3 ownVel{state.transform.vel[0], state.transform.vel[1], state.transform.vel[2]};
    const glm::dvec3 tgtPos{tv.pos[0], tv.pos[1], tv.pos[2]};
    const glm::vec3 tgtVel{tv.vel[0], tv.vel[1], tv.vel[2]};

    const glm::vec3 gravity = -fl::radialUp(ownPos, m_planetRadiusM) * 9.80665f;
    const BallisticLeadResult lead = computeBallisticLead(ownPos, ownVel, tgtPos, tgtVel, m_muzzleVelMps, gravity);
    if (!lead.valid)
        return ctrl;

    // Trigger discipline (same rule as GunsEmploymentController): fire only when the predicted miss at
    // the target's range is inside the lethal radius and the target is within reach.
    const float rangeM = static_cast<float>(glm::length(tgtPos - ownPos));
    if (rangeM > 1.f && rangeM <= m_engageRangeM) {
        const glm::vec3 want = glm::normalize(glm::vec3(lead.aimPoint - ownPos));
        const float cosOff = std::min(1.f, std::max(-1.f, glm::dot(nose, want)));
        const float missM = std::acos(cosOff) * rangeM; // small-angle arc at the target's range
        ctrl.trigger = missM <= m_lethalRadiusM;
    }
    return ctrl;
}

} // namespace fl::ai
