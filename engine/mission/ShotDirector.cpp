// SPDX-License-Identifier: GPL-3.0-or-later
#include "mission/ShotDirector.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace fl {

namespace {

constexpr float kFovMin = 20.f;
constexpr float kFovMax = 120.f;

// Build an approximate up vector for a look direction: world +Y, unless the look is near-vertical
// (looking straight up/down), where +Y degenerates — then fall back to a horizontal reference.
glm::vec3 upFor(const glm::vec3& fwd) {
    const glm::vec3 f = glm::length(fwd) > 1e-6f ? glm::normalize(fwd) : glm::vec3(0.f, 0.f, -1.f);
    if (std::fabs(f.y) > 0.999f)
        return glm::vec3(0.f, 0.f, -1.f);
    return glm::vec3(0.f, 1.f, 0.f);
}

// Catmull-Rom interpolation of one component across p1→p2 (u in [0,1]); p0/p3 are the neighbours.
double catmullRom(double p0, double p1, double p2, double p3, double u) {
    const double u2 = u * u;
    const double u3 = u2 * u;
    return 0.5 * ((2.0 * p1) + (-p0 + p2) * u + (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * u2 +
                  (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * u3);
}

} // namespace

ShotDirector::ShotDirector(std::vector<MissionShot> shots) : m_shots(std::move(shots)) {}

double ShotDirector::totalDurationSec() const {
    if (m_shots.empty())
        return 0.0;
    const MissionShot& last = m_shots.back();
    return last.startSec + last.durationSec;
}

// Resolve the eye/look/fov for one shot. Returns false when a required entity pose is unavailable.
bool ShotDirector::evaluateShot(const MissionShot& shot, int idx, double localSec, double dt,
                                const EntityPoseFn& poseOf, ShotPose& out) {
    out.fovYDeg = std::clamp(shot.fovYDeg, kFovMin, kFovMax);

    glm::dvec3 targetPos{0.0};
    glm::dquat targetOrient{1.0, 0.0, 0.0, 0.0};
    const bool needTarget = (shot.type == ShotType::Orbit || shot.type == ShotType::Chase);
    if (needTarget && !poseOf(shot.targetId, targetPos, targetOrient))
        return false; // dead/unspawned target — hold the last pose

    // ── eye ──────────────────────────────────────────────────────────────────
    switch (shot.type) {
    case ShotType::Static:
        out.eye = glm::dvec3(shot.pos[0], shot.pos[1], shot.pos[2]);
        break;
    case ShotType::Orbit: {
        // Angle sweeps 2π per `period`; a negative period reverses the sweep (clockwise).
        const double period = shot.orbitPeriodSec != 0.0 ? shot.orbitPeriodSec : 30.0;
        const double angle = 2.0 * std::numbers::pi * (localSec / period);
        out.eye = glm::dvec3(targetPos.x + shot.orbitRadiusM * std::cos(angle), targetPos.y + shot.orbitHeightM,
                             targetPos.z + shot.orbitRadiusM * std::sin(angle));
        break;
    }
    case ShotType::Chase: {
        // Rigid eye = target position + orientation-rotated body-frame offset (offset[0] along the
        // forward axis, negative = aft; [1] up; [2] right — body frame fwd=+X, up=+Y, right=+Z).
        const glm::dvec3 bodyOffset(shot.chaseOffset[0], shot.chaseOffset[1], shot.chaseOffset[2]);
        const glm::dvec3 rigidEye = targetPos + targetOrient * bodyOffset;
        const bool shotChanged = (m_chaseShotIdx != idx);
        if (shotChanged || shot.chaseStiffness <= 0.0 || dt <= 0.0 || !m_haveLastEvalT) {
            out.eye = rigidEye; // snap on entry / rigid / first frame
        } else {
            const double alpha = 1.0 - std::exp(-shot.chaseStiffness * dt);
            out.eye = glm::mix(m_chaseEye, rigidEye, alpha);
        }
        m_chaseEye = out.eye;
        m_chaseShotIdx = idx;
        break;
    }
    case ShotType::Move: {
        if (shot.keyframes.empty()) {
            out.eye = glm::dvec3(shot.pos[0], shot.pos[1], shot.pos[2]);
            break;
        }
        const auto& kf = shot.keyframes;
        if (localSec <= kf.front().timeSec || kf.size() == 1) {
            out.eye = glm::dvec3(kf.front().pos[0], kf.front().pos[1], kf.front().pos[2]);
            break;
        }
        if (localSec >= kf.back().timeSec) {
            out.eye = glm::dvec3(kf.back().pos[0], kf.back().pos[1], kf.back().pos[2]);
            break;
        }
        // Find segment [a, a+1] straddling localSec.
        std::size_t a = 0;
        for (std::size_t k = 0; k + 1 < kf.size(); ++k) {
            if (localSec >= kf[k].timeSec && localSec < kf[k + 1].timeSec) {
                a = k;
                break;
            }
        }
        const std::size_t b = a + 1;
        const double span = kf[b].timeSec - kf[a].timeSec;
        const double u = span > 1e-9 ? (localSec - kf[a].timeSec) / span : 0.0;
        if (shot.ease == ShotEase::Smooth) {
            const std::size_t p0 = (a == 0) ? a : a - 1;
            const std::size_t p3 = (b + 1 < kf.size()) ? b + 1 : b;
            for (int c = 0; c < 3; ++c)
                out.eye[c] = catmullRom(kf[p0].pos[c], kf[a].pos[c], kf[b].pos[c], kf[p3].pos[c], u);
        } else {
            for (int c = 0; c < 3; ++c)
                out.eye[c] = kf[a].pos[c] + (kf[b].pos[c] - kf[a].pos[c]) * u;
        }
        break;
    }
    }

    // ── look direction ─────────────────────────────────────────────────────────
    glm::dvec3 lookPoint;
    if (shot.lookAtPointSet) {
        lookPoint = glm::dvec3(shot.lookAtPoint[0], shot.lookAtPoint[1], shot.lookAtPoint[2]);
    } else if (!shot.lookAtId.empty()) {
        glm::dvec3 lp{0.0};
        glm::dquat lo{1.0, 0.0, 0.0, 0.0};
        if (!poseOf(shot.lookAtId, lp, lo))
            return false; // dead look target — hold the last pose
        lookPoint = lp;
    } else if (needTarget) {
        lookPoint = targetPos; // orbit/chase default: look at the tracked target
    } else {
        return false; // static/move without a resolvable look target (parser rejects this upstream)
    }

    const glm::dvec3 dir = lookPoint - out.eye;
    if (glm::dot(dir, dir) < 1e-12)
        out.fwd = glm::vec3(0.f, 0.f, -1.f); // eye coincides with look point — keep a sane forward
    else
        out.fwd = glm::normalize(glm::vec3(dir));
    out.up = upFor(out.fwd);
    return true;
}

ShotPose ShotDirector::evaluate(double t, const EntityPoseFn& poseOf) {
    const double dt = m_haveLastEvalT ? std::max(0.0, t - m_lastEvalT) : 0.0;

    ShotPose pose;
    if (m_shots.empty()) {
        // No shots: a neutral default. (The recorder should not drive off an empty director, but be
        // safe rather than divide-by-zero.)
        m_lastEvalT = t;
        m_haveLastEvalT = true;
        return pose;
    }

    // Locate the shot to evaluate + its local time, and whether we are inside a shot's span.
    int idx = 0;
    double local = 0.0;
    bool active = false;
    if (t < m_shots.front().startSec) {
        idx = 0;
        local = 0.0; // before the first shot: hold its start pose
    } else {
        std::size_t cand = 0;
        for (std::size_t i = 0; i < m_shots.size(); ++i) {
            if (m_shots[i].startSec <= t)
                cand = i;
            else
                break;
        }
        idx = static_cast<int>(cand);
        const double end = m_shots[cand].startSec + m_shots[cand].durationSec;
        if (t < end) {
            active = true;
            local = t - m_shots[cand].startSec;
        } else {
            active = false;
            local = m_shots[cand].durationSec; // gap / after-last: hold this shot's final pose
        }
    }

    const bool ok = evaluateShot(m_shots[static_cast<std::size_t>(idx)], idx, local, dt, poseOf, pose);
    if (ok) {
        pose.shotIndex = active ? idx : -1;
        pose.active = active;
        m_lastPose = pose;
        m_haveLastPose = true;
    } else if (m_haveLastPose) {
        pose = m_lastPose; // dead/unresolvable target — hold the last valid pose
        pose.shotIndex = active ? idx : -1;
        pose.active = active;
    } else {
        // No prior valid pose to fall back on: neutral default, never a snap-to-origin look.
        pose.shotIndex = active ? idx : -1;
        pose.active = active;
    }

    m_lastEvalT = t;
    m_haveLastEvalT = true;
    return pose;
}

} // namespace fl
