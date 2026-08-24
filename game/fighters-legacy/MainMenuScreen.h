// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IScreen.h"
#include "RenderTypes.h"

#include <array>
#include <string>
#include <vector>

namespace fl {

// Main menu: Instant Action + Free Flight (single-player) or Join Server (multiplayer), Select Mission
// (packs only), Settings, Exit to Desktop.
class MainMenuScreen : public IScreen {
  public:
    // hasPacks controls whether "Select Mission" is shown.
    // isMultiplayer replaces the single-player Instant Action / Free Flight entries with "Join Server".
    explicit MainMenuScreen(bool hasPacks, bool isMultiplayer = false);

    Screen update(IInput& input, IWindow& window, float frameDtS) override;
    std::span<const HudElement> buildElements() override;

    // The mission to launch for the last confirmed item (#40): "builtin:sandbox" for Instant Action,
    // empty for Free Flight / Join Server. Game reads it when a menu confirm enters a session.
    const std::string& confirmedMission() const {
        return m_confirmedMission;
    }

    // Auto-start (menu bypass): Game injects the --mission / --auto session before the first frame
    // by setting the confirmed mission here and driving the same enters-session transition a menu
    // confirm produces, so the LoadingScreen wiring is identical to a human pressing Enter.
    void setConfirmedMission(std::string mission) {
        m_confirmedMission = std::move(mission);
    }

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
        std::string mission; // mission id to launch when this item enters a session (#40); empty = none
    };

    std::vector<Item> m_items;
    int m_selectedIdx{0};
    std::string m_confirmedMission; // mission of the last confirm() (#40)

    static constexpr int kMaxElements = 16;
    std::array<HudElement, kMaxElements> m_elements{};
    std::array<std::string, kMaxElements> m_strings{}; // backing storage for text string_views
    int m_elementCount{0};
};

} // namespace fl
