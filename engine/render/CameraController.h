// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "RenderTypes.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace fl {

enum class CameraMode : uint8_t { Free, Chase };

// Produces a CameraView each frame for use with IRenderer::setScene.
//
// Free mode  — spherical-orbit camera around a configurable pivot.
// Chase mode — camera positioned behind and above a moving target entity.
//
// All state is main-thread-only. No input processing is done here; callers
// drive the camera by calling setFreeOrbit / setTarget directly.
class CameraController {
  public:
    CameraController();

    void setMode(CameraMode mode) noexcept;
    [[nodiscard]] CameraMode mode() const noexcept;

    // Free-orbit parameters (effective in Free mode).
    // pivot   — world-space look target (m).
    // yaw     — horizontal rotation, degrees (0 = camera south of pivot).
    // pitch   — elevation angle, degrees (positive = camera above horizon).
    // distance — camera distance from pivot (m).
    void setFreeOrbit(glm::dvec3 pivot, float yaw, float pitch, float distance) noexcept;

    // Chase target (effective in Chase mode). Call each frame with the latest entity state.
    void setTarget(glm::dvec3 worldPosition, glm::quat worldOrientation) noexcept;

    // Build the CameraView for the current frame.
    // aspectRatio — viewport width / height.
    // fovY        — vertical field of view in radians (default 60°).
    // near        — near plane distance in meters (default 0.1 m).
    [[nodiscard]] CameraView view(float aspectRatio, float fovY = 1.0472f, float near = 0.1f) const;

  private:
    CameraMode m_mode{CameraMode::Free};

    // Free-orbit state
    glm::dvec3 m_pivot{};
    float m_yaw{0.0f};
    float m_pitch{20.0f};
    float m_distance{50.0f};

    // Chase state
    glm::dvec3 m_targetPos{};
    glm::quat m_targetOri{glm::quat(1.0f, 0.0f, 0.0f, 0.0f)};

    // Chase camera offsets in entity-local space (m).
    static constexpr float kChaseBack{30.0f};
    static constexpr float kChaseUp{5.0f};
};

} // namespace fl
