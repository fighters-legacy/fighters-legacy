// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <IInput.h>
#include <IWindow.h>

namespace fl {

// The game's menu input contract, in one place (#1260).
//
// Six screens -- main menu, mission select, pause, replay select, settings and mission brief --
// each spelled out the same confirm gesture, and five of them the same up/down/D-pad navigation and
// the same mouse-hover row loop. Any change to it (rebinding a menu key, supporting another
// gamepad button) was a six-file synchronised edit, and the copies had already started to drift in
// style: mission select repeats its scroll clamp inline twice where replay select uses lambdas.
//
// Game-local rather than promoted to engine/: screens are client-only, there is no server consumer,
// and D23 asks for a shared home only when a second target needs it.
//
// What deliberately does NOT live here: each screen's Escape SEMANTICS. Main menu jumps the
// selection to Exit, pause resumes, the select screens go back a level. That is a decision about
// the state machine, so the helper offers a predicate and the screen keeps the transition.

// Enter / Space / left mouse / gamepad A.
[[nodiscard]] inline bool menuConfirmPressed(IInput& input) {
    return input.isKeyJustPressed(Key::Enter) || input.isKeyJustPressed(Key::Space) ||
           input.isMouseButtonJustPressed(MouseButton::Left) || input.isGamepadButtonJustPressed(0, GamepadButton::A);
}

// Escape / gamepad B. What "back" MEANS is the caller's business.
[[nodiscard]] inline bool menuBackPressed(IInput& input) {
    return input.isKeyJustPressed(Key::Escape) || input.isGamepadButtonJustPressed(0, GamepadButton::B);
}

[[nodiscard]] inline bool menuUpPressed(IInput& input) {
    return input.isKeyJustPressed(Key::ArrowUp) || input.isKeyJustPressed(Key::W) ||
           input.isGamepadButtonJustPressed(0, GamepadButton::DpadUp);
}

[[nodiscard]] inline bool menuDownPressed(IInput& input) {
    return input.isKeyJustPressed(Key::ArrowDown) || input.isKeyJustPressed(Key::S) ||
           input.isGamepadButtonJustPressed(0, GamepadButton::DpadDown);
}

// Wrapping navigation over `n` items: off the top is the bottom. For short fixed menus where every
// item is on screen.
inline void menuNavigateWrap(IInput& input, int n, int& selectedIdx) {
    if (n <= 0)
        return;
    if (menuUpPressed(input))
        selectedIdx = (selectedIdx - 1 + n) % n;
    if (menuDownPressed(input))
        selectedIdx = (selectedIdx + 1) % n;
}

// Clamped navigation with a scroll window: the selection stops at the ends rather than wrapping,
// and the window follows it. For lists longer than the screen, where wrapping from the last entry
// to the first would also have to jump the scroll offset the whole way back.
inline void menuNavigateScrolled(IInput& input, int n, int visible, int& selectedIdx, int& scrollOffset) {
    if (n <= 0 || visible <= 0)
        return;
    if (menuUpPressed(input) && selectedIdx > 0) {
        --selectedIdx;
        if (selectedIdx < scrollOffset)
            scrollOffset = selectedIdx;
    }
    if (menuDownPressed(input) && selectedIdx < n - 1) {
        ++selectedIdx;
        if (selectedIdx >= scrollOffset + visible)
            scrollOffset = selectedIdx - visible + 1;
    }
}

// Point the selection at the row under the mouse, if any.
//
// `rowY(row)` returns the TOP edge of displayed row `row` in normalized [0, 1] screen coordinates;
// displayed row r shows item `firstIndex + r`. A callback rather than a start/pitch pair so a screen
// that already owns its row geometry -- the settings screen lays rows out with its own rowY() and
// the renderer uses the same function -- hit-tests against that instead of re-deriving it here.
//
// The last matching row wins, where the main menu used to take the first. Equivalent as things
// stand: every screen's hit height is <= its row pitch and the interval is half-open, so no point
// falls in two rows. If a screen ever overlaps its rows, that is the line to revisit.
//
// Coordinates are LOGICAL, not physical: pointer events arrive in logical units, and using physical
// pixels mis-hits every row at 2x scaling.
template <typename RowYFn>
inline void menuHoverHitTest(IInput& input, IWindow& window, int displayedRows, int firstIndex, int itemCount,
                             float rowHitHeight, RowYFn rowY, int& selectedIdx) {
    const float fh = static_cast<float>(window.logicalHeight());
    if (fh <= 0.f)
        return;
    int mx = 0, my = 0;
    input.getMousePosition(mx, my);
    const float ny = static_cast<float>(my) / fh;
    for (int r = 0; r < displayedRows; ++r) {
        const int idx = firstIndex + r;
        if (idx >= itemCount)
            break;
        const float iy = rowY(r);
        if (ny >= iy && ny < iy + rowHitHeight)
            selectedIdx = idx;
    }
}

} // namespace fl
