// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "RenderTypes.h"

#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace fl {

enum class CameraMode : uint8_t {
    Cockpit, // F1: camera at entity; RMB held to look around
    Chase,   // F2: orbit around entity; mouse drag to rotate; scroll to zoom
    Free,    // F4: fully free camera; WASD/QE move anywhere, mouse drag orbits
};

// Produces a CameraView each frame for use with IRenderer::setScene.
//
// Cockpit mode — camera sits at the target entity's position.
//   Call setTarget() + setCockpitLook() each frame.
//   RMB drag accumulates look offsets via setCockpitLook().
//
// Chase mode — spherical orbit around the target entity.
//   Call setFreeOrbit(entityPos, yaw, pitch, radius) each frame.
//   Mouse drag / scroll update the orbit parameters before calling setFreeOrbit.
//
// Free mode — spherical orbit around a freely movable world pivot.
//   Call setFreeOrbit(pivot, yaw, pitch, radius) each frame.
//   WASD/QE pan the pivot; mouse drag orbits; scroll zooms.
//
// Chase and Free share the same orbit code in view(); the distinction is only
// in how the caller chooses the pivot (entity position vs. free world point).
//
// All state is main-thread-only. No input processing is done here.
class CameraController {
  public:
    CameraController();

    void setMode(CameraMode mode) noexcept;
    [[nodiscard]] CameraMode mode() const noexcept;

    // Free-orbit parameters (used by both Free and Chase modes).
    // pivot    — world-space look target (m).
    // yaw      — horizontal rotation, degrees (0 = camera south of pivot).
    // pitch    — elevation angle, degrees (positive = camera above horizon).
    // distance — camera distance from pivot (m).
    void setFreeOrbit(glm::dvec3 pivot, float yaw, float pitch, float distance) noexcept;

    // Cockpit target (call each frame when in Cockpit mode).
    void setTarget(glm::dvec3 worldPosition, glm::quat worldOrientation) noexcept;

    // Cockpit look offsets from entity forward, in degrees (call each frame when in Cockpit mode).
    // yawDeg   — left/right offset; 0 = entity forward, ±180 = looking behind.
    // pitchDeg — up/down offset; positive = up, clamped to ±80° by caller.
    void setCockpitLook(float yawDeg, float pitchDeg) noexcept;

    // Build the CameraView for the current frame.
    // aspectRatio — viewport width / height.
    // fovY        — vertical field of view in radians (default 60°).
    // near        — near plane distance in meters (default 0.1 m).
    [[nodiscard]] CameraView view(float aspectRatio, float fovY = 1.0472f, float near = 0.1f) const;

  private:
    CameraMode m_mode{CameraMode::Free};

    // Free/Chase orbit state
    glm::dvec3 m_pivot{};
    float m_yaw{0.0f};
    float m_pitch{20.0f};
    float m_distance{50.0f};

    // Cockpit state
    glm::dvec3 m_targetPos{};
    glm::quat m_targetOri{glm::quat(1.0f, 0.0f, 0.0f, 0.0f)};
    float m_cockpitYaw{0.0f};
    float m_cockpitPitch{0.0f};
};

} // namespace fl
