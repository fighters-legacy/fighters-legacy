// SPDX-License-Identifier: GPL-3.0-or-later
#include "CameraInput.h"

#include "IInput.h"
#include "console/GameConsole.h"
#include "render/CameraController.h"
#include "render/RenderSnapshot.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <glm/ext/scalar_constants.hpp>
#include <glm/gtc/constants.hpp>

// Degrees to radians helper
static float toRad(float deg) {
    return deg * (glm::pi<float>() / 180.f);
}

void CameraInput::pollModeKeys(fl::CameraController& ctrl, GameConsole& console, IInput& input,
                               const fl::EntityRenderEntry* player) {
    const bool* keys = SDL_GetKeyboardState(nullptr);

    const bool graveNow = keys[SDL_SCANCODE_GRAVE] != 0;
    if (graveNow && !m_gravePrev) {
        if (console.isOpen())
            console.close(input);
        else
            console.open(input);
    }
    m_gravePrev = graveNow;

    if (!console.isOpen()) {
        if (keys[SDL_SCANCODE_F1] && !m_f1Prev) {
            ctrl.setMode(fl::CameraMode::Cockpit);
            onModeSwitch(fl::CameraMode::Cockpit, player);
        }
        if (keys[SDL_SCANCODE_F2] && !m_f2Prev) {
            ctrl.setMode(fl::CameraMode::Chase);
            onModeSwitch(fl::CameraMode::Chase, player);
        }
        if (keys[SDL_SCANCODE_F4] && !m_f4Prev) {
            ctrl.setMode(fl::CameraMode::Free);
            onModeSwitch(fl::CameraMode::Free, player);
        }
    }
    m_f1Prev = keys[SDL_SCANCODE_F1] != 0;
    m_f2Prev = keys[SDL_SCANCODE_F2] != 0;
    m_f4Prev = keys[SDL_SCANCODE_F4] != 0;
}

void CameraInput::startSession() noexcept {
    m_needsPivotSnap = true;
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
        m_chaseYawOffset = 0.f; // directly behind the tail
        m_chasePitch = 3.f;     // nearly level — behind the aircraft, not looking down on it
        m_chaseRadius = 80.f;
    } else if (newMode == CameraMode::Free) {
        if (player) {
            m_sbPivot = player->position;
            m_needsPivotSnap = false;
        } else {
            m_needsPivotSnap = true;
        }
        m_sbPitch = 30.0f;
    }
}

void CameraInput::update(fl::CameraController& ctrl, const fl::EntityRenderEntry* player, const GameConsole& console) {
    float mx = 0.f, my = 0.f;
    SDL_MouseButtonFlags mb = SDL_GetMouseState(&mx, &my);
    const bool* keys = SDL_GetKeyboardState(nullptr);
    const bool consoleOpen = console.isOpen();

    const auto mode = ctrl.mode();
    using fl::CameraMode;

    switch (mode) {
    case CameraMode::Free: {
        // Snap pivot to player on the first free-mode update that has a valid player.
        // Handles the common case where the game starts in Free mode (no mode switch fires).
        if (m_needsPivotSnap && player) {
            m_sbPivot = player->position;
            m_needsPivotSnap = false;
        }
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
        ctrl.setFreeOrbit(m_sbPivot, m_sbYaw, m_sbPitch, m_sbRadius);
        break;
    }
    case CameraMode::Chase:
        if (player) {
            // Pivot locked to the entity; yaw locked to the entity heading so the camera trails
            // the tail as the aircraft turns. LMB drag adds a relative offset; scroll zooms.
            if (!m_firstFrame && (mb & SDL_BUTTON_LMASK)) {
                m_chaseYawOffset -= (mx - m_lastMx) * 0.35f;
                m_chasePitch += (my - m_lastMy) * 0.25f;
                m_chasePitch = std::clamp(m_chasePitch, -89.0f, 89.0f);
            }
            static constexpr float kTickDt = 1.0f / 60.0f;
            const glm::dvec3 pivot = player->position + glm::dvec3(player->velocity * (m_renderAlpha * kTickDt));
            // Place the orbit camera on the tail side so it trails behind the nose (body +X) and
            // looks forward along the aircraft's heading. yaw = atan2(fwd.x, fwd.z). The offset
            // rotates around relative to "directly behind".
            const glm::vec3 fwd = player->orientation * glm::vec3(1.f, 0.f, 0.f);
            const float behindYaw = glm::degrees(std::atan2(fwd.x, fwd.z));
            ctrl.setFreeOrbit(pivot, behindYaw + m_chaseYawOffset, m_chasePitch, m_chaseRadius);
        }
        break;
    case CameraMode::Cockpit:
        if (player) {
            // Extrapolate camera origin to match SceneRenderer's entity position.
            static constexpr float kTickDt = 1.0f / 60.0f;
            glm::dvec3 pos = player->position + glm::dvec3(player->velocity * (m_renderAlpha * kTickDt));
            ctrl.setTarget(pos, player->orientation);
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
