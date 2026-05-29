// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/CameraController.h"

#include <glm/gtc/matrix_transform.hpp> // glm::lookAt
#include <glm/gtc/quaternion.hpp>       // glm::quat operator*

#include <cmath>

namespace fl {

CameraController::CameraController() = default;

void CameraController::setMode(CameraMode mode) noexcept {
    m_mode = mode;
}

CameraMode CameraController::mode() const noexcept {
    return m_mode;
}

void CameraController::setFreeOrbit(glm::vec3 pivot, float yaw, float pitch, float distance) noexcept {
    m_pivot = pivot;
    m_yaw = yaw;
    m_pitch = pitch;
    m_distance = distance;
}

void CameraController::setTarget(glm::vec3 worldPosition, glm::quat worldOrientation) noexcept {
    m_targetPos = worldPosition;
    m_targetOri = worldOrientation;
}

CameraView CameraController::view(float aspectRatio, float fovY, float near) const {
    glm::vec3 camWorldPos;
    glm::vec3 lookTarget;

    if (m_mode == CameraMode::Chase) {
        // Position camera behind and above the target entity.
        // kChaseBack is in entity +Z local space (away from nose in standard body frame).
        glm::vec3 localBack{0.0f, 0.0f, kChaseBack};
        glm::vec3 worldBack = m_targetOri * localBack;
        glm::vec3 worldUp{0.0f, kChaseUp, 0.0f};
        camWorldPos = m_targetPos + worldBack + worldUp;
        lookTarget = m_targetPos;
    } else {
        // Spherical orbit around pivot.
        float yawRad = glm::radians(m_yaw);
        float pitchRad = glm::radians(m_pitch);
        float cosP = std::cos(pitchRad);
        camWorldPos = m_pivot + glm::vec3(std::sin(yawRad) * cosP * m_distance, std::sin(pitchRad) * m_distance,
                                          std::cos(yawRad) * cosP * m_distance);
        lookTarget = m_pivot;
    }

    CameraView cv;
    cv.worldOrigin = camWorldPos;

    // Camera-relative view: camera sits at origin in camera-relative world space.
    cv.view = glm::lookAt(glm::vec3(0.0f), lookTarget - camWorldPos, glm::vec3(0.0f, 1.0f, 0.0f));

    // Infinite reverse-Z perspective with Vulkan clip-space Y-flip.
    // near plane → depth 1.0; far (∞) → depth 0.0.
    // proj[3][2] = near (used by VkRenderer shadow cascade split).
    float f = 1.0f / std::tan(fovY * 0.5f);
    cv.proj = glm::mat4(0.0f);
    cv.proj[0][0] = f / aspectRatio;
    cv.proj[1][1] = -f; // Y-flip for Vulkan
    cv.proj[2][3] = -1.0f;
    cv.proj[3][2] = near;

    return cv;
}

} // namespace fl
