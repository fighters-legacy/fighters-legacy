// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IScreen.h"
#include "RenderTypes.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// ReplaySelectScreen (#41) — the browser for recorded matches.
//
// Modelled on MissionSelectScreen (same list/scroll/element-budget shape), with one deliberate
// difference: a replay this build CANNOT play is still listed, greyed out, with the reader's own
// refusal as its subtitle. Silently omitting it would leave a player staring at a directory full of
// files and a menu that says "no replays", which is the worst possible answer to "why is my
// recording missing".

namespace fl {

class ReplaySelectScreen : public IScreen {
  public:
    // One row: what the header says about a file, or why it cannot be read.
    struct Entry {
        std::filesystem::path path;
        std::string label;  // "27 Jul 14:02   ci_smoke   4m12s"
        std::string detail; // engine version, or the refusal message
        bool playable{false};
    };

    // Scan `dir` for `.flrep` files, newest first, reading each header. A directory that does not
    // exist is not an error -- it is a player who has not recorded anything yet.
    static std::vector<Entry> scan(const std::filesystem::path& dir);

    explicit ReplaySelectScreen(std::vector<Entry> entries);

    Screen update(IInput& input, IWindow& window) override;
    std::span<const HudElement> buildElements() override;

    // The file confirmed by the last successful selection; empty until then.
    [[nodiscard]] const std::filesystem::path& selectedReplay() const noexcept {
        return m_selected;
    }
    [[nodiscard]] std::size_t entryCount() const noexcept {
        return m_entries.size();
    }
    [[nodiscard]] int selectedIndex() const noexcept {
        return m_selectedIdx;
    }
    // Test seam + the "you cannot play that one" feedback line.
    [[nodiscard]] const std::string& statusText() const noexcept {
        return m_status;
    }

  private:
    std::vector<Entry> m_entries;
    int m_selectedIdx{0};
    int m_scrollOffset{0};
    std::filesystem::path m_selected;
    std::string m_status;

    static constexpr int kVisible = 10;
    static constexpr int kMaxElements = (kVisible * 2) + 5; // label + detail per row, title, bg, status
    std::array<HudElement, kMaxElements> m_elements{};
    std::array<std::string, kMaxElements> m_strings{};
    int m_elementCount{0};
};

} // namespace fl
