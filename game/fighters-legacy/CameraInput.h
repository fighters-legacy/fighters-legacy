// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "flight/Geodetic.h" // kEarthRadiusM (default planet radius)

#include <chrono>
#include <glm/glm.hpp>

namespace fl {

class GameConsole;
class IInput;
class InputBindings;

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
    // Detect camera mode switches (CameraCockpit/Chase/Free actions, #689) and the backtick console
    // toggle. Call once per frame before update(). Mode keys resolve through InputBindings (rebindable,
    // gamepad-capable) via setBindings(); the console toggle stays a raw scancode (not a bound action).
    void pollModeKeys(CameraController& ctrl, GameConsole& console, IInput& input, const EntityRenderEntry* player);

    // Compute and apply the camera pose for the current mode from SDL keyboard/mouse state.
    // console is queried to suppress camera movement when the console is open.
    // terrain is used to keep the free-fly camera above the ground.
    // input drives the View* cockpit-look pan (#689) — a keyboard/d-pad alternative to RMB drag.
    void update(CameraController& ctrl,
                const EntityRenderEntry* player, // nullptr = no snapshot yet
                const GameConsole& console, TerrainStreamer& terrain, IInput& input);

    // Provide the binding table used to resolve camera-mode + cockpit-pan actions (#689). Not owned;
    // set once on entering Flight. Null = mode switching / pan disabled (defensive).
    void setBindings(const InputBindings* bindings) noexcept {
        m_bindings = bindings;
    }

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

    // Set the planet sphere radius (metres) used to compute the camera "up" as the radial direction
    // from the planet centre, so the horizon stays level far from the world origin (#479). From
    // MsgConnectAck; defaults to Earth radius. Call on entering Flight.
    void setPlanetRadius(double radiusM) noexcept {
        m_planetRadiusM = radiusM;
    }

    // Reset per-session state so the free-fly camera re-initialises relative to the player entity
    // on the first frame of a new session. Call at the start of each session.
    void startSession() noexcept;

    // The camera eye applied on the most recent update(), absolute world metres. The client sends
    // this to the server each frame so interest management can center on an entity-less observer's
    // viewpoint (#858). Retains its previous value on a frame where the pose was not recomputed
    // (Chase/Cockpit with no player entity yet).
    glm::dvec3 eyeWorld() const noexcept {
        return m_lastEye;
    }

    // Seed the free-fly eye directly (used by the observer ghost flow, #859, where there is no
    // player entity for initFlyFromPlayer to key off).
    // Ground-crew scene (#55): while on, the Chase camera slowly ORBITS the parked aircraft on the
    // ramp instead of locking behind the tail — the post-landing external view. FlightScreen drives
    // it from its landed-and-stopped detection; any camera-mode change or takeoff roll clears it.
    void setGroundScene(bool on) noexcept {
        m_groundScene = on;
    }
    [[nodiscard]] bool groundScene() const noexcept {
        return m_groundScene;
    }

    void setFlyEye(const glm::dvec3& eye) noexcept {
        m_flyEye = eye;
        m_lastEye = eye;
        m_needsFlyInit = false;
    }

  private:
    // Reset per-mode state when the user switches camera modes.
    void onModeSwitch(CameraMode newMode, const EntityRenderEntry* player);

    // Place the free-fly eye behind and above the player, looking at it (used on entering Free).
    void initFlyFromPlayer(const EntityRenderEntry& player);

    // Free-fly camera state (the base camera).
    glm::dvec3 m_flyEye{0.0, 2000.0, 0.0};
    glm::dvec3 m_lastEye{0.0, 2000.0, 0.0}; // eye applied on the last update(); reported by eyeWorld() (#858)
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
    bool m_groundScene{false}; // #55: Chase orbits the parked aircraft instead of trailing it

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

    // Planet radius (m) for the radial camera "up"; Earth default until MsgConnectAck arrives.
    double m_planetRadiusM{kEarthRadiusM};

    // Binding table for camera-mode + cockpit-pan actions (#689); not owned, set via setBindings().
    const InputBindings* m_bindings{nullptr};

    // Console-toggle edge detection (the grave key is not a bound action, so it stays raw).
    bool m_gravePrev{false};
};

} // namespace fl
