// SPDX-License-Identifier: GPL-3.0-or-later
#include "LoadingScreen.h"

#include "IInput.h"
#include "IWindow.h"

#include <chrono>

LoadingScreen::LoadingScreen(std::atomic<bool>& serverReady, std::function<bool()> isConnected,
                             std::function<void()> onServerReady)
    : m_serverReady(serverReady), m_isConnected(std::move(isConnected)), m_onServerReady(std::move(onServerReady)) {
    addLine("Loading...");
    addLine("Starting local server...");
}

void LoadingScreen::reset() {
    m_phase = Phase::StartingServer;
    m_onServerReadyCalled = false;
    m_lineCount = 0;
    m_elementCount = 0;
    addLine("Loading...");
    addLine("Starting local server...");
}

void LoadingScreen::addLine(const char* text) {
    if (m_lineCount < kMaxLines)
        m_lines[static_cast<std::size_t>(m_lineCount++)] = text;
}

Screen LoadingScreen::update(IInput& /*input*/, IWindow& /*window*/) {
    switch (m_phase) {
    case Phase::StartingServer:
        if (m_serverReady.load(std::memory_order_relaxed)) {
            if (!m_onServerReadyCalled) {
                m_onServerReadyCalled = true;
                m_onServerReady();
                addLine("Connecting...");
            }
            m_phase = Phase::Connecting;
        }
        break;

    case Phase::Connecting:
        if (m_isConnected()) {
            addLine("Ready.");
            m_phase = Phase::Ready;
        }
        break;

    case Phase::Ready:
        return Screen::Flight;

    case Phase::Failed: {
        using namespace std::chrono;
        auto elapsed = duration_cast<duration<float>>(steady_clock::now() - m_failedAt).count();
        if (elapsed >= kFailDisplaySeconds)
            return Screen::MainMenu;
        break;
    }
    }
    return Screen::Loading;
}

std::span<const HudElement> LoadingScreen::buildElements() {
    m_elementCount = 0;

    // Background
    {
        auto& el = m_elements[static_cast<std::size_t>(m_elementCount++)];
        el = HudElement{};
        el.type = HudElement::Type::Rect;
        el.x = 0.f;
        el.y = 0.f;
        el.x2 = 1.f;
        el.y2 = 1.f;
        el.r = 0.f;
        el.g = 0.f;
        el.b = 0.f;
        el.a = 1.f;
    }

    // Progress lines
    for (int i = 0; i < m_lineCount && m_elementCount < kMaxElements; ++i) {
        auto& el = m_elements[static_cast<std::size_t>(m_elementCount++)];
        el = HudElement{};
        el.type = HudElement::Type::Text;
        el.text = m_lines[static_cast<std::size_t>(i)];
        el.x = 0.1f;
        el.y = 0.4f + static_cast<float>(i) * 0.06f;
        el.scale = 1.f;
        el.r = 0.9f;
        el.g = 0.9f;
        el.b = 0.9f;
        el.a = 1.f;
    }

    return {m_elements.data(), static_cast<std::size_t>(m_elementCount)};
}
