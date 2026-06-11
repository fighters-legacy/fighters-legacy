// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IScreen.h"
#include "RenderTypes.h"

#include <array>
#include <string>
#include <vector>

// Main menu: Sandbox (always), Select Mission (packs only), Settings, Exit to Desktop.
class MainMenuScreen : public IScreen {
  public:
    // hasPacks controls whether "Select Mission" is shown.
    explicit MainMenuScreen(bool hasPacks);

    Screen update(IInput& input, IWindow& window) override;
    std::span<const HudElement> buildElements() override;

    // Test helpers
    void selectNext();
    void selectPrev();
    Screen confirm();

    int selectedIdx() const {
        return m_selectedIdx;
    }
    int itemCount() const {
        return static_cast<int>(m_items.size());
    }

  private:
    struct Item {
        std::string label;
        Screen target;
    };

    std::vector<Item> m_items;
    int m_selectedIdx{0};

    static constexpr int kMaxElements = 16;
    std::array<HudElement, kMaxElements> m_elements{};
    std::array<std::string, kMaxElements> m_strings{}; // backing storage for text string_views
    int m_elementCount{0};
};
