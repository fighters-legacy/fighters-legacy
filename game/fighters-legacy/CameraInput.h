// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <glm/glm.hpp>

class DebugConsole;

namespace fl {
class CameraController;
class TerrainStreamer;
struct EntityRenderEntry;
enum class CameraMode : uint8_t;
} // namespace fl

// Owns all camera orbit state and translates SDL keyboard/mouse input into
// CameraController updates each frame. Consolidates the 13 previously loose
// variables that lived in game/main.cpp.
class CameraInput {
  public:
    // Update controller based on current SDL keyboard/mouse state.
    // console is queried to suppress camera movement when the console is open.
    void update(fl::CameraController& ctrl,
                const fl::EntityRenderEntry* player, // nullptr = no snapshot yet
                const DebugConsole& console, fl::TerrainStreamer& terrain);

    // Reset per-mode state when the user switches camera modes (F1/F2/F4).
    void onModeSwitch(fl::CameraMode newMode, const fl::EntityRenderEntry* player);

    // Persistent throttle [0,1] shared between camera and flight input.
    float throttle() const {
        return m_sbThrottle;
    }
    void adjustThrottle(float delta); // clamped to [0,1]
    void setThrottle(float t);

    // Initial free-cam pivot (used when the sandbox starts).
    void setInitialPivot(glm::dvec3 pivot) {
        m_sbPivot = pivot;
    }

  private:
    // Free / orbit state
    glm::dvec3 m_sbPivot{0.0, 2000.0, 0.0};
    float m_sbYaw{0.f};
    float m_sbPitch{-10.f};
    float m_sbRadius{200.f};
    float m_sbThrottle{0.f};
    // Chase state
    float m_chaseYaw{180.f};
    float m_chasePitch{20.f};
    float m_chaseRadius{25.f};
    // Cockpit look
    float m_cockpitYaw{0.f};
    float m_cockpitPitch{0.f};
    // Mouse tracking
    float m_lastMx{0.f};
    float m_lastMy{0.f};
    bool m_firstFrame{true};
};
