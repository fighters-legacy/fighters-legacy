// SPDX-License-Identifier: GPL-3.0-or-later
#include "CameraInput.h"

#include "debug/DebugConsole.h"
#include "render/CameraController.h"
#include "render/RenderSnapshot.h"
#include "render/TerrainStreamer.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <glm/ext/scalar_constants.hpp>
#include <glm/gtc/constants.hpp>

// Degrees to radians helper
static float toRad(float deg) {
    return deg * (glm::pi<float>() / 180.f);
}

void CameraInput::adjustThrottle(float delta) {
    m_sbThrottle = std::clamp(m_sbThrottle + delta, 0.f, 1.f);
}

void CameraInput::setThrottle(float t) {
    m_sbThrottle = std::clamp(t, 0.f, 1.f);
}

void CameraInput::onModeSwitch(fl::CameraMode newMode, const fl::EntityRenderEntry* player) {
    using fl::CameraMode;
    m_firstFrame = true;
    if (newMode == CameraMode::Cockpit) {
        m_cockpitYaw = 0.f;
        m_cockpitPitch = 0.f;
    } else if (newMode == CameraMode::Chase) {
        if (player) {
            // Initialise chase yaw to behind the player
            float ey = std::atan2(
                2.f * (player->orientation.w * player->orientation.y + player->orientation.x * player->orientation.z),
                1.f - 2.f * (player->orientation.y * player->orientation.y +
                             player->orientation.z * player->orientation.z));
            // Entity forward convention: body +X. "Behind" = camera in -X direction from entity.
            // sin(yaw)=-1 → -X camera offset; that requires yaw = ey - 90°.
            m_chaseYaw = glm::degrees(ey) - 90.f;
        }
        m_chasePitch = 20.f;
        m_chaseRadius = 25.f;
    } else if (newMode == CameraMode::Free) {
        if (player)
            m_sbPivot = player->position;
        m_sbPitch = 30.0f;
    }
}

void CameraInput::update(fl::CameraController& ctrl, const fl::EntityRenderEntry* player, const DebugConsole& console,
                         fl::TerrainStreamer& terrain) {
    float mx = 0.f, my = 0.f;
    SDL_MouseButtonFlags mb = SDL_GetMouseState(&mx, &my);
    const bool* keys = SDL_GetKeyboardState(nullptr);
    const bool consoleOpen = console.isOpen();

    const auto mode = ctrl.mode();
    using fl::CameraMode;

    switch (mode) {
    case CameraMode::Free: {
        if (!m_firstFrame && (mb & SDL_BUTTON_LMASK)) {
            m_sbYaw -= (mx - m_lastMx) * 0.35f;
            m_sbPitch += (my - m_lastMy) * 0.25f;
            m_sbPitch = std::clamp(m_sbPitch, -89.0f, 89.0f);
        }
        if (!consoleOpen) {
            if (keys[SDL_SCANCODE_EQUALS] || keys[SDL_SCANCODE_KP_PLUS])
                m_sbRadius = std::max(20.0f, m_sbRadius - 5.0f);
            if (keys[SDL_SCANCODE_MINUS] || keys[SDL_SCANCODE_KP_MINUS])
                m_sbRadius = std::min(5000.0f, m_sbRadius + 5.0f);
            const float speed = std::max(1.0f, m_sbRadius * 0.01f);
            const float yr = toRad(m_sbYaw);
            const glm::vec3 fwd{-std::sin(yr), 0.0f, -std::cos(yr)};
            const glm::vec3 rgt{std::cos(yr), 0.0f, -std::sin(yr)};
            if (keys[SDL_SCANCODE_W])
                m_sbPivot += glm::dvec3(fwd * speed);
            if (keys[SDL_SCANCODE_S])
                m_sbPivot -= glm::dvec3(fwd * speed);
            if (keys[SDL_SCANCODE_D])
                m_sbPivot += glm::dvec3(rgt * speed);
            if (keys[SDL_SCANCODE_A])
                m_sbPivot -= glm::dvec3(rgt * speed);
            if (keys[SDL_SCANCODE_E])
                m_sbPivot.y += speed;
            if (keys[SDL_SCANCODE_Q])
                m_sbPivot.y -= speed;
            if (keys[SDL_SCANCODE_R]) {
                m_sbPivot = player ? player->position : glm::dvec3{0.0, 2000.0, 0.0};
                m_sbYaw = 0.f;
                m_sbPitch = 30.f;
                m_sbRadius = 30.f;
            }
        }
        // Clamp pivot to terrain surface + 2 m eye clearance.
        {
            const double groundElev = terrain.heightAt(m_sbPivot.x, m_sbPivot.z);
            if (m_sbPivot.y < groundElev + 2.0)
                m_sbPivot.y = groundElev + 2.0;
        }
        ctrl.setFreeOrbit(m_sbPivot, m_sbYaw, m_sbPitch, m_sbRadius);
        break;
    }
    case CameraMode::Chase:
        if (player) {
            if (!m_firstFrame && (mb & SDL_BUTTON_LMASK)) {
                m_chaseYaw -= (mx - m_lastMx) * 0.35f;
                m_chasePitch += (my - m_lastMy) * 0.25f;
                m_chasePitch = std::clamp(m_chasePitch, -89.0f, 89.0f);
            }
            ctrl.setFreeOrbit(player->position, m_chaseYaw, m_chasePitch, m_chaseRadius);
        }
        break;
    case CameraMode::Cockpit:
        if (player) {
            ctrl.setTarget(player->position, player->orientation);
            if (!m_firstFrame && (mb & SDL_BUTTON_RMASK)) {
                m_cockpitYaw -= (mx - m_lastMx) * 0.35f;
                m_cockpitPitch += (my - m_lastMy) * 0.25f;
                m_cockpitPitch = std::clamp(m_cockpitPitch, -80.0f, 80.0f);
            }
            ctrl.setCockpitLook(m_cockpitYaw, m_cockpitPitch);
        }
        break;
    }

    m_lastMx = mx;
    m_lastMy = my;
    m_firstFrame = false;
}
