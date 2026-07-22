// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IScreen.h"
#include "RenderTypes.h"

#include "Autopilot.h"      // player autopilot holds (#640)
#include "CrewSeatMenu.h"   // crew seat picker (#975)
#include "EntitySelector.h" // observer entity picker (#860)
#include "render/RenderSnapshot.h"

#include <array>
#include <cstdint>

namespace fl {

class CameraInput;
struct ClientNetEventHandler;
class EntityTypeRegistry;
class FlightInputCollector;
class GameConsole;
class HapticController;
class INetwork;
class IJoystick;
class UserConfig;

class SandboxInspector;

class CameraController;
class ClientPrediction;
class SceneRenderer;
class IHud;
class ManualOverlay;
class SimRenderBridge;
class TerrainStreamer;
class WindshieldRain;
class WingmanMenu;
class CommsMenu;
class ManualOverlay;
class ChatOverlay;
class IGui;

struct EnvironmentState;
class InputBindings;
class TargetDesignation;

// All dependencies FlightScreen needs; set by Game before transitioning to Flight.
struct FlightScreenDeps {
    CameraInput* camInput{nullptr};
    FlightInputCollector* flightInput{nullptr};
    CameraController* cameraController{nullptr};
    GameConsole* gameConsole{nullptr};
    HapticController* hapticController{nullptr};
    IHud** activeHud{nullptr}; // pointer to the swappable active HUD ptr
    WindshieldRain* windshieldRain{nullptr};
    SimRenderBridge* renderBridge{nullptr};
    TerrainStreamer* terrainStreamer{nullptr};
    EnvironmentState* env{nullptr};
    INetwork* clientNet{nullptr};
    ClientNetEventHandler* clientNetHandler{nullptr}; // for sendHeartbeatIfNeeded; may be null
    EntityTypeRegistry* entityRegistry{nullptr};      // for the observer picker's type-name label (#860)
    IJoystick* joystick{nullptr};
    UserConfig* userConfig{nullptr};
    SandboxInspector* inspector{nullptr};          // null = no inspector
    ClientPrediction* prediction{nullptr};         // null = no prediction
    const InputBindings* inputBindings{nullptr};   // for edge-detecting autopilot/target actions (#640/#696)
    TargetDesignation* targetDesignation{nullptr}; // client-side designated target (#696); null = disabled
    SceneRenderer* sceneRenderer{nullptr};         // for the target-slaved inset view (#698); null = disabled
    WingmanMenu* wingmanMenu{nullptr};             // null = no radio menu (#610)
    CommsMenu* commsMenu{nullptr};                 // null = no ATC comms menu (#704)
    ManualOverlay* manual{nullptr};                // null = no in-flight aircraft manual (#821)
    ChatOverlay* chat{nullptr};                    // null = no in-match chat (#646)
    IGui* gui{nullptr};                            // null = no GUI backend (chat input box degrades off)
    uint32_t* assignedEntityIdx{nullptr};
    uint32_t* assignedEntityGen{nullptr};
};

// IScreen for the in-flight state. Handles camera/flight input, HUD update,
// haptics, and inspector check. Returns Screen::Pause on Escape or
// Screen::MainMenu when the inspector signals exit.
class FlightScreen : public IScreen {
  public:
    explicit FlightScreen(FlightScreenDeps deps);
    ~FlightScreen() override;

    Screen update(IInput& input, IWindow& window) override;
    std::span<const HudElement> buildElements() override;

  private:
    FlightScreenDeps m_deps;
    const EntityRenderEntry* m_playerEntry{nullptr};
    bool m_weaponFired{false};

    // The frame's camera view (#438 D1 seam). Computed in update() right after CameraInput sets the
    // pose, cached so buildElements()-time world->screen cues (#696/#698) project against the same
    // matrix the HUD used.
    CameraView m_frameCam{};

    // Player autopilot (#640): altitude / heading / speed hold shaped over the client input before it
    // is sent. Reset at session start; disengaged when there is no predicted ownship state.
    Autopilot m_autopilot;
    float m_lastRawThrottle{0.0f}; // to detect a throttle touch (disengages speed hold)

    // The resolved designated target this frame (#696), snapshot-lifetime — copy fields, never retain.
    // Used by the designator cue in buildElements(). Null = no valid designation.
    const EntityRenderEntry* m_designatedTarget{nullptr};
    char m_tgtLabel[64]{}; // "TGT F-16C  4.2 km" rebuilt each frame (HudElement::text is non-owning)

    // Target-slaved inset view (#698): toggled by TargetInsetToggle; the border rect is remembered so
    // buildElements() frames the live 3D inset the renderer draws.
    bool m_insetOn{false};
    glm::vec4 m_insetRect{0.0f};
    bool m_insetActive{false}; // inset shown this frame (on + a target resolved)

    // Ground-crew scene (#55): landed-and-stopped detection on the OWN aircraft. The airborne→landed
    // edge records a landing score into the logbook (the #674 sink that had no producer); holding
    // landed-and-stopped for a couple of seconds blends the Chase camera into a slow ramp orbit,
    // which clears on the takeoff roll, a manual camera-mode change, or the scene timeout.
    bool m_wasAirborne{false};
    float m_prevVertSpeedMps{0.f}; // last frame's vertical speed — the touchdown sink rate
    double m_landedStillS{0.0};    // consecutive seconds landed and stopped
    bool m_groundSceneOn{false};

    // Observer entity picker (#860): which live entity a spectator views from, cycled with Num1/Num2.
    // #403 extends "spectator" to a dead pilot awaiting respawn, not just a role-observer.
    EntitySelector m_selector;
    bool m_prevNextTarget{false}; // Num1 edge detector (next entity)
    bool m_prevPrevTarget{false}; // Num2 edge detector (previous entity)
    char m_pickerLabel[96]{};     // "[ F-16C | Blue ]" built each frame; empty = not shown
    bool m_wasSpectating{false};  // spectate rising-edge detector (#403)
    glm::dvec3 m_lastOwnPos{};    // last known own-aircraft position, seeds the dead-pilot ghost eye (#403)

    // Crew seat picker (#975): K cycles joinable seats across all crewed aircraft, J joins the selected
    // seat, L leaves the current seat. Non-modal (axes stay live), like the radio menu. The overlay +
    // last-result line render in buildElements when active.
    CrewSeatPicker m_seatPicker;
    bool m_seatPickerActive{false};
    bool m_prevSeatCycle{false}; // K edge
    bool m_prevSeatJoin{false};  // J edge
    bool m_prevSeatLeave{false}; // L edge
    char m_seatResultLine[80]{}; // last MsgSeatResult, surfaced to the player
    char m_seatPickerLine[96]{}; // the picker's current selection, rebuilt each frame (HUD text is non-owning)

    // Sized for the worst case: the redesigned cockpit HUD (<=320 elements, #438) + 48 windshield-rain
    // streaks + the target designator box (#696) + the radio/comms menu + the in-flight manual (#821),
    // which is a full page of text. Must stay >= the FlightHud element cap or HUD symbology is truncated.
    static constexpr int kMaxElements = 480;
    std::array<HudElement, kMaxElements> m_elements{};
    int m_elementCount{0};
};

} // namespace fl
