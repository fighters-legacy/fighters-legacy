// SPDX-License-Identifier: GPL-3.0-or-later
#include "DebriefScreen.h"

#include "IInput.h"
#include "IWindow.h"

void DebriefScreen::setStats(int kills, int losses, bool missionSuccess) {
    m_kills = kills;
    m_losses = losses;
    m_success = missionSuccess;
}

Screen DebriefScreen::update(IInput& input, IWindow& /*window*/) {
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

    // Outcome title
    m_strings[static_cast<std::size_t>(si)] = m_success ? "MISSION COMPLETE" : "MISSION FAILED";
    {
        auto& el = m_elements[static_cast<std::size_t>(m_elementCount++)];
        el = HudElement{};
        el.type = HudElement::Type::Text;
        el.text = m_strings[static_cast<std::size_t>(si++)];
        el.x = 0.5f;
        el.y = 0.20f;
        el.scale = 1.8f;
        el.r = m_success ? 0.2f : 1.0f;
        el.g = m_success ? 1.0f : 0.2f;
        el.b = 0.2f;
        el.a = 1.f;
    }

    // Kills
    m_strings[static_cast<std::size_t>(si)] = "Kills: " + std::to_string(m_kills);
    {
        auto& el = m_elements[static_cast<std::size_t>(m_elementCount++)];
        el = HudElement{};
        el.type = HudElement::Type::Text;
        el.text = m_strings[static_cast<std::size_t>(si++)];
        el.x = 0.5f;
        el.y = 0.40f;
        el.r = 0.9f;
        el.g = 0.9f;
        el.b = 0.9f;
        el.a = 1.f;
    }

    // Losses
    m_strings[static_cast<std::size_t>(si)] = "Losses: " + std::to_string(m_losses);
    {
        auto& el = m_elements[static_cast<std::size_t>(m_elementCount++)];
        el = HudElement{};
        el.type = HudElement::Type::Text;
        el.text = m_strings[static_cast<std::size_t>(si++)];
        el.x = 0.5f;
        el.y = 0.48f;
        el.r = 0.9f;
        el.g = 0.9f;
        el.b = 0.9f;
        el.a = 1.f;
    }

    // Continue
    m_strings[static_cast<std::size_t>(si)] = "[ Continue ]";
    {
        auto& el = m_elements[static_cast<std::size_t>(m_elementCount++)];
        el = HudElement{};
        el.type = HudElement::Type::Text;
        el.text = m_strings[static_cast<std::size_t>(si++)];
        el.x = 0.5f;
        el.y = 0.70f;
        el.r = 0.2f;
        el.g = 1.f;
        el.b = 0.2f;
        el.a = 1.f;
    }

    return {m_elements.data(), static_cast<std::size_t>(m_elementCount)};
}
