// SPDX-License-Identifier: GPL-3.0-or-later
#include "DebriefScreen.h"
#include "render/HudBuilder.h" // hudFullscreenBg (#1261)

#include "IInput.h"
#include "IWindow.h"

namespace fl {

void DebriefScreen::setStats(int kills, int losses, bool missionSuccess) {
    m_kills = kills;
    m_losses = losses;
    m_success = missionSuccess;
}

void DebriefScreen::setMatchResult(std::string winnerText, std::vector<std::pair<std::string, int>> teamScores) {
    m_winner = std::move(winnerText);
    m_teamScores = std::move(teamScores);
    m_hasMatchResult = !m_teamScores.empty();
}

Screen DebriefScreen::update(IInput& input, IWindow& /*window*/, float /*frameDtS*/) {
    if (input.isKeyJustPressed(Key::Enter) || input.isKeyJustPressed(Key::Space) ||
        input.isKeyJustPressed(Key::Escape) || input.isMouseButtonJustPressed(MouseButton::Left) ||
        input.isGamepadButtonJustPressed(0, GamepadButton::A) || input.isGamepadButtonJustPressed(0, GamepadButton::B))
        return Screen::MainMenu;

    return Screen::Debrief;
}

std::span<const HudElement> DebriefScreen::buildElements() {
    m_elementCount = 0;
    int si = 0;

    // Background
    {
        m_elements[static_cast<std::size_t>(m_elementCount++)] = hudFullscreenBg();
    }

    // A center-aligned text line at (0.5, y). Advances si + m_elementCount together.
    auto addText = [&](const std::string& text, float y, float scale, float r, float g, float b) {
        m_strings[static_cast<std::size_t>(si)] = text;
        auto& el = m_elements[static_cast<std::size_t>(m_elementCount++)];
        el = HudElement{};
        el.type = HudElement::Type::Text;
        el.text = m_strings[static_cast<std::size_t>(si++)];
        el.x = 0.5f;
        el.align = HudAlign::Center;
        el.y = y;
        el.scale = scale;
        el.r = r;
        el.g = g;
        el.b = b;
        el.a = 1.f;
    };

    // Outcome title
    addText(m_success ? "MISSION COMPLETE" : "MISSION FAILED", m_hasMatchResult ? 0.14f : 0.20f, 1.8f,
            m_success ? 0.2f : 1.0f, m_success ? 1.0f : 0.2f, 0.2f);

    // Multiplayer match result (#647): winner banner + per-team scores, above the personal tallies.
    float y = 0.40f; // where the personal tallies start (single-player)
    if (m_hasMatchResult) {
        addText(m_winner.empty() ? "MATCH OVER" : m_winner, 0.24f, 1.3f, 1.0f, 0.85f, 0.3f);
        float ty = 0.31f;
        for (const auto& [name, score] : m_teamScores) {
            addText(name + ": " + std::to_string(score), ty, 1.f, 0.9f, 0.9f, 0.9f);
            ty += 0.045f;
        }
        y = ty + 0.03f;
    }

    // Personal tallies
    addText("Kills: " + std::to_string(m_kills), y, 1.f, 0.9f, 0.9f, 0.9f);
    addText("Losses: " + std::to_string(m_losses), y + 0.06f, 1.f, 0.9f, 0.9f, 0.9f);
    addText("[ Continue ]", 0.82f, 1.f, 0.2f, 1.f, 0.2f);

    return {m_elements.data(), static_cast<std::size_t>(m_elementCount)};
}

} // namespace fl