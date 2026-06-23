// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IScreen.h"
#include "RenderTypes.h"

#include <array>
#include <string>

namespace fl {

// Post-flight debrief stub. Shows mission outcome and kill/loss tallies.
// Phase 2: stats are always zero (sandbox has no kill tracking).
class DebriefScreen : public IScreen {
  public:
    DebriefScreen() = default;

    void setStats(int kills, int losses, bool missionSuccess);

    Screen update(IInput& input, IWindow& window) override;
    std::span<const HudElement> buildElements() override;

  private:
    int m_kills{0};
    int m_losses{0};
    bool m_success{true};

    static constexpr int kMaxElements = 10;
    std::array<HudElement, kMaxElements> m_elements{};
    std::array<std::string, kMaxElements> m_strings{};
    int m_elementCount{0};
};

} // namespace fl
