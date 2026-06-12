// SPDX-License-Identifier: GPL-3.0-or-later
#include "MainMenuScreen.h"

#include "IInput.h"
#include "IWindow.h"

static constexpr float kStartY = 0.38f;
static constexpr float kSpacing = 0.07f;
static constexpr float kItemH = 0.05f;

MainMenuScreen::MainMenuScreen(bool hasPacks, bool isMultiplayer) {
    m_items.push_back({isMultiplayer ? "Join Server" : "Sandbox (Instant Action)", Screen::Loading});
    if (hasPacks)
        m_items.push_back({"Select Mission", Screen::MissionSelect});
    m_items.push_back({"Settings", Screen::Settings});
    m_items.push_back({"Exit to Desktop", Screen::Quit});
}

Screen MainMenuScreen::update(IInput& input, IWindow& window) {
    const int n = static_cast<int>(m_items.size());

    // Keyboard navigation
    if (input.isKeyJustPressed(Key::ArrowUp) || input.isKeyJustPressed(Key::W))
        m_selectedIdx = (m_selectedIdx - 1 + n) % n;
    if (input.isKeyJustPressed(Key::ArrowDown) || input.isKeyJustPressed(Key::S))
        m_selectedIdx = (m_selectedIdx + 1) % n;
    if (input.isKeyJustPressed(Key::Escape))
        m_selectedIdx = n - 1;

    // Gamepad navigation (D-pad)
    if (input.isGamepadButtonJustPressed(0, GamepadButton::DpadUp))
        m_selectedIdx = (m_selectedIdx - 1 + n) % n;
    if (input.isGamepadButtonJustPressed(0, GamepadButton::DpadDown))
        m_selectedIdx = (m_selectedIdx + 1) % n;

    // Mouse hover
    int mx = 0, my = 0;
    input.getMousePosition(mx, my);
    const float fh = static_cast<float>(window.logicalHeight());
    if (fh > 0.f) {
        const float ny = static_cast<float>(my) / fh;
        for (int i = 0; i < n; ++i) {
            float iy = kStartY + static_cast<float>(i) * kSpacing;
            if (ny >= iy && ny < iy + kItemH) {
                m_selectedIdx = i;
                break;
            }
        }
    }

    // Confirm
    bool confirmed = input.isKeyJustPressed(Key::Enter) || input.isKeyJustPressed(Key::Space) ||
                     input.isMouseButtonJustPressed(MouseButton::Left) ||
                     input.isGamepadButtonJustPressed(0, GamepadButton::A);
    if (confirmed)
        return confirm();

    return Screen::MainMenu;
}

Screen MainMenuScreen::confirm() {
    if (m_selectedIdx >= 0 && m_selectedIdx < static_cast<int>(m_items.size()))
        return m_items[static_cast<std::size_t>(m_selectedIdx)].target;
    return Screen::MainMenu;
}

void MainMenuScreen::selectNext() {
    const int n = static_cast<int>(m_items.size());
    m_selectedIdx = (m_selectedIdx + 1) % n;
}

void MainMenuScreen::selectPrev() {
    const int n = static_cast<int>(m_items.size());
    m_selectedIdx = (m_selectedIdx - 1 + n) % n;
}

std::span<const HudElement> MainMenuScreen::buildElements() {
    m_elementCount = 0;

    // Full-screen dark background (Rect: x/y = top-left, x2/y2 = bottom-right)
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
    m_strings[0] = "FIGHTERS LEGACY";
    {
        auto& el = m_elements[static_cast<std::size_t>(m_elementCount++)];
        el = HudElement{};
        el.type = HudElement::Type::Text;
        el.text = m_strings[0];
        el.x = 0.5f;
        el.y = 0.15f;
        el.scale = 2.f;
        el.r = 1.f;
        el.g = 1.f;
        el.b = 1.f;
        el.a = 1.f;
    }

    // Menu items
    const int n = static_cast<int>(m_items.size());
    for (int i = 0; i < n && m_elementCount < kMaxElements; ++i) {
        m_strings[static_cast<std::size_t>(2 + i)] = m_items[static_cast<std::size_t>(i)].label;
        auto& el = m_elements[static_cast<std::size_t>(m_elementCount++)];
        el = HudElement{};
        el.type = HudElement::Type::Text;
        el.text = m_strings[static_cast<std::size_t>(2 + i)];
        el.x = 0.5f;
        el.y = kStartY + static_cast<float>(i) * kSpacing;
        el.scale = 1.f;
        if (i == m_selectedIdx) {
            el.r = 0.2f;
            el.g = 1.f;
            el.b = 0.2f;
            el.a = 1.f; // bright green
        } else {
            el.r = 0.7f;
            el.g = 0.7f;
            el.b = 0.7f;
            el.a = 1.f; // dim white
        }
    }

    return {m_elements.data(), static_cast<std::size_t>(m_elementCount)};
}
