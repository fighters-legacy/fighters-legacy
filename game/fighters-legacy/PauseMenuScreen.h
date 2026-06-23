// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IScreen.h"
#include "RenderTypes.h"

#include <array>
#include <string>

namespace fl {

// Pause overlay: Resume / Settings / Quit to Menu / Exit to Desktop.
// Escape = Resume. Semi-transparent rect overlays the paused scene.
class PauseMenuScreen : public IScreen {
  public:
    PauseMenuScreen() = default;

    Screen update(IInput& input, IWindow& window) override;
    std::span<const HudElement> buildElements() override;

    // Expose for unit tests.
    int selectedIdx() const {
        return m_selectedIdx;
    }
    int itemCount() const {
        return kItemCount;
    }

  private:
    int m_selectedIdx{0};

    static constexpr int kItemCount = 4; // Resume, Settings, Quit to Menu, Exit to Desktop
    static constexpr int kMaxElements = 12;
    std::array<HudElement, kMaxElements> m_elements{};
    std::array<std::string, kMaxElements> m_strings{};
    int m_elementCount{0};
};

} // namespace fl
