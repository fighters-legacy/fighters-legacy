// SPDX-License-Identifier: GPL-3.0-or-later
#include "CameraInput.h"

#include "IInput.h"
#include "console/GameConsole.h"
#include "render/CameraController.h"
#include "render/RenderSnapshot.h"
#include "render/TerrainStreamer.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

namespace fl {

namespace {
constexpr float kTickDt = 1.0f / 60.0f;
constexpr double kFlyGroundMarginM = 0.0; // free-fly camera descends to ground level, not below

// Height of the entity's visual centre above its (ground-contact) origin. The cockpit eye is
// raised by this so it sits inside the body rather than at the wheels. Placeholder for the builtin
// tetrahedron (centroid is R*sqrt(8/9)/2 above the base, ~2.36 m at R=5); real content will derive
// this from the mesh bounds / a cockpit-anchor node.
constexpr double kEntityCentreHeightM = 2.36;

float toRad(float deg) {
    return deg * (glm::pi<float>() / 180.f);
}

// Horizontal "behind" direction for an entity (opposite its nose), normalized; falls back to +Z
// when the nose points straight up/down.
glm::dvec3 behindHorizontal(const glm::vec3& forward) {
    glm::dvec3 fwdH{forward.x, 0.0, forward.z};
    const double len = glm::length(fwdH);
    if (len < 1e-6)
        return glm::dvec3{0.0, 0.0, 1.0};
    return -fwdH / len;
}
} // namespace

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
    m_needsFlyInit = true;
}

void CameraInput::adjustThrottle(float delta) {
    m_throttle = std::clamp(m_throttle + delta, 0.f, 1.f);
}

void CameraInput::setThrottle(float t) {
    m_throttle = std::clamp(t, 0.f, 1.f);
}

void CameraInput::initFlyFromPlayer(const fl::EntityRenderEntry& player) {
    const glm::vec3 fwd = player.orientation * glm::vec3{1.f, 0.f, 0.f};
    // Start ~18 m behind and ~8 m above the aircraft, looking at it (origin = ground contact).
    const glm::dvec3 target = player.position;
    m_flyEye = target + behindHorizontal(fwd) * 18.0 + glm::dvec3{0.0, 8.0, 0.0};
    const glm::dvec3 toTarget = target - m_flyEye;
    const double len = glm::length(toTarget);
    if (len > 1e-6) {
        const glm::dvec3 dir = toTarget / len;
        m_flyYaw = glm::degrees(static_cast<float>(std::atan2(-dir.x, -dir.z)));
        m_flyPitch = glm::degrees(static_cast<float>(std::asin(std::clamp(dir.y, -1.0, 1.0))));
    }
    m_needsFlyInit = false;
}

void CameraInput::onModeSwitch(fl::CameraMode newMode, const fl::EntityRenderEntry* player) {
    using fl::CameraMode;
    m_firstFrame = true;
    if (newMode == CameraMode::Cockpit) {
        m_cockpitYaw = 0.f;
        m_cockpitPitch = 0.f;
    } else if (newMode == CameraMode::Chase) {
        m_chasePitch = 8.f;
        m_chaseDistance = 25.f;
    } else if (newMode == CameraMode::Free) {
        if (player)
            initFlyFromPlayer(*player);
        else
            m_needsFlyInit = true;
    }
}

