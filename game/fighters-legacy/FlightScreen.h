// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IScreen.h"
#include "RenderTypes.h"

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
class IHud;
class ManualOverlay;
class SimRenderBridge;
class TerrainStreamer;
class WindshieldRain;
class WingmanMenu;
class CommsMenu;
class ManualOverlay;

struct EnvironmentState;

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
    SandboxInspector* inspector{nullptr};  // null = no inspector
    ClientPrediction* prediction{nullptr}; // null = no prediction
    WingmanMenu* wingmanMenu{nullptr};     // null = no radio menu (#610)
    CommsMenu* commsMenu{nullptr};         // null = no ATC comms menu (#704)
    ManualOverlay* manual{nullptr};        // null = no in-flight aircraft manual (#821)
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

    // Observer entity picker (#860): which live entity a spectator views from, cycled with Num1/Num2.
    EntitySelector m_selector;
    bool m_prevNextTarget{false}; // Num1 edge detector (next entity)
    bool m_prevPrevTarget{false}; // Num2 edge detector (previous entity)
    char m_pickerLabel[96]{};     // "[ F-16C | Blue ]" built each frame; empty = not shown

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

    // HUD (max 16) + rain (max 48) + slack
    // HUD (<=16) + windshield rain (<=48) + the radio menu (<=10) + slack.
    // Sized for the worst case: cockpit HUD + 48 windshield-rain streaks + the radio menu + the
    // in-flight manual (#821), which is a full page of text.
    static constexpr int kMaxElements = 176;
    std::array<HudElement, kMaxElements> m_elements{};
    int m_elementCount{0};
};

} // namespace fl
