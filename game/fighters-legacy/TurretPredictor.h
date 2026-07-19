// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "weapon/Turret.h" // TurretState / TurretLimits / stepTurret / turretAimToAzEl (the SHARED servo)

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <cmath>

namespace fl {

// ── Client turret pose predictor (#966/#979) ─────────────────────────────────
//
// A human gunner's own turret is server-authoritative (slew rate + arc), but at RTT the replicated
// pose (the SnapshotCrew TLV) is always a few ticks stale, so the reticle would rubber-band. This
// predicts the turret locally by running the SAME pure servo the server runs — stepTurret, verbatim —
// toward the gunner's commanded aim each frame, exactly as ClientPrediction reuses FlightIntegrator for
// the airframe. Because a turret is a rate-limited scalar (az, el) with no accumulating integration
// error, reconciliation is a trivial blend toward the server pose: the prediction can drift at most one
// RTT of slew and is pulled back smoothly, with no snapping under normal RTT.
//
// This owns NO wire format — it is fed the servo limits (from the aircraft's turret def), the commanded
// aim, and the replicated server pose. Pure and headless-testable.
class TurretPredictor {
  public:
    // Set the turret's traverse envelope + slew rate (radians), from the aircraft's TurretDef. Call once
    // when the gunner takes the seat (or when the aircraft/turret is known).
    void configure(const TurretLimits& limits) noexcept {
        m_limits = limits;
    }

    // Seed the predicted pose (e.g. to the current server pose when the gunner first mans the station).
    void seed(float azRad, float elRad) noexcept {
        m_state.azRad = azRad;
        m_state.elRad = elRad;
        m_state.cmdAzRad = azRad;
        m_state.cmdElRad = elRad;
        m_seeded = true;
    }

    // Predict one frame: command the servo toward the gunner's aim (mount-frame az/el) and slew toward
    // it at the servo rate. dt is the client frame time. A local, immediate response to the gunner's aim.
    void predict(float cmdAzRad, float cmdElRad, float dt) noexcept {
        m_state.cmdAzRad = std::clamp(cmdAzRad, m_limits.azMinRad, m_limits.azMaxRad);
        m_state.cmdElRad = std::clamp(cmdElRad, m_limits.elMinRad, m_limits.elMaxRad);
        stepTurret(m_state, m_limits, dt);
    }

    // Command from a WORLD-space aim direction (the gunner's camera look) + the airframe + mount pose.
    void predictWorld(const glm::vec3& aimDirWorld, const glm::quat& mountRest, const glm::quat& airframeWorldQuat,
                      float dt) noexcept {
        // world -> body -> mount, then decompose to az/el (mirrors commandTurretWorld's front half).
        const glm::vec3 mountDir = glm::conjugate(mountRest) * (glm::conjugate(airframeWorldQuat) * aimDirWorld);
        float az = 0.f, el = 0.f;
        turretAimToAzEl(mountDir, az, el);
        predict(az, el, dt);
    }

    // Reconcile toward the server's replicated pose (SnapshotCrew). blend in [0,1]; small values keep the
    // response local while preventing drift. A large divergence (e.g. after a stall / packet gap) snaps.
    void reconcile(float serverAzRad, float serverElRad, float blend = 0.2f, float snapRad = 0.35f) noexcept {
        if (!m_seeded) {
            seed(serverAzRad, serverElRad);
            return;
        }
        const float dAz = serverAzRad - m_state.azRad;
        const float dEl = serverElRad - m_state.elRad;
        if (std::abs(dAz) > snapRad || std::abs(dEl) > snapRad) {
            m_state.azRad = serverAzRad;
            m_state.elRad = serverElRad;
        } else {
            m_state.azRad += dAz * blend;
            m_state.elRad += dEl * blend;
        }
    }

    [[nodiscard]] float azRad() const noexcept {
        return m_state.azRad;
    }
    [[nodiscard]] float elRad() const noexcept {
        return m_state.elRad;
    }
    [[nodiscard]] bool seeded() const noexcept {
        return m_seeded;
    }

    // The predicted world bore (for the reticle), given the airframe + mount pose.
    [[nodiscard]] glm::vec3 boreWorld(const glm::quat& mountRest, const glm::quat& airframeWorldQuat) const noexcept {
        return turretWorldDir(m_state, mountRest, airframeWorldQuat);
    }

  private:
    TurretState m_state{};
    TurretLimits m_limits{};
    bool m_seeded{false};
};

} // namespace fl
