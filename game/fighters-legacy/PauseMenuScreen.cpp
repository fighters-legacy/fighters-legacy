// SPDX-License-Identifier: GPL-3.0-or-later
#include "PauseMenuScreen.h"

#include "IInput.h"
#include "IWindow.h"

namespace fl {

// Items: 0=Resume 1=Settings 2=Quit to Menu 3=Exit to Desktop
static constexpr std::pair<const char*, Screen> kItems[4] = {
    {"Resume", Screen::Flight},
    {"Settings", Screen::Settings},
    {"Quit to Menu", Screen::MainMenu},
    {"Exit to Desktop", Screen::Quit},
};

Screen PauseMenuScreen::update(IInput& input, IWindow& window) {
    if (input.isKeyJustPressed(Key::Escape) || input.isGamepadButtonJustPressed(0, GamepadButton::B))
        return Screen::Flight; // Resume

    if (input.isKeyJustPressed(Key::ArrowUp) || input.isKeyJustPressed(Key::W) ||
        input.isGamepadButtonJustPressed(0, GamepadButton::DpadUp))
        m_selectedIdx = (m_selectedIdx - 1 + kItemCount) % kItemCount;

    if (input.isKeyJustPressed(Key::ArrowDown) || input.isKeyJustPressed(Key::S) ||
        input.isGamepadButtonJustPressed(0, GamepadButton::DpadDown))
        m_selectedIdx = (m_selectedIdx + 1) % kItemCount;

    // Mouse hover
    int mx = 0, my = 0;
    input.getMousePosition(mx, my);
    const float fh = static_cast<float>(window.logicalHeight());
    if (fh > 0.f) {
        const float ny = static_cast<float>(my) / fh;
        for (int i = 0; i < kItemCount; ++i) {
            float iy = 0.35f + static_cast<float>(i) * 0.09f;
            if (ny >= iy && ny < iy + 0.07f)
                m_selectedIdx = i;
        }
    }

    const bool confirm = input.isKeyJustPressed(Key::Enter) || input.isKeyJustPressed(Key::Space) ||
                         input.isMouseButtonJustPressed(MouseButton::Left) ||
                         input.isGamepadButtonJustPressed(0, GamepadButton::A);
    if (confirm)
        return kItems[static_cast<std::size_t>(m_selectedIdx)].second;

    return Screen::Pause;
}

std::span<const HudElement> PauseMenuScreen::buildElements() {
    m_elementCount = 0;

    // Semi-transparent overlay
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
    m_strings[0] = "PAUSED";
    {
        auto& el = m_elements[static_cast<std::size_t>(m_elementCount++)];
        el = HudElement{};
        el.type = HudElement::Type::Text;
        el.text = m_strings[0];
        el.x = 0.5f;
        el.y = 0.28f;
        el.scale = 1.3f;
        el.r = 1.f;
        el.g = 1.f;
        el.b = 0.8f;
        el.a = 1.f;
    }

    // Menu items
    for (int i = 0; i < kItemCount; ++i) {
        int si = i + 1;
        m_strings[static_cast<std::size_t>(si)] = kItems[static_cast<std::size_t>(i)].first;
        auto& el = m_elements[static_cast<std::size_t>(m_elementCount++)];
        el = HudElement{};
        el.type = HudElement::Type::Text;
        el.text = m_strings[static_cast<std::size_t>(si)];
        el.x = 0.5f;
        el.y = 0.35f + static_cast<float>(i) * 0.09f;
        if (i == m_selectedIdx) {
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

} // namespace fl