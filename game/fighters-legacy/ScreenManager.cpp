// SPDX-License-Identifier: GPL-3.0-or-later
#include "ScreenManager.h"

#include "DebriefScreen.h"
#include "FlightScreen.h"
#include "IInput.h"
#include "LoadingScreen.h"
#include "MainMenuScreen.h"
#include "MissionBriefScreen.h"
#include "MissionSelectScreen.h"
#include "PauseMenuScreen.h"
#include "SettingsScreen.h"

#include "IDisplay.h"
#include "ILogger.h"
#include "IRenderer.h"
#include "IWindow.h"
#include "config/UserConfig.h"
#include "content/AssetManager.h"

ScreenManager::ScreenManager(IInput& input, ILogger& log) : m_input(input), m_log(log) {}

ScreenManager::~ScreenManager() = default;

void ScreenManager::init(UserConfig& config, IRenderer& renderer, IWindow& window, IDisplay& display,
                         AssetManager& assets, bool isMultiplayer) {
    m_mainMenu = std::make_unique<MainMenuScreen>(assets.hasPacks(), isMultiplayer);
    // LoadingScreen and FlightScreen are created lazily per session
    m_missionSelect = std::make_unique<MissionSelectScreen>(assets.listMissions());
    m_missionBrief = std::make_unique<MissionBriefScreen>();
    m_settings = std::make_unique<SettingsScreen>(config, renderer, window, display);
    m_pauseMenu = std::make_unique<PauseMenuScreen>();
    m_debrief = std::make_unique<DebriefScreen>();
}

void ScreenManager::reinitFlight(FlightScreenDeps deps) {
    m_flight = std::make_unique<FlightScreen>(std::move(deps));
}

void ScreenManager::reinitLoading(std::atomic<bool>& serverReady, std::function<bool()> isConnected,
                                  std::function<void()> onConnect, bool isSinglePlayer,
                                  std::atomic<SessionFailure>* sessionFailure) {
    m_loading = std::make_unique<LoadingScreen>(serverReady, std::move(isConnected), std::move(onConnect),
                                                isSinglePlayer, sessionFailure);
}

IScreen& ScreenManager::active() {
    switch (m_current) {
    case Screen::MainMenu:
        return *m_mainMenu;
    case Screen::Loading:
        return *m_loading;
    case Screen::MissionSelect:
        return *m_missionSelect;
    case Screen::MissionBrief:
        return *m_missionBrief;
    case Screen::Settings:
        return *m_settings;
    case Screen::Flight:
        return *m_flight;
    case Screen::Pause:
        return *m_pauseMenu;
    case Screen::Debrief:
        return *m_debrief;
    case Screen::Quit:
        return *m_mainMenu; // unreachable, but satisfies compiler
    }
    return *m_mainMenu;
}

void ScreenManager::transition(Screen next) {
    const Screen prev = m_current;
    m_current = next;

    // Mouse capture: Flight mode captures, all menu screens release.
    if (next == Screen::Flight)
        m_input.setMouseCapture(true);
    else if (prev == Screen::Flight || prev == Screen::Pause)
        m_input.setMouseCapture(false);

    // Server-side pause (single-player only; m_serverCmd is null in multiplayer).
    if (m_serverCmd) {
        if (next == Screen::Pause)
            m_serverCmd("pause");
        else if (prev == Screen::Pause)
            m_serverCmd("resume");
    }

    // Update SettingsScreen return target so Back goes to the right place.
    if (next == Screen::Settings && m_settings) {
        if (prev == Screen::Pause)
            m_settings->setReturnTarget(Screen::Pause);
        else
            m_settings->setReturnTarget(Screen::MainMenu);
    }

    (void)prev; // transitions are visible through game behaviour; no need to log
}

void ScreenManager::setServerCmd(std::function<void(std::string_view)> fn) {
    m_serverCmd = std::move(fn);
}

void ScreenManager::setSettingsReturnTarget(Screen target) {
    if (m_settings)
        m_settings->setReturnTarget(target);
}

MainMenuScreen& ScreenManager::mainMenu() {
    return *m_mainMenu;
}
LoadingScreen& ScreenManager::loading() {
    return *m_loading;
}
MissionSelectScreen& ScreenManager::missionSelect() {
    return *m_missionSelect;
}
MissionBriefScreen& ScreenManager::missionBrief() {
    return *m_missionBrief;
}
SettingsScreen& ScreenManager::settings() {
    return *m_settings;
}
FlightScreen& ScreenManager::flight() {
    return *m_flight;
}
PauseMenuScreen& ScreenManager::pauseMenu() {
    return *m_pauseMenu;
}
DebriefScreen& ScreenManager::debrief() {
    return *m_debrief;
}
