// SPDX-License-Identifier: GPL-3.0-or-later
#include "MissionSelectScreen.h"

#include "IInput.h"
#include "IWindow.h"

MissionSelectScreen::MissionSelectScreen(std::vector<std::string> missions) : m_missions(std::move(missions)) {}

Screen MissionSelectScreen::update(IInput& input, IWindow& window) {
    const int n = static_cast<int>(m_missions.size());

    if (input.isKeyJustPressed(Key::Escape))
        return Screen::MainMenu;

    if (n == 0)
        return Screen::MissionSelect;

    if (input.isKeyJustPressed(Key::ArrowUp) || input.isKeyJustPressed(Key::W)) {
        if (m_selectedIdx > 0) {
            --m_selectedIdx;
            if (m_selectedIdx < m_scrollOffset)
                m_scrollOffset = m_selectedIdx;
        }
    }
    if (input.isKeyJustPressed(Key::ArrowDown) || input.isKeyJustPressed(Key::S)) {
        if (m_selectedIdx < n - 1) {
            ++m_selectedIdx;
            if (m_selectedIdx >= m_scrollOffset + kVisible)
                m_scrollOffset = m_selectedIdx - kVisible + 1;
        }
    }

    // Gamepad
    if (input.isGamepadButtonJustPressed(0, GamepadButton::DpadUp) && m_selectedIdx > 0) {
        --m_selectedIdx;
        if (m_selectedIdx < m_scrollOffset)
            m_scrollOffset = m_selectedIdx;
    }
    if (input.isGamepadButtonJustPressed(0, GamepadButton::DpadDown) && m_selectedIdx < n - 1) {
        ++m_selectedIdx;
        if (m_selectedIdx >= m_scrollOffset + kVisible)
            m_scrollOffset = m_selectedIdx - kVisible + 1;
    }

    // Mouse hover
    int mx = 0, my = 0;
    input.getMousePosition(mx, my);
    const float fh = static_cast<float>(window.logicalHeight());
    if (fh > 0.f) {
        const float ny = static_cast<float>(my) / fh;
        for (int i = 0; i < kVisible; ++i) {
            int idx = m_scrollOffset + i;
            if (idx >= n)
                break;
            float iy = 0.25f + static_cast<float>(i) * 0.065f;
            if (ny >= iy && ny < iy + 0.055f)
                m_selectedIdx = idx;
        }
    }

    bool confirmed = input.isKeyJustPressed(Key::Enter) || input.isKeyJustPressed(Key::Space) ||
                     input.isMouseButtonJustPressed(MouseButton::Left) ||
                     input.isGamepadButtonJustPressed(0, GamepadButton::A);
    if (confirmed && m_selectedIdx >= 0 && m_selectedIdx < n) {
        m_selected = m_missions[static_cast<std::size_t>(m_selectedIdx)];
        return Screen::MissionBrief;
    }

    return Screen::MissionSelect;
}

std::span<const HudElement> MissionSelectScreen::buildElements() {
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

    // Title
    m_strings[0] = "SELECT MISSION";
    {
        auto& el = m_elements[static_cast<std::size_t>(m_elementCount++)];
        el = HudElement{};
        el.type = HudElement::Type::Text;
        el.text = m_strings[0];
        el.x = 0.5f;
        el.y = 0.1f;
        el.scale = 1.5f;
        el.r = 1.f;
        el.g = 1.f;
        el.b = 1.f;
        el.a = 1.f;
    }

    if (m_missions.empty()) {
        m_strings[1] = "No missions available.";
        auto& el = m_elements[static_cast<std::size_t>(m_elementCount++)];
        el = HudElement{};
        el.type = HudElement::Type::Text;
        el.text = m_strings[1];
        el.x = 0.5f;
        el.y = 0.5f;
        el.r = 0.6f;
        el.g = 0.6f;
        el.b = 0.6f;
        el.a = 1.f;
        return {m_elements.data(), static_cast<std::size_t>(m_elementCount)};
    }

    const int n = static_cast<int>(m_missions.size());
    for (int i = 0; i < kVisible && m_elementCount < kMaxElements; ++i) {
        int idx = m_scrollOffset + i;
        if (idx >= n)
            break;
        int si = i + 2; // first 2 strings used above
        m_strings[static_cast<std::size_t>(si)] = m_missions[static_cast<std::size_t>(idx)];
        auto& el = m_elements[static_cast<std::size_t>(m_elementCount++)];
        el = HudElement{};
        el.type = HudElement::Type::Text;
        el.text = m_strings[static_cast<std::size_t>(si)];
        el.x = 0.5f;
        el.y = 0.25f + static_cast<float>(i) * 0.065f;
        el.scale = 1.f;
        if (idx == m_selectedIdx) {
            el.r = 0.2f;
            el.g = 1.f;
            el.b = 0.2f;
            el.a = 1.f;
        } else {
            el.r = 0.8f;
            el.g = 0.8f;
            el.b = 0.8f;
            el.a = 1.f;
        }
    }

    return {m_elements.data(), static_cast<std::size_t>(m_elementCount)};
}
