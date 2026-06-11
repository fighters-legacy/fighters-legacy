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
//   serverReady   — atomic set by the background server thread on success
//   isConnected   — returns true once renderBridge.hasSnapshot()
//   onServerReady — called once when serverReady fires; creates clientHandler and
//                   calls clientNet->connect()
class LoadingScreen : public IScreen {
  public:
    LoadingScreen(std::atomic<bool>& serverReady, std::function<bool()> isConnected,
                  std::function<void()> onServerReady);

    Screen update(IInput& input, IWindow& window) override;
    std::span<const HudElement> buildElements() override;

    // Reset to initial state for a new session.
    void reset();

  private:
    enum class Phase { StartingServer, Connecting, Ready, Failed };

    std::atomic<bool>& m_serverReady;
    std::function<bool()> m_isConnected;
    std::function<void()> m_onServerReady;

    Phase m_phase{Phase::StartingServer};
    bool m_onServerReadyCalled{false};
    std::chrono::steady_clock::time_point m_failedAt{};
    static constexpr float kFailDisplaySeconds = 3.f;

    static constexpr int kMaxLines = 6;
    static constexpr int kMaxElements = kMaxLines + 1; // + background rect
    std::array<std::string, kMaxLines> m_lines{};
    int m_lineCount{0};
    std::array<HudElement, kMaxElements> m_elements{};
    int m_elementCount{0};

    void addLine(const char* text);
    void buildHudElements();
};
