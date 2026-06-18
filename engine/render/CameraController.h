// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "RenderTypes.h"

#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace fl {

enum class CameraMode : uint8_t {
    Cockpit, // F1: camera at entity; RMB held to look around
    Chase,   // F2: orbit around entity; LMB drag to rotate; scroll to zoom
    Free,    // F4: fully free camera; WASD/QE move anywhere, LMB drag orbits
};

// Produces a CameraView each frame for use with IRenderer::setScene.
//
// Cockpit mode — camera sits at the entity's world position, looking along its
//   body forward axis. Call setTarget() + setCockpitLook() each frame.
//   RMB drag accumulates look offsets via setCockpitLook().
//
// Chase mode — spherical orbit around the entity. Same math as Free mode;
//   the only difference is that CameraInput locks the pivot to the entity
//   position each frame. Call setFreeOrbit(entityPos, yaw, pitch, radius).
//
// Free mode — spherical orbit around a freely movable world pivot.
//   WASD/QE pan the pivot; LMB drag orbits; scroll zooms.
//
// All state is main-thread-only. No input processing is done here.
class CameraController {
  public:
    CameraController();

    void setMode(CameraMode mode) noexcept;
    [[nodiscard]] CameraMode mode() const noexcept;

    // Orbit parameters — used by both Chase and Free modes.
    // Chase: caller passes entity position as pivot each frame.
    // Free:  caller passes freely movable world reference point as pivot.
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
    CameraMode m_mode{CameraMode::Cockpit};

    // Shared orbit state (Chase and Free)
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
