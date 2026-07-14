// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IScreen.h"
#include "RenderTypes.h"

#include "render/RenderSnapshot.h"

#include <array>
#include <cstdint>

namespace fl {

class CameraInput;
struct ClientNetEventHandler;
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
    IJoystick* joystick{nullptr};
    UserConfig* userConfig{nullptr};
    SandboxInspector* inspector{nullptr};  // null = no inspector
    ClientPrediction* prediction{nullptr}; // null = no prediction
    WingmanMenu* wingmanMenu{nullptr};     // null = no radio menu (#610)
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

    // HUD (max 16) + rain (max 48) + slack
    // HUD (<=16) + windshield rain (<=48) + the radio menu (<=10) + slack.
    // Sized for the worst case: cockpit HUD + 48 windshield-rain streaks + the radio menu + the
    // in-flight manual (#821), which is a full page of text.
    static constexpr int kMaxElements = 176;
    std::array<HudElement, kMaxElements> m_elements{};
    int m_elementCount{0};
};

} // namespace fl
