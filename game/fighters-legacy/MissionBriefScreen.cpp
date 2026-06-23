// SPDX-License-Identifier: GPL-3.0-or-later
#include "MissionBriefScreen.h"

#include "IInput.h"
#include "IWindow.h"

namespace fl {

void MissionBriefScreen::setMission(std::string_view missionId, std::string_view missionName) {
    m_id = missionId;
    m_name = missionName.empty() ? std::string(missionId) : std::string(missionName);
    m_selectedIdx = 0;
}

Screen MissionBriefScreen::update(IInput& input, IWindow& window) {
    if (input.isKeyJustPressed(Key::Escape))
        return Screen::MissionSelect;

    if (input.isKeyJustPressed(Key::ArrowLeft) || input.isKeyJustPressed(Key::A))
        m_selectedIdx = 0;
    if (input.isKeyJustPressed(Key::ArrowRight) || input.isKeyJustPressed(Key::D))
        m_selectedIdx = 1;

    if (input.isGamepadButtonJustPressed(0, GamepadButton::DpadLeft))
        m_selectedIdx = 0;
    if (input.isGamepadButtonJustPressed(0, GamepadButton::DpadRight))
        m_selectedIdx = 1;
    if (input.isGamepadButtonJustPressed(0, GamepadButton::B))
        return Screen::MissionSelect;

    // Mouse hover over the two buttons
    int mx = 0, my = 0;
    input.getMousePosition(mx, my);
    const float fw = static_cast<float>(window.logicalWidth());
    const float fh = static_cast<float>(window.logicalHeight());
    if (fw > 0.f && fh > 0.f) {
        const float nx = static_cast<float>(mx) / fw;
        const float ny = static_cast<float>(my) / fh;
        if (ny >= 0.75f && ny < 0.82f) {
            if (nx >= 0.25f && nx < 0.45f)
                m_selectedIdx = 0; // Fly
            if (nx >= 0.55f && nx < 0.75f)
                m_selectedIdx = 1; // Back
        }
    }

    bool confirmed = input.isKeyJustPressed(Key::Enter) || input.isKeyJustPressed(Key::Space) ||
                     input.isMouseButtonJustPressed(MouseButton::Left) ||
                     input.isGamepadButtonJustPressed(0, GamepadButton::A);
    if (confirmed)
        return (m_selectedIdx == 0) ? Screen::Loading : Screen::MissionSelect;

    return Screen::MissionBrief;
}

std::span<const HudElement> MissionBriefScreen::buildElements() {
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

    // Mission name
    m_strings[0] = m_name.empty() ? "(no mission)" : m_name;
    {
        auto& el = m_elements[static_cast<std::size_t>(m_elementCount++)];
        el = HudElement{};
        el.type = HudElement::Type::Text;
        el.text = m_strings[0];
        el.x = 0.5f;
        el.y = 0.15f;
        el.scale = 1.5f;
        el.r = 1.f;
        el.g = 1.f;
        el.b = 1.f;
        el.a = 1.f;
    }

    // Map placeholder
    m_strings[1] = "Map: " + m_id;
    {
        auto& el = m_elements[static_cast<std::size_t>(m_elementCount++)];
        el = HudElement{};
        el.type = HudElement::Type::Text;
        el.text = m_strings[1];
        el.x = 0.5f;
        el.y = 0.30f;
        el.r = 0.8f;
        el.g = 0.8f;
        el.b = 0.8f;
        el.a = 1.f;
    }

    // Objectives stub
    m_strings[2] = "Objectives: --";
    {
        auto& el = m_elements[static_cast<std::size_t>(m_elementCount++)];
        el = HudElement{};
        el.type = HudElement::Type::Text;
        el.text = m_strings[2];
        el.x = 0.5f;
        el.y = 0.40f;
        el.r = 0.7f;
        el.g = 0.7f;
        el.b = 0.7f;
        el.a = 1.f;
    }

    // Fly button
    m_strings[3] = "[ Fly ]";
    {
        auto& el = m_elements[static_cast<std::size_t>(m_elementCount++)];
        el = HudElement{};
        el.type = HudElement::Type::Text;
        el.text = m_strings[3];
        el.x = 0.35f;
        el.y = 0.77f;
        el.scale = 1.f;
        if (m_selectedIdx == 0) {
            el.r = 0.2f;
            el.g = 1.f;
            el.b = 0.2f;
            el.a = 1.f;
        } else {
            el.r = 0.7f;
            el.g = 0.7f;
            el.b = 0.7f;
            el.a = 1.f;
        }
    }

    // Back button
    m_strings[4] = "[ Back ]";
    {
        auto& el = m_elements[static_cast<std::size_t>(m_elementCount++)];
        el = HudElement{};
        el.type = HudElement::Type::Text;
        el.text = m_strings[4];
        el.x = 0.65f;
        el.y = 0.77f;
        el.scale = 1.f;
        if (m_selectedIdx == 1) {
            el.r = 0.2f;
            el.g = 1.f;
            el.b = 0.2f;
            el.a = 1.f;
        } else {
            el.r = 0.7f;
            el.g = 0.7f;
            el.b = 0.7f;
            el.a = 1.f;
        }
    }

    return {m_elements.data(), static_cast<std::size_t>(m_elementCount)};
}

} // namespace fl