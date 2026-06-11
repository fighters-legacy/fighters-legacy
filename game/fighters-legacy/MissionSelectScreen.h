// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IScreen.h"
#include "RenderTypes.h"

#include <array>
#include <string>
#include <vector>

// Mission selection list populated from AssetManager::listMissions().
class MissionSelectScreen : public IScreen {
  public:
    explicit MissionSelectScreen(std::vector<std::string> missions);

    Screen update(IInput& input, IWindow& window) override;
    std::span<const HudElement> buildElements() override;

    // Returns the id of the last confirmed mission.
    const std::string& selectedMission() const {
        return m_selected;
    }

  private:
    std::vector<std::string> m_missions;
    int m_selectedIdx{0};
    int m_scrollOffset{0};
    std::string m_selected;

    static constexpr int kVisible = 10;
    static constexpr int kMaxElements = kVisible + 4; // items + title + bg + scroll hint
    std::array<HudElement, kMaxElements> m_elements{};
    std::array<std::string, kVisible + 4> m_strings{};
    int m_elementCount{0};
};
