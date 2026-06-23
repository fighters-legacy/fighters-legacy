// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "PauseMenuScreen.h"
#include "mock_hal.h"

using namespace fl;

static MockWindow g_win;

TEST_CASE("PauseMenuScreen: has exactly 4 items") {
    PauseMenuScreen s;
    CHECK(s.itemCount() == 4);
}

TEST_CASE("PauseMenuScreen: Escape returns Flight (Resume)") {
    PauseMenuScreen s;
    MockInput inp;
    inp.justPressed.insert(Key::Escape);
    CHECK(s.update(inp, g_win) == Screen::Flight);
}

TEST_CASE("PauseMenuScreen: Enter on first item (Resume) returns Flight") {
    PauseMenuScreen s;
    CHECK(s.selectedIdx() == 0);
    MockInput inp;
    inp.justPressed.insert(Key::Enter);
    CHECK(s.update(inp, g_win) == Screen::Flight);
}

TEST_CASE("PauseMenuScreen: Enter on Settings returns Settings") {
    PauseMenuScreen s;
    // Navigate to Settings (index 1)
    {
        MockInput inp;
        inp.justPressed.insert(Key::ArrowDown);
        s.update(inp, g_win);
    }
    CHECK(s.selectedIdx() == 1);
    MockInput inp;
    inp.justPressed.insert(Key::Enter);
    CHECK(s.update(inp, g_win) == Screen::Settings);
}

TEST_CASE("PauseMenuScreen: Enter on Quit to Menu returns MainMenu") {
    PauseMenuScreen s;
    // Navigate to Quit to Menu (index 2)
    for (int i = 0; i < 2; ++i) {
        MockInput inp;
        inp.justPressed.insert(Key::ArrowDown);
        s.update(inp, g_win);
    }
    CHECK(s.selectedIdx() == 2);
    MockInput inp;
    inp.justPressed.insert(Key::Enter);
    CHECK(s.update(inp, g_win) == Screen::MainMenu);
}

TEST_CASE("PauseMenuScreen: Enter on Exit to Desktop returns Quit") {
    PauseMenuScreen s;
    // Navigate to Exit to Desktop (index 3)
    for (int i = 0; i < 3; ++i) {
        MockInput inp;
        inp.justPressed.insert(Key::ArrowDown);
        s.update(inp, g_win);
    }
    CHECK(s.selectedIdx() == 3);
    MockInput inp;
    inp.justPressed.insert(Key::Enter);
    CHECK(s.update(inp, g_win) == Screen::Quit);
}

TEST_CASE("PauseMenuScreen: ArrowUp from index 0 wraps to last item") {
    PauseMenuScreen s;
    CHECK(s.selectedIdx() == 0);
    MockInput inp;
    inp.justPressed.insert(Key::ArrowUp);
    s.update(inp, g_win);
    CHECK(s.selectedIdx() == s.itemCount() - 1);
}

TEST_CASE("PauseMenuScreen: ArrowDown wraps from last to first") {
    PauseMenuScreen s;
    for (int i = 0; i < s.itemCount(); ++i) {
        MockInput inp;
        inp.justPressed.insert(Key::ArrowDown);
        s.update(inp, g_win);
    }
    CHECK(s.selectedIdx() == 0);
}

TEST_CASE("PauseMenuScreen: no input stays on Pause") {
    PauseMenuScreen s;
    MockInput inp;
    CHECK(s.update(inp, g_win) == Screen::Pause);
}

TEST_CASE("PauseMenuScreen: buildElements not empty") {
    PauseMenuScreen s;
    MockInput inp;
    s.update(inp, g_win);
    CHECK(!s.buildElements().empty());
}
