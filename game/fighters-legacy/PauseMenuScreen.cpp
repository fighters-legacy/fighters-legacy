// SPDX-License-Identifier: GPL-3.0-or-later
#include "PauseMenuScreen.h"
#include "MenuNav.h"
#include "render/HudBuilder.h" // hudFullscreenBg (#1261)

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

Screen PauseMenuScreen::update(IInput& input, IWindow& window, float /*frameDtS*/) {
    if (menuBackPressed(input))
        return Screen::Flight; // Resume

    menuNavigateWrap(input, kItemCount, m_selectedIdx);
    menuHoverHitTest(
        input, window, kItemCount, 0, kItemCount, 0.07f, [](int r) { return 0.35f + static_cast<float>(r) * 0.09f; },
        m_selectedIdx);

    if (menuConfirmPressed(input))
        return kItems[static_cast<std::size_t>(m_selectedIdx)].second;

    return Screen::Pause;
}

std::span<const HudElement> PauseMenuScreen::buildElements() {
    m_elementCount = 0;

    // Semi-transparent overlay
    {
        m_elements[static_cast<std::size_t>(m_elementCount++)] = hudFullscreenBg();
    }

    // Title
    m_strings[0] = "PAUSED";
    {
        auto& el = m_elements[static_cast<std::size_t>(m_elementCount++)];
        el = HudElement{};
        el.type = HudElement::Type::Text;
        el.text = m_strings[0];
        el.x = 0.5f;
        el.align = HudAlign::Center;
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
        el.align = HudAlign::Center;
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