// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IScreen.h"
#include "SessionStatus.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string_view>

class AssetManager;
class IDisplay;
class IInput;
class ILogger;
class IRenderer;
class IWindow;
class UserConfig;

// Forward-declare all screen types to keep this header lean.
class MainMenuScreen;
class LoadingScreen;
class MissionSelectScreen;
class MissionBriefScreen;
class SettingsScreen;
class FlightScreen;
class PauseMenuScreen;
class DebriefScreen;

struct FlightScreenDeps;

// Owns all IScreen instances and drives screen transitions.
// Handles mouse-capture and server-pause side effects on transition().
// Music state and session lifecycle remain in Game.cpp's handleTransition().
class ScreenManager {
  public:
    ScreenManager(IInput& input, ILogger& log);
    ~ScreenManager();

    // Create all screen instances. Must be called once after platform is ready.
    // LoadingScreen and FlightScreen are created lazily via reinitLoading/reinitFlight.
    // isMultiplayer relabels "Sandbox (Instant Action)" → "Join Server" in the main menu.
    void init(UserConfig& config, IRenderer& renderer, IWindow& window, IDisplay& display, AssetManager& assets,
              bool isMultiplayer = false);

    // (Re)create the loading screen with fresh callbacks. Called by Game::startGame()
    // each time a new session begins.
    // isSinglePlayer controls initial status text and whether a server-start phase is shown.
    // sessionFailure is an optional atomic written (first-writer-wins) by the server thread and the
    // ENet client handler with a typed SessionFailure; the LoadingScreen polls it and surfaces the
    // message via sessionFailureMessage(). nullptr (default) = no external failure signalling.
    void reinitLoading(std::atomic<bool>& serverReady, std::function<bool()> isConnected,
                       std::function<void()> onConnect, bool isSinglePlayer = true,
                       std::atomic<SessionFailure>* sessionFailure = nullptr);

    // (Re)create the flight screen with fresh session deps. Called by Game::startGame()
    // after session objects (clientNet, hapticController, etc.) are created.
    void reinitFlight(FlightScreenDeps deps);

    Screen current() const {
        return m_current;
    }
    IScreen& active();

    // Transition to a new screen, firing mouse-capture and server-pause side effects.
    void transition(Screen next);

    // Set the admin command sender for server-pause support (single-player only).
    // Call after the local server starts and the session token is known.
    // Pass nullptr to clear (multiplayer or main menu state).
    void setServerCmd(std::function<void(std::string_view)> fn);

    // Set the return target for SettingsScreen so "Back" knows where to go.
    void setSettingsReturnTarget(Screen target);

    MainMenuScreen& mainMenu();
    LoadingScreen& loading();
    MissionSelectScreen& missionSelect();
    MissionBriefScreen& missionBrief();
    SettingsScreen& settings();
    FlightScreen& flight();
    PauseMenuScreen& pauseMenu();
    DebriefScreen& debrief();

  private:
    Screen m_current{Screen::MainMenu};
    IInput& m_input;
    ILogger& m_log;
    std::function<void(std::string_view)> m_serverCmd;

    std::unique_ptr<MainMenuScreen> m_mainMenu;
    std::unique_ptr<LoadingScreen> m_loading;
    std::unique_ptr<MissionSelectScreen> m_missionSelect;
    std::unique_ptr<MissionBriefScreen> m_missionBrief;
    std::unique_ptr<SettingsScreen> m_settings;
    std::unique_ptr<FlightScreen> m_flight;
    std::unique_ptr<PauseMenuScreen> m_pauseMenu;
    std::unique_ptr<DebriefScreen> m_debrief;
};
