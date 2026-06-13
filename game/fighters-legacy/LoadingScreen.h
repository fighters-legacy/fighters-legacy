// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IScreen.h"
#include "RenderTypes.h"

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <string>

// Async loading screen shown while the local server starts and ENet connects.
// Constructor takes three callables injected by Game::startGame():
//   serverReady    — atomic set by the background server thread on success
//   isConnected    — returns true once renderBridge.hasSnapshot()
//   onServerReady  — called once when serverReady fires; creates clientHandler and
//                    calls clientNet->connect()
//   isSinglePlayer — controls initial status text and whether the StartingServer
//                    phase is shown; pass false for remote-connect sessions
class LoadingScreen : public IScreen {
  public:
    LoadingScreen(std::atomic<bool>& serverReady, std::function<bool()> isConnected,
                  std::function<void()> onServerReady, bool isSinglePlayer = true);

    Screen update(IInput& input, IWindow& window) override;
    std::span<const HudElement> buildElements() override;

    // Reset to initial state for a new session.
    void reset();

    void setClockOverride(std::function<std::chrono::steady_clock::time_point()> fn);

  private:
    enum class Phase { StartingServer, Connecting, Ready, Failed };

    std::atomic<bool>& m_serverReady;
    std::function<bool()> m_isConnected;
    std::function<void()> m_onServerReady;
    bool m_isSinglePlayer;

    Phase m_phase{Phase::StartingServer};
    bool m_onServerReadyCalled{false};
    std::chrono::steady_clock::time_point m_failedAt{};
    std::chrono::steady_clock::time_point m_connectDeadline{};
    std::chrono::steady_clock::time_point m_startDeadline{};
    static constexpr float kFailDisplaySeconds = 3.f;
    static constexpr float kConnectTimeoutSeconds = 10.f;
    static constexpr float kStartTimeoutSeconds = 10.f;
    std::function<std::chrono::steady_clock::time_point()> m_now{std::chrono::steady_clock::now};

    static constexpr int kMaxLines = 6;
    static constexpr int kMaxElements = kMaxLines + 1; // + background rect
    std::array<std::string, kMaxLines> m_lines{};
    int m_lineCount{0};
    std::array<HudElement, kMaxElements> m_elements{};
    int m_elementCount{0};

    void addLine(const char* text);
    void buildHudElements();
};