void CameraInput::update(fl::CameraController& ctrl, const fl::EntityRenderEntry* player, const GameConsole& console,
                         fl::TerrainStreamer& terrain) {
    float mx = 0.f, my = 0.f;
    const SDL_MouseButtonFlags mb = SDL_GetMouseState(&mx, &my);
    const bool* keys = SDL_GetKeyboardState(nullptr);
    const bool consoleOpen = console.isOpen();

    // Frame time for frame-rate-independent fly movement. Movement was previously applied per
    // frame, so at high frame rates (e.g. 240 fps) it moved ~4x faster than intended.
    const auto now = std::chrono::steady_clock::now();
    float dt = m_haveLastUpdate ? std::chrono::duration<float>(now - m_lastUpdate).count() : (1.0f / 60.0f);
    dt = std::clamp(dt, 0.0f, 0.1f); // guard against pauses / first-frame spikes
    m_lastUpdate = now;
    m_haveLastUpdate = true;

    using fl::CameraMode;
    switch (ctrl.mode()) {
    case CameraMode::Free: {
        // The base camera: move the eye freely and turn the view; only constraint is the ground.
        if (m_needsFlyInit && player)
            initFlyFromPlayer(*player);

        if (!m_firstFrame && (mb & SDL_BUTTON_LMASK)) {
            m_flyYaw -= (mx - m_lastMx) * 0.35f;
            m_flyPitch -= (my - m_lastMy) * 0.25f; // mouse up -> look up
            m_flyPitch = std::clamp(m_flyPitch, -89.0f, 89.0f);
        }
        if (!consoleOpen) {
            if (keys[SDL_SCANCODE_EQUALS] || keys[SDL_SCANCODE_KP_PLUS])
                m_flySpeed = std::min(1000.0f, m_flySpeed * 1.08f);
            if (keys[SDL_SCANCODE_MINUS] || keys[SDL_SCANCODE_KP_MINUS])
                m_flySpeed = std::max(2.0f, m_flySpeed * 0.92f);

            const float yr = toRad(m_flyYaw);
            const glm::dvec3 fwdH{-std::sin(yr), 0.0, -std::cos(yr)};
            const glm::dvec3 rgtH{std::cos(yr), 0.0, -std::sin(yr)};
            const double step = static_cast<double>(m_flySpeed) * dt; // m_flySpeed is m/s
            if (keys[SDL_SCANCODE_W])
                m_flyEye += fwdH * step;
            if (keys[SDL_SCANCODE_S])
                m_flyEye -= fwdH * step;
            if (keys[SDL_SCANCODE_D])
                m_flyEye += rgtH * step;
            if (keys[SDL_SCANCODE_A])
                m_flyEye -= rgtH * step;
            if (keys[SDL_SCANCODE_E])
                m_flyEye.y += step;
            if (keys[SDL_SCANCODE_Q])
                m_flyEye.y -= step;
            if (keys[SDL_SCANCODE_R] && player)
                initFlyFromPlayer(*player);
        }

        // Hard floor: never pass through the ground.
        const double minY = terrain.heightAt(m_flyEye.x, m_flyEye.z) + kFlyGroundMarginM;
        if (m_flyEye.y < minY)
            m_flyEye.y = minY;

        const float yr = toRad(m_flyYaw);
        const float pr = toRad(m_flyPitch);
        const float cp = std::cos(pr);
        const glm::vec3 forward{-std::sin(yr) * cp, std::sin(pr), -std::cos(yr) * cp};
        ctrl.setPose(m_flyEye, forward, glm::vec3{0.f, 1.f, 0.f});
        break;
    }
    case CameraMode::Chase:
        if (player) {
            // Locked behind the tail, following the entity heading. The user cannot move it.
            // Aim at the entity (its origin is the ground-contact point, where it visibly sits).
            const glm::dvec3 target = player->position + glm::dvec3(player->velocity * (m_renderAlpha * kTickDt));
            const glm::vec3 fwd = player->orientation * glm::vec3{1.f, 0.f, 0.f};
            const float pr = toRad(m_chasePitch);
            const double horiz = static_cast<double>(m_chaseDistance) * std::cos(pr);
            const double vert = static_cast<double>(m_chaseDistance) * std::sin(pr);
            const glm::dvec3 eye = target + behindHorizontal(fwd) * horiz + glm::dvec3{0.0, vert, 0.0};
            ctrl.setPose(eye, glm::vec3(target - eye), glm::vec3{0.f, 1.f, 0.f});
        }
        break;
    case CameraMode::Cockpit:
        if (player) {
            // Locked inside the entity, looking along its forward axis (+ RMB look offset). The
            // origin is the ground-contact point, so raise the eye to the body centre.
            const glm::dvec3 eye =
                player->position + glm::dvec3(player->velocity * (m_renderAlpha * kTickDt)) +
                glm::dvec3(player->orientation * glm::vec3{0.f, static_cast<float>(kEntityCentreHeightM), 0.f});
            if (!m_firstFrame && (mb & SDL_BUTTON_RMASK)) {
                m_cockpitYaw -= (mx - m_lastMx) * 0.35f;
                m_cockpitPitch += (my - m_lastMy) * 0.25f;
                m_cockpitPitch = std::clamp(m_cockpitPitch, -80.0f, 80.0f);
            }
            const glm::quat lookRot = glm::angleAxis(glm::radians(m_cockpitYaw), glm::vec3{0.f, 1.f, 0.f}) *
                                      glm::angleAxis(glm::radians(m_cockpitPitch), glm::vec3{0.f, 0.f, 1.f});
            const glm::vec3 forward = player->orientation * lookRot * glm::vec3{1.f, 0.f, 0.f};
            const glm::vec3 up = player->orientation * glm::vec3{0.f, 1.f, 0.f};
            ctrl.setPose(eye, forward, up);
        }
        break;
    }

    m_lastMx = mx;
    m_lastMy = my;
    m_firstFrame = false;
}

} // namespace fl
