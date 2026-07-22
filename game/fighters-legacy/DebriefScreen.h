// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IScreen.h"
#include "RenderTypes.h"

#include <array>
#include <string>
#include <utility>
#include <vector>

namespace fl {

// Post-flight debrief. Shows mission outcome and kill/loss tallies, plus — in a multiplayer match
// (#647) — the team result banner and per-team scores above the personal tallies.
class DebriefScreen : public IScreen {
  public:
    DebriefScreen() = default;

    void setStats(int kills, int losses, bool missionSuccess);

    // Multiplayer match result (#647): a winner/draw banner and per-team final scores, shown above the
    // personal tallies. Passing an empty team list clears the match section (single-player debrief).
    void setMatchResult(std::string winnerText, std::vector<std::pair<std::string, int>> teamScores);

    Screen update(IInput& input, IWindow& window) override;
    std::span<const HudElement> buildElements() override;

  private:
    int m_kills{0};
    int m_losses{0};
    bool m_success{true};

    bool m_hasMatchResult{false};
    std::string m_winner;
    std::vector<std::pair<std::string, int>> m_teamScores;

    static constexpr int kMaxElements = 20;
    std::array<HudElement, kMaxElements> m_elements{};
    std::array<std::string, kMaxElements> m_strings{};
    int m_elementCount{0};
};

} // namespace fl
