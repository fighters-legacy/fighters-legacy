// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/CameraController.h"

#include <glm/gtc/matrix_transform.hpp> // glm::lookAt

#include <cmath>

namespace fl {

CameraController::CameraController() = default;

void CameraController::setMode(CameraMode mode) noexcept {
    m_mode = mode;
}

CameraMode CameraController::mode() const noexcept {
    return m_mode;
}

void CameraController::setPose(glm::dvec3 eye, glm::vec3 forward, glm::vec3 up) noexcept {
    m_eye = eye;
    m_forward = forward;
    m_up = up;
}

CameraView makeCameraView(const glm::dvec3& eye, glm::vec3 forward, glm::vec3 up, float aspectRatio, float fovY,
                          float near) {
    CameraView cv;

    // Camera-relative rendering: worldOrigin is subtracted from world positions before upload, so
    // the view matrix is built from the local origin looking along the pose's forward/up.
    cv.worldOrigin = eye;
    glm::vec3 fwd = forward;
    if (glm::dot(fwd, fwd) < 1e-12f) // degenerate forward: fall back to -Z
        fwd = glm::vec3{0.0f, 0.0f, -1.0f};
    // Guard against forward ~parallel to up (otherwise lookAt produces NaNs).
    if (std::abs(glm::dot(glm::normalize(fwd), glm::normalize(up))) > 0.999f)
        up = (std::abs(fwd.y) > 0.5f) ? glm::vec3{0.0f, 0.0f, 1.0f} : glm::vec3{0.0f, 1.0f, 0.0f};
    cv.view = glm::lookAt(glm::vec3(0.f), fwd, up);

    // Infinite reverse-Z perspective with Vulkan clip-space Y-flip.
    float f = 1.0f / std::tan(fovY * 0.5f);
    cv.proj = glm::mat4(0.0f);
    cv.proj[0][0] = f / aspectRatio;
    cv.proj[1][1] = -f;
    cv.proj[2][3] = -1.0f;
    cv.proj[3][2] = near;

    return cv;
}

CameraView CameraController::view(float aspectRatio, float fovY, float near) const {
    return makeCameraView(m_eye, m_forward, m_up, aspectRatio, fovY, near);
}

} // namespace fl
