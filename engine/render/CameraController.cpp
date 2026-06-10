// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/CameraController.h"

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/gtc/matrix_transform.hpp> // glm::lookAt
#include <glm/gtc/quaternion.hpp>       // glm::quat operator*, glm::angleAxis

#include <cmath>

namespace fl {

CameraController::CameraController() = default;

void CameraController::setMode(CameraMode mode) noexcept {
    m_mode = mode;
}

CameraMode CameraController::mode() const noexcept {
    return m_mode;
}

void CameraController::setFreeOrbit(glm::dvec3 pivot, float yaw, float pitch, float distance) noexcept {
    m_pivot = pivot;
    m_yaw = yaw;
    m_pitch = pitch;
    m_distance = distance;
}

void CameraController::setTarget(glm::dvec3 worldPosition, glm::quat worldOrientation) noexcept {
    m_targetPos = worldPosition;
    m_targetOri = worldOrientation;
}

void CameraController::setCockpitLook(float yawDeg, float pitchDeg) noexcept {
    m_cockpitYaw = yawDeg;
    m_cockpitPitch = pitchDeg;
}

CameraView CameraController::view(float aspectRatio, float fovY, float near) const {
    CameraView cv;

    switch (m_mode) {
    case CameraMode::Cockpit: {
        // Entity forward convention: body +X (matches flight model longitudinal axis and builtin
        // tetrahedron orientation). Yaw rotates around body Y; pitch rotates around body +Z
        // so that positive pitch tilts the view toward +Y (upward) when facing +X.
        glm::quat lookRot = glm::angleAxis(glm::radians(m_cockpitYaw), glm::vec3{0.f, 1.f, 0.f}) *
                            glm::angleAxis(glm::radians(m_cockpitPitch), glm::vec3{0.f, 0.f, 1.f});
        glm::vec3 viewDir = m_targetOri * lookRot * glm::vec3{1.f, 0.f, 0.f};
        cv.worldOrigin = m_targetPos;
        // Use the entity's body +Y (up) in world space so that banking rolls the
        // horizon. The look offset (yaw/pitch) does not affect the seat's up direction.
        glm::vec3 up = m_targetOri * glm::vec3{0.f, 1.f, 0.f};
        if (std::abs(glm::dot(viewDir, up)) > 0.999f)
            up = m_targetOri * glm::vec3{1.f, 0.f, 0.f}; // fallback: entity forward
        cv.view = glm::lookAt(glm::vec3(0.f), viewDir, up);
        break;
    }
    case CameraMode::Chase:
    case CameraMode::Free:
    default: {
        // Spherical orbit around pivot.
        // Chase: caller sets pivot = entity position each frame.
        // Free: caller sets pivot = freely movable world reference point.
        float yawRad = glm::radians(m_yaw);
        float pitchRad = glm::radians(m_pitch);
        float cosP = std::cos(pitchRad);
        glm::dvec3 camWorldPos =
            m_pivot + glm::dvec3(std::sin(yawRad) * cosP * m_distance, std::sin(pitchRad) * m_distance,
                                 std::cos(yawRad) * cosP * m_distance);
        cv.worldOrigin = camWorldPos;
        cv.view = glm::lookAt(glm::vec3(0.f), glm::vec3(m_pivot - camWorldPos), glm::vec3(0.f, 1.f, 0.f));
        break;
    }
    }

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
