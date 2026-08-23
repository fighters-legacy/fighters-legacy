// SPDX-License-Identifier: GPL-3.0-or-later
//
// fl::MenuNav (#1260) -- the game's menu input contract.
//
// Six screens each spelled out the same confirm gesture, and five the same up/down/D-pad handling
// and mouse-hover row loop. Any change to it -- rebinding a menu key, supporting another gamepad
// button -- was a six-file synchronised edit, and the copies had already started to drift: mission
// select repeats its scroll clamp inline twice where replay select uses lambdas.
//
// The per-screen tests pin each screen's behaviour. These pin the contract itself, including the
// edges the screens never exercise: an empty list, and the ends of a scrolled one.

#include "MenuNav.h"
#include "mock_hal.h"

#include <catch2/catch_test_macros.hpp>

using namespace fl;

namespace {
MockWindow g_win;
} // namespace

TEST_CASE("the confirm gesture is all four bindings", "[menu_nav]") {
    for (const Key k : {Key::Enter, Key::Space}) {
        MockInput inp;
        inp.justPressed.insert(k);
        CHECK(menuConfirmPressed(inp));
    }
    MockInput mouse;
    mouse.mouseJustPressed.insert(MouseButton::Left);
    CHECK(menuConfirmPressed(mouse));

    MockInput pad;
    pad.gpJustPressed.insert({0, GamepadButton::A});
    CHECK(menuConfirmPressed(pad));

    MockInput none;
    CHECK_FALSE(menuConfirmPressed(none));
    // Back is NOT confirm -- the two gestures must stay distinct or Escape would activate a button.
    MockInput esc;
    esc.justPressed.insert(Key::Escape);
    CHECK_FALSE(menuConfirmPressed(esc));
    CHECK(menuBackPressed(esc));
}

TEST_CASE("wrap navigation wraps at both ends", "[menu_nav]") {
    int idx = 0;
    MockInput up;
    up.justPressed.insert(Key::ArrowUp);
    menuNavigateWrap(up, 3, idx);
    CHECK(idx == 2); // off the top is the bottom

    MockInput down;
    down.justPressed.insert(Key::ArrowDown);
    menuNavigateWrap(down, 3, idx);
    CHECK(idx == 0);

    // W/S and the D-pad are the same gesture as the arrows.
    MockInput w;
    w.justPressed.insert(Key::W);
    menuNavigateWrap(w, 3, idx);
    CHECK(idx == 2);
    MockInput dpad;
    dpad.gpJustPressed.insert({0, GamepadButton::DpadDown});
    menuNavigateWrap(dpad, 3, idx);
    CHECK(idx == 0);
}

TEST_CASE("wrap navigation on an empty list does not divide by zero", "[menu_nav]") {
    // The main menu builds its items conditionally on what content is installed, so "no items" is
    // reachable rather than theoretical -- and `% 0` is undefined behaviour, not a no-op.
    int idx = 0;
    MockInput up;
    up.justPressed.insert(Key::ArrowUp);
    menuNavigateWrap(up, 0, idx);
    CHECK(idx == 0);
}

TEST_CASE("scrolled navigation clamps at the ends instead of wrapping", "[menu_nav]") {
    int idx = 0, scroll = 0;
    MockInput up;
    up.justPressed.insert(Key::ArrowUp);
    menuNavigateScrolled(up, 10, 4, idx, scroll);
    CHECK(idx == 0); // stays put; wrapping would also have to jump the window to the end
    CHECK(scroll == 0);

    MockInput down;
    down.justPressed.insert(Key::ArrowDown);
    for (int i = 0; i < 9; ++i)
        menuNavigateScrolled(down, 10, 4, idx, scroll);
    CHECK(idx == 9);
    CHECK(scroll == 6); // the window followed: rows 6..9 visible
    menuNavigateScrolled(down, 10, 4, idx, scroll);
    CHECK(idx == 9); // clamped at the bottom
}

TEST_CASE("the scroll window follows the selection back up", "[menu_nav]") {
    int idx = 9, scroll = 6;
    MockInput up;
    up.justPressed.insert(Key::ArrowUp);
    for (int i = 0; i < 4; ++i)
        menuNavigateScrolled(up, 10, 4, idx, scroll);
    CHECK(idx == 5);
    CHECK(scroll == 5);
}

TEST_CASE("hover selects the row under the pointer and ignores the gaps", "[menu_nav]") {
    // Rows at 0.25, 0.315, 0.38..., hit height 0.055 -- so there is a 0.01 gap between them.
    const auto rowY = [](int r) { return 0.25f + static_cast<float>(r) * 0.065f; };
    int idx = 0;

    MockInput inp;
    inp.mouseY = static_cast<int>(0.32f * static_cast<float>(g_win.logicalHeight())); // inside row 1
    menuHoverHitTest(inp, g_win, 4, 0, 10, 0.055f, rowY, idx);
    CHECK(idx == 1);

    // In the gap between rows: the selection does NOT move, rather than snapping to a neighbour.
    idx = 3;
    MockInput gap;
    gap.mouseY = static_cast<int>(0.311f * static_cast<float>(g_win.logicalHeight()));
    menuHoverHitTest(gap, g_win, 4, 0, 10, 0.055f, rowY, idx);
    CHECK(idx == 3);
}

TEST_CASE("hover maps a displayed row onto the scrolled item index", "[menu_nav]") {
    // Displayed row 0 is item `firstIndex`, which is what makes hovering the top of a scrolled
    // list select item 6 rather than item 0.
    const auto rowY = [](int r) { return 0.25f + static_cast<float>(r) * 0.065f; };
    int idx = 0;
    MockInput inp;
    inp.mouseY = static_cast<int>(0.26f * static_cast<float>(g_win.logicalHeight()));
    menuHoverHitTest(inp, g_win, 4, 6, 10, 0.055f, rowY, idx);
    CHECK(idx == 6);
}

TEST_CASE("hover stops at the end of a short list", "[menu_nav]") {
    // A window of 4 showing the last 2 items must not select a row that has nothing in it.
    const auto rowY = [](int r) { return 0.25f + static_cast<float>(r) * 0.065f; };
    int idx = 8;
    MockInput inp;
    inp.mouseY = static_cast<int>(0.45f * static_cast<float>(g_win.logicalHeight())); // displayed row 3
    menuHoverHitTest(inp, g_win, 4, 8, 10, 0.055f, rowY, idx);
    CHECK(idx == 8); // rows 2 and 3 are past the end of the list
}
