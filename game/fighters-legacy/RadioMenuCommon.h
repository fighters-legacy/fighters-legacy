// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IClock.h"
#include "IInput.h"

#include <chrono>

namespace fl {

// The radio-menu skeleton, in one place (#1265).
//
// WingmanMenu (#791) came first; CommsMenu (#984) was copied from it and has since grown pages and
// an Escape that backs out one level instead of closing. Those differences are real feature surface
// and stay in each menu. What was NOT a difference — the auto-close clock and the digit/arrow/Enter
// selection — was the same code twice, and the auto-close in particular is the kind of thing that
// drifts silently: a menu that stops timing out just sits on the HUD, and nobody files that as a bug
// against the menu they were not using.
//
// Game-local and client-only: this is HUD input handling, not simulation.

// Auto-close deadline for an abandoned menu. `expired` is what every caller checks first, before any
// input, so a menu that timed out while the player was busy flying cannot also act on a keypress.
struct MenuTimeout {
    std::chrono::steady_clock::time_point until{};

    void arm(std::chrono::steady_clock::time_point now, float seconds) noexcept {
        until = now + std::chrono::milliseconds(static_cast<long long>(seconds * 1000.f));
    }
    [[nodiscard]] bool expired(std::chrono::steady_clock::time_point now) const noexcept {
        return now >= until;
    }
};

// Advance the highlight on the arrows and report which item the player chose this frame, or -1.
//
// A digit picks its item DIRECTLY: the digit is the ordinal + 1, so there is no mapping to get
// wrong, and it is the fast path for a player who is mid-turn. The arrows wrap and Enter commits the
// highlight, for players who would rather not hunt for a digit. Nine digits is the ceiling because
// there are nine of them; a longer menu simply cannot be reached by digit past the ninth item.
[[nodiscard]] inline int pickMenuItem(IInput& input, int count, int& selected) {
    if (count <= 0)
        return -1;

    if (input.isKeyJustPressed(Key::ArrowDown))
        selected = (selected + 1) % count;
    if (input.isKeyJustPressed(Key::ArrowUp))
        selected = (selected + count - 1) % count;

    static constexpr Key kDigits[9] = {Key::Num1, Key::Num2, Key::Num3, Key::Num4, Key::Num5,
                                       Key::Num6, Key::Num7, Key::Num8, Key::Num9};
    int chosen = -1;
    for (int i = 0; i < count && i < 9; ++i) {
        if (input.isKeyJustPressed(kDigits[i]))
            chosen = i;
    }
    if (input.isKeyJustPressed(Key::Enter))
        chosen = selected;

    return (chosen >= 0 && chosen < count) ? chosen : -1;
}

} // namespace fl
