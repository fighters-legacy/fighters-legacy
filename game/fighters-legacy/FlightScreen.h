// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IScreen.h"
#include "RenderTypes.h"

#include "render/RenderSnapshot.h"

#include <array>
#include <cstdint>

class CameraInput;
struct ClientNetEventHandler;
class FlightInputCollector;
class GameConsole;
class HapticController;
class INetwork;
class IJoystick;
class UserConfig;

class SandboxInspector;

namespace fl {
class CameraController;
class IHud;
class SimRenderBridge;
class TerrainStreamer;
class WindshieldRain;
} // namespace fl

struct EnvironmentState;

// All dependencies FlightScreen needs; set by Game before transitioning to Flight.
struct FlightScreenDeps {
    CameraInput* camInput{nullptr};
    FlightInputCollector* flightInput{nullptr};
    fl::CameraController* cameraController{nullptr};
    GameConsole* gameConsole{nullptr};
    HapticController* hapticController{nullptr};
    fl::IHud** activeHud{nullptr}; // pointer to the swappable active HUD ptr
    fl::WindshieldRain* windshieldRain{nullptr};
    fl::SimRenderBridge* renderBridge{nullptr};
    fl::TerrainStreamer* terrainStreamer{nullptr};
    EnvironmentState* env{nullptr};
    INetwork* clientNet{nullptr};
    ClientNetEventHandler* clientNetHandler{nullptr}; // for sendHeartbeatIfNeeded; may be null
    IJoystick* joystick{nullptr};
    UserConfig* userConfig{nullptr};
    SandboxInspector* inspector{nullptr}; // null = no inspector
    uint32_t* assignedEntityIdx{nullptr};
    uint32_t* assignedEntityGen{nullptr};
};

// IScreen for the in-flight state. Handles camera/flight input, HUD update,
// haptics, and inspector check. Returns Screen::Pause on Escape or
// Screen::MainMenu when the inspector signals exit.
class FlightScreen : public IScreen {
  public:
    explicit FlightScreen(FlightScreenDeps deps);

    Screen update(IInput& input, IWindow& window) override;
    std::span<const HudElement> buildElements() override;

  private:
    FlightScreenDeps m_deps;
    const fl::EntityRenderEntry* m_playerEntry{nullptr};
    bool m_weaponFired{false};

    // HUD (max 16) + rain (max 48) + slack
    static constexpr int kMaxElements = 72;
    std::array<HudElement, kMaxElements> m_elements{};
    int m_elementCount{0};
};
