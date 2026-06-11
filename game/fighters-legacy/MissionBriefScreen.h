// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IScreen.h"
#include "RenderTypes.h"

#include <array>
#include <string>

// Mission briefing: shows name, map placeholder, objectives stub.
// "Fly" → Loading, "Back" → MissionSelect.
class MissionBriefScreen : public IScreen {
  public:
    MissionBriefScreen() = default;

    void setMission(std::string_view missionId, std::string_view missionName);

    Screen update(IInput& input, IWindow& window) override;
    std::span<const HudElement> buildElements() override;

  private:
    std::string m_id;
    std::string m_name;
    int m_selectedIdx{0}; // 0 = Fly, 1 = Back

    static constexpr int kMaxElements = 10;
    std::array<HudElement, kMaxElements> m_elements{};
    std::array<std::string, 8> m_strings{};
    int m_elementCount{0};
};
