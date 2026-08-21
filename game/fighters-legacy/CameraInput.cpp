// SPDX-License-Identifier: GPL-3.0-or-later
#include "CameraInput.h"

#include "IInput.h"
#include "console/GameConsole.h"
#include "flight/LocalFrame.h"   // radialUp: camera "up" = radial direction on a spherical planet
#include "input/BindingQuery.h"  // actionDown / actionJustPressed (#689/#1050)
#include "input/InputBindings.h" // camera-mode + View* pan bindings (#689)
#include "input/InputSources.h"  // the live input hardware, one struct (#1061)
#include "render/CameraController.h"
#include "render/RenderSnapshot.h"
#include "render/TerrainStreamer.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>
#include <numbers>

namespace fl {

namespace {

constexpr double kFlyGroundMarginM = 0.0; // free-fly camera descends to ground level, not below

// Height of the entity's visual centre above its (ground-contact) origin. The cockpit eye is
// raised by this so it sits inside the body rather than at the wheels. Placeholder for the builtin
// tetrahedron (centroid is R*sqrt(8/9)/2 above the base, ~2.36 m at R=5); real content will derive
// this from the mesh bounds / a cockpit-anchor node.
constexpr double kEntityCentreHeightM = 2.36;

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

void CameraInput::pollModeKeys(fl::CameraController& ctrl, GameConsole& console, const fl::InputSources& sources,
                               const fl::EntityRenderEntry* player) {
    if (!m_bindings || !sources.input)
        return;

    // The console toggle is a bound action too (#1050) — it was the last raw scancode read in this
    // file, and a control the binding table does not own is one the conflict checker cannot see.
    if (fl::actionJustPressed(sources, *m_bindings, fl::InputAction::ConsoleToggle)) {
        if (console.isOpen())
            console.close(*sources.input);
        else
            console.open(*sources.input);
    }

    if (!console.isOpen()) {
        // Camera-mode switches route through InputBindings (#689): rebindable, and since #1061
        // reachable from any bound device — the rising edge comes from the HAL for a key or a button,
        // and from the device table's previous sample for a POV hat.
        if (fl::actionJustPressed(sources, *m_bindings, fl::InputAction::CameraCockpit)) {
            ctrl.setMode(fl::CameraMode::Cockpit);
            onModeSwitch(fl::CameraMode::Cockpit, player);
        }
        if (fl::actionJustPressed(sources, *m_bindings, fl::InputAction::CameraChase)) {
            ctrl.setMode(fl::CameraMode::Chase);
            onModeSwitch(fl::CameraMode::Chase, player);
        }
        if (fl::actionJustPressed(sources, *m_bindings, fl::InputAction::CameraFree)) {
            ctrl.setMode(fl::CameraMode::Free);
            onModeSwitch(fl::CameraMode::Free, player);
        }
    }
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
                         fl::TerrainStreamer& terrain, const fl::InputSources& sources) {
    float mx = 0.f, my = 0.f;
    const SDL_MouseButtonFlags mb = SDL_GetMouseState(&mx, &my);
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
        // Free-camera movement is bound, not raw scancodes (#1050). It is reachable in EVERY
        // session mode — including while flying — so its keys have to be distinct from the flight
        // controls, and the only way the conflict checker can enforce that is if it can see them.
        if (!consoleOpen && m_bindings) {
            auto camDown = [&](fl::InputAction a) { return fl::actionDown(sources, *m_bindings, a); };
            if (camDown(fl::InputAction::FreeCamFaster))
                m_flySpeed = std::min(1000.0f, m_flySpeed * 1.08f);
            if (camDown(fl::InputAction::FreeCamSlower))
                m_flySpeed = std::max(2.0f, m_flySpeed * 0.92f);

            const float yr = glm::radians(m_flyYaw);
            const glm::dvec3 fwdH{-std::sin(yr), 0.0, -std::cos(yr)};
            const glm::dvec3 rgtH{std::cos(yr), 0.0, -std::sin(yr)};
            const double step = static_cast<double>(m_flySpeed) * dt; // m_flySpeed is m/s
            if (camDown(fl::InputAction::FreeCamForward))
                m_flyEye += fwdH * step;
            if (camDown(fl::InputAction::FreeCamBack))
                m_flyEye -= fwdH * step;
            if (camDown(fl::InputAction::FreeCamRight))
                m_flyEye += rgtH * step;
            if (camDown(fl::InputAction::FreeCamLeft))
                m_flyEye -= rgtH * step;
            if (camDown(fl::InputAction::FreeCamUp))
                m_flyEye.y += step;
            if (camDown(fl::InputAction::FreeCamDown))
                m_flyEye.y -= step;
            if (camDown(fl::InputAction::FreeCamReset) && player)
                initFlyFromPlayer(*player);
        }

        // Hard floor: never pass through the ground. Keep the eye's geodetic altitude at least a
        // margin above the terrain radial elevation, pushing outward along the local radial up so
        // the clamp holds far from the world origin (#477). Near origin this reduces to a world-Y
        // floor.
        const double terrElev = terrain.heightAt(m_flyEye);
        const double eyeAlt = geodeticAltitude(m_flyEye.x, m_flyEye.y, m_flyEye.z, m_planetRadiusM);
        const double minAlt = terrElev + kFlyGroundMarginM;
        if (eyeAlt < minAlt)
            m_flyEye += glm::dvec3(radialUp(m_flyEye, m_planetRadiusM)) * (minAlt - eyeAlt);

        const float yr = glm::radians(m_flyYaw);
        const float pr = glm::radians(m_flyPitch);
        const float cp = std::cos(pr);
        const glm::vec3 forward{-std::sin(yr) * cp, std::sin(pr), -std::cos(yr) * cp};
        // Up = radial direction from the planet centre so the horizon stays level far from origin.
        m_lastEye = m_flyEye;
        ctrl.setPose(m_flyEye, forward, radialUp(m_flyEye, m_planetRadiusM));
        break;
    }
    case CameraMode::Chase:
        if (player) {
            // Locked behind the tail, following the entity heading. The user cannot move it.
            // Aim at the entity (its origin is the ground-contact point, where it visibly sits).
            const glm::dvec3 target =
                player->position + glm::dvec3(player->velocity * (m_renderAlpha * m_serverTickRate.dtSeconds()));
            const glm::vec3 fwd = player->orientation * glm::vec3{1.f, 0.f, 0.f};
            const float pr = glm::radians(m_chasePitch);
            const double horiz = static_cast<double>(m_chaseDistance) * std::cos(pr);
            const double vert = static_cast<double>(m_chaseDistance) * std::sin(pr);
            glm::dvec3 eye;
            if (m_groundScene) {
                // Ground-crew scene (#55): a slow orbit around the parked aircraft on the ramp.
                // Wall-clock driven — this is presentation, nothing downstream reads it.
                constexpr double kOrbitDegPerS = 6.0;
                const double t =
                    std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
                const double az = t * kOrbitDegPerS * (std::numbers::pi / 180.0);
                eye = target + glm::dvec3{std::cos(az) * horiz, vert, std::sin(az) * horiz};
            } else {
                eye = target + behindHorizontal(fwd) * horiz + glm::dvec3{0.0, vert, 0.0};
            }
            // Up = radial direction so the chase view keeps a level horizon planet-wide.
            m_lastEye = eye;
            ctrl.setPose(eye, glm::vec3(target - eye), radialUp(eye, m_planetRadiusM));
        }
        break;
    case CameraMode::Cockpit:
        if (player) {
            // Locked inside the entity, looking along its forward axis (+ RMB look offset). The
            // origin is the ground-contact point, so raise the eye to the body centre.
            glm::dvec3 eye =
                player->position + glm::dvec3(player->velocity * (m_renderAlpha * m_serverTickRate.dtSeconds())) +
                glm::dvec3(player->orientation * glm::vec3{0.f, static_cast<float>(kEntityCentreHeightM), 0.f});
            if (!m_firstFrame && (mb & SDL_BUTTON_RMASK)) {
                m_cockpitYaw -= (mx - m_lastMx) * 0.35f;
                m_cockpitPitch += (my - m_lastMy) * 0.25f;
                m_cockpitPitch = std::clamp(m_cockpitPitch, -80.0f, 80.0f);
            }
            // Head tracking (#927): compose the head pose additively with the RMB look. Head roll tilts
            // the camera up (about the body forward); a body-frame eye offset lets the pilot lean.
            const bool headFresh = m_headPose && m_headPose->fresh;
            const float hYaw = headFresh ? glm::degrees(m_headPose->yawRad) : 0.f;
            const float hPitch = headFresh ? glm::degrees(m_headPose->pitchRad) : 0.f;
            const float hRoll = headFresh ? m_headPose->rollRad : 0.f;
            if (headFresh)
                eye += glm::dvec3(player->orientation * m_headPose->offsetM);
            // Keyboard / d-pad look pan (#689) — composes additively with the RMB drag above. Gated on
            // the console being closed, matching the free-fly movement cluster.
            if (!consoleOpen && m_bindings) {
                constexpr float kPanDegPerS = 90.f;
                const float panStep = kPanDegPerS * dt;
                if (fl::actionDown(sources, *m_bindings, fl::InputAction::ViewLeft))
                    m_cockpitYaw += panStep;
                if (fl::actionDown(sources, *m_bindings, fl::InputAction::ViewRight))
                    m_cockpitYaw -= panStep;
                if (fl::actionDown(sources, *m_bindings, fl::InputAction::ViewUp))
                    m_cockpitPitch += panStep;
                if (fl::actionDown(sources, *m_bindings, fl::InputAction::ViewDown))
                    m_cockpitPitch -= panStep;
                m_cockpitPitch = std::clamp(m_cockpitPitch, -80.0f, 80.0f);
            }
            const float lookPitch = std::clamp(m_cockpitPitch + hPitch, -80.0f, 80.0f);
            const glm::quat lookRot = glm::angleAxis(glm::radians(m_cockpitYaw + hYaw), glm::vec3{0.f, 1.f, 0.f}) *
                                      glm::angleAxis(glm::radians(lookPitch), glm::vec3{0.f, 0.f, 1.f});
            const glm::vec3 forward = player->orientation * lookRot * glm::vec3{1.f, 0.f, 0.f};
            // Head roll tilts the view up about the body forward axis (0 = the unchanged body-up).
            const glm::vec3 up =
                player->orientation * glm::angleAxis(hRoll, glm::vec3{1.f, 0.f, 0.f}) * glm::vec3{0.f, 1.f, 0.f};
            ctrl.setPose(eye, forward, up);
            m_lastForward = forward; // seed for a subsequent padlock entry (#697)
            m_lastUp = up;
        }
        break;
    case CameraMode::Padlock:
        if (player) {
            // Eye identical to Cockpit; forward/up come from the padlock tracker slewing to the target.
            const glm::dvec3 eye =
                player->position + glm::dvec3(player->velocity * (m_renderAlpha * m_serverTickRate.dtSeconds())) +
                glm::dvec3(player->orientation * glm::vec3{0.f, static_cast<float>(kEntityCentreHeightM), 0.f});
            const glm::vec3 worldUp = radialUp(eye, m_planetRadiusM);
            if (!m_padlockTarget) {
                // No target: hold the airframe forward; FlightScreen reverts to Cockpit.
                const glm::vec3 fwd = player->orientation * glm::vec3{1.f, 0.f, 0.f};
                ctrl.setPose(eye, fwd, worldUp);
                m_lastForward = fwd;
                m_lastUp = worldUp;
                m_lastEye = eye;
                break;
            }
            const glm::dvec3 tgt =
                m_padlockTarget->position +
                glm::dvec3(m_padlockTarget->velocity * (m_renderAlpha * m_serverTickRate.dtSeconds()));

            // Terrain LOS latched at ~15 Hz — a full segment march every 60 Hz frame is wasteful.
            m_losAccumS += dt;
            if (m_losAccumS >= 1.0f / 15.0f) {
                m_losAccumS = 0.f;
                auto hfn = [&](double x, double y, double z) {
                    return static_cast<double>(terrain.heightAt(glm::dvec3{x, y, z}));
                };
                auto rfn = [&](double x, double y, double z) { return terrain.heightReadyAt(glm::dvec3{x, y, z}); };
                const double a[3] = {eye.x, eye.y, eye.z};
                const double b[3] = {tgt.x, tgt.y, tgt.z};
                m_latchedLos = terrainLos(a, b, hfn, rfn, m_planetRadiusM);
            }

            PadlockInputs pin;
            pin.dt = dt;
            pin.ownPos = eye;
            pin.ownOrient = player->orientation;
            pin.targetPos = tgt;
            pin.terrainLos = m_latchedLos; // Unknown treated as Clear inside the tracker
            pin.worldUp = worldUp;
            const PadlockPose pose = m_padlock.update(pin);
            if (pose.exitToCockpit) {
                ctrl.setMode(fl::CameraMode::Cockpit);
                onModeSwitch(fl::CameraMode::Cockpit, player);
            } else {
                ctrl.setPose(eye, pose.forward, pose.up);
            }
            m_lastForward = pose.forward;
            m_lastUp = pose.up;
            m_lastEye = eye;
        }
        break;
    }

    m_lastMx = mx;
    m_lastMy = my;
    m_firstFrame = false;
}

} // namespace fl
