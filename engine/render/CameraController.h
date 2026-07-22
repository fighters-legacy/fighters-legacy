// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "RenderTypes.h"

#include <cstdint>
#include <glm/glm.hpp>

namespace fl {

// Build a CameraView from a pose (#698). Extracted from CameraController::view() so the target-slaved
// inset and any other secondary camera construct their view with the SAME reverse-Z / Y-flip /
// camera-relative conventions — they cannot diverge from the main path. `eye` is the world position;
// `forward`/`up` need not be normalized or exactly orthogonal.
[[nodiscard]] CameraView makeCameraView(const glm::dvec3& eye, glm::vec3 forward, glm::vec3 up, float aspectRatio,
                                        float fovY = 1.0472f, float near = 0.1f);

enum class CameraMode : uint8_t {
    Cockpit, // F1: locked inside the entity, looking out (the entity model is hidden in this view)
    Chase,   // F2: locked behind the entity, following its heading
    Free,    // F4: free-fly camera; moves anywhere in the world (only constraint: above the ground)
    Padlock, // F5: eye at the cockpit, slewed to keep the designated target centred (#697)
};

// Produces a CameraView each frame for use with IRenderer::setScene.
//
// There is a single underlying camera: an eye position plus a look direction (forward + up).
// CameraInput computes that pose every frame and calls setPose(). The three modes are just
// different ways of driving the one camera:
//   Free    — the pose is driven directly by the user (WASD/QE move the eye, mouse turns the view),
//             clamped so it cannot pass through the ground.
//   Chase   — the pose is computed to sit behind the entity and follow it.
//   Cockpit — the pose is locked to the entity (eye at the entity, looking along its forward axis).
//
// mode() is a label read by the game layer (HUD, hiding the ownship in cockpit); it does not change
// how view() builds the camera — every mode resolves to the same setPose()/view() path.
//
// All state is main-thread-only. No input processing is done here.
class CameraController {
  public:
    CameraController();

    void setMode(CameraMode mode) noexcept;
    [[nodiscard]] CameraMode mode() const noexcept;

    // Set the camera pose for this frame. Called by CameraInput once per frame for every mode.
    // eye     — camera world position (m).
    // forward — view direction (need not be normalized).
    // up      — approximate up vector (need not be exactly orthogonal to forward).
    void setPose(glm::dvec3 eye, glm::vec3 forward, glm::vec3 up) noexcept;

    // Build the CameraView for the current frame.
    // aspectRatio — viewport width / height.
    // fovY        — vertical field of view in radians (default 60°).
    // near        — near plane distance in meters (default 0.1 m).
    [[nodiscard]] CameraView view(float aspectRatio, float fovY = 1.0472f, float near = 0.1f) const;

  private:
    CameraMode m_mode{CameraMode::Cockpit};

    // Single camera pose (world space). worldOrigin = m_eye; view = lookAt(0, m_forward, m_up).
    glm::dvec3 m_eye{};
    glm::vec3 m_forward{0.0f, 0.0f, -1.0f};
    glm::vec3 m_up{0.0f, 1.0f, 0.0f};
};

} // namespace fl
