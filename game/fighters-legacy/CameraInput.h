// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <glm/glm.hpp>

namespace fl {

class GameConsole;
class IInput;

class CameraController;
class TerrainStreamer;
struct EntityRenderEntry;
enum class CameraMode : uint8_t;

// Translates SDL keyboard/mouse input into a camera pose each frame.
//
// There is one underlying free-fly camera; the modes are constrained ways of driving it:
//   Free    (F4) — the user moves the eye (WASD/QE) and turns the view (mouse); clamped above the
//                  terrain so it cannot pass through the ground.
//   Chase   (F2) — the pose is computed to sit behind the entity and follow its heading.
//   Cockpit (F1) — the pose is locked to the entity (eye at the entity, looking along its forward
//                  axis); the ownship model is hidden by the renderer in this view.
// Every mode resolves to a single CameraController::setPose() call.
class CameraInput {
  public:
    // Detect F1/F2/F4 camera mode switches and backtick console toggle.
    // Call once per frame before update().
    void pollModeKeys(CameraController& ctrl, GameConsole& console, IInput& input, const EntityRenderEntry* player);

    // Compute and apply the camera pose for the current mode from SDL keyboard/mouse state.
    // console is queried to suppress camera movement when the console is open.
    // terrain is used to keep the free-fly camera above the ground.
    void update(CameraController& ctrl,
                const EntityRenderEntry* player, // nullptr = no snapshot yet
                const GameConsole& console, TerrainStreamer& terrain);

    // Persistent throttle [0,1] shared between camera and flight input.
    float throttle() const {
        return m_throttle;
    }
    void adjustThrottle(float delta); // clamped to [0,1]
    void setThrottle(float t);

    // Set the render-interpolation alpha for this frame. Call before update() so Cockpit/Chase
    // extrapolate the entity position by the same amount that SceneRenderer extrapolates it.
    void setRenderAlpha(float alpha) noexcept {
        m_renderAlpha = alpha;
    }

    // Reset per-session state so the free-fly camera re-initialises relative to the player entity
    // on the first frame of a new session. Call at the start of each session.
    void startSession() noexcept;

  private:
    // Reset per-mode state when the user switches camera modes.
    void onModeSwitch(CameraMode newMode, const EntityRenderEntry* player);

    // Place the free-fly eye behind and above the player, looking at it (used on entering Free).
    void initFlyFromPlayer(const EntityRenderEntry& player);

    // Free-fly camera state (the base camera).
    glm::dvec3 m_flyEye{0.0, 2000.0, 0.0};
    float m_flyYaw{0.f};
    float m_flyPitch{0.f};
    float m_flySpeed{30.f};    // metres per SECOND (frame-rate independent); adjustable with +/-
    bool m_needsFlyInit{true}; // re-seat the fly camera on the player on the next valid frame

    // Frame-time tracking for frame-rate-independent fly movement.
    std::chrono::steady_clock::time_point m_lastUpdate{};
    bool m_haveLastUpdate{false};

    // Chase follow state.
    float m_chasePitch{8.f}; // degrees above the entity; eye trails behind and slightly up
    float m_chaseDistance{25.f};

    // Cockpit look offsets (RMB drag).
    float m_cockpitYaw{0.f};
    float m_cockpitPitch{0.f};

    // Persistent throttle (shared with FlightInputCollector).
    float m_throttle{0.0f};

    // Mouse tracking.
    float m_lastMx{0.f};
    float m_lastMy{0.f};
    bool m_firstFrame{true};

    // Render alpha — set by Game.cpp via setRenderAlpha() before update() each frame.
    float m_renderAlpha{0.f};

    // Mode-key edge detection.
    bool m_f1Prev{false};
    bool m_f2Prev{false};
    bool m_f4Prev{false};
    bool m_gravePrev{false};
};

} // namespace fl
