// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// ShotDirector (#911) — the pure camera-pose evaluator for the cinematic demo-recording pipeline.
// Turns a mission's `cameras:` shot list (MissionShot, #910) + live entity poses into a camera pose
// at a given sim time. It lives in engine-mission (GLM + stdlib only, no HAL) so it is fully
// unit-testable and the recording client and any tooling share one implementation.
//
// The recorder calls evaluate() once per capture boundary tick, in order, at a FIXED step. Chase
// smoothing carries state across those in-order fixed-step calls, so re-running a recording is
// reproducible (a single out-of-order evaluate() is NOT stateless by design — that is what "a
// deterministic function of (t, fixed step)" means for the chase lever).

#include "mission/Mission.h"

#include <functional>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string_view>
#include <vector>

namespace fl {

// The resolved camera pose for one frame. `fwd`/`up` are a look direction + approximate up (fed
// straight into CameraController::setPose); `fovYDeg` is the per-shot vertical FOV (clamped
// [20, 120]). `shotIndex` is the active shot, or -1 when before the first / in a gap / after the
// last shot (a held pose). `active` is true only while inside a shot's [start, start+duration) span.
struct ShotPose {
    glm::dvec3 eye{0.0, 0.0, 0.0};
    glm::vec3 fwd{0.f, 0.f, -1.f};
    glm::vec3 up{0.f, 1.f, 0.f};
    float fovYDeg{60.f};
    int shotIndex{-1};
    bool active{false};
};

// Resolve a mission object id to its current world pose. Returns false when the entity is not
// present (dead / not spawned / not in the roster) — the director then holds the last valid pose.
using EntityPoseFn = std::function<bool(std::string_view id, glm::dvec3& pos, glm::dquat& orient)>;

class ShotDirector {
  public:
    // shots must be sorted + non-overlapping (parseMission guarantees this). A copy is taken.
    explicit ShotDirector(std::vector<MissionShot> shots);

    // Camera pose at simTimeSec (sim-seconds from mission start). Not const: chase smoothing and the
    // dead-target fallback carry state across in-order fixed-step calls (see the header note).
    ShotPose evaluate(double simTimeSec, const EntityPoseFn& poseOf);

    // The last shot's start + duration (0 when there are no shots) — the recorder's end-of-track cue.
    [[nodiscard]] double totalDurationSec() const;

    [[nodiscard]] std::size_t shotCount() const noexcept {
        return m_shots.size();
    }

  private:
    // Fill `out` for shot `idx` at local time `localSec` (dt = seconds since the previous evaluate,
    // for chase smoothing). Returns false if a required entity lookup failed (caller holds last pose).
    bool evaluateShot(const MissionShot& shot, int idx, double localSec, double dt, const EntityPoseFn& poseOf,
                      ShotPose& out);

    std::vector<MissionShot> m_shots;

    // Cross-call smoothing / fallback state.
    double m_lastEvalT{0.0};
    bool m_haveLastEvalT{false};
    glm::dvec3 m_chaseEye{0.0, 0.0, 0.0};
    int m_chaseShotIdx{-1}; // which shot m_chaseEye tracks (reset when the active chase shot changes)
    ShotPose m_lastPose{};  // last successfully resolved pose (dead-target fallback)
    bool m_haveLastPose{false};
};

} // namespace fl
