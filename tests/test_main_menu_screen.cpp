// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "MainMenuScreen.h"
#include "mock_hal.h"

TEST_CASE("MainMenuScreen: no packs - no Select Mission item") {
    MainMenuScreen s(/*hasPacks=*/false);
    CHECK(s.itemCount() == 3); // Sandbox, Settings, Exit to Desktop
}

TEST_CASE("MainMenuScreen: with packs - includes Select Mission") {
    MainMenuScreen s(/*hasPacks=*/true);
    CHECK(s.itemCount() == 4); // Sandbox, Select Mission, Settings, Exit to Desktop
}

TEST_CASE("MainMenuScreen: ArrowDown wraps navigation") {
    MainMenuScreen s(false);
    CHECK(s.selectedIdx() == 0);
    for (int i = 0; i < s.itemCount(); ++i)
        s.selectNext();
    CHECK(s.selectedIdx() == 0); // wrapped back to start
}

TEST_CASE("MainMenuScreen: ArrowUp wraps upward") {
    MainMenuScreen s(false);
    CHECK(s.selectedIdx() == 0);
    s.selectPrev();
    CHECK(s.selectedIdx() == s.itemCount() - 1);
}

TEST_CASE("MainMenuScreen: confirm Sandbox returns Screen::Loading") {
    MainMenuScreen s(false);
    // First item is always Sandbox
    CHECK(s.selectedIdx() == 0);
    CHECK(s.confirm() == Screen::Loading);
}

TEST_CASE("MainMenuScreen: confirm Exit to Desktop returns Screen::Quit") {
    MainMenuScreen s(false);
    while (s.selectedIdx() != s.itemCount() - 1)
        s.selectNext();
    CHECK(s.confirm() == Screen::Quit);
}

TEST_CASE("MainMenuScreen: confirm Settings returns Screen::Settings") {
    MainMenuScreen s(false);
    // Settings is second-to-last (after Sandbox, before Exit)
    while (s.confirm() != Screen::Settings)
        s.selectNext();
    CHECK(s.confirm() == Screen::Settings);
}

TEST_CASE("MainMenuScreen: keyboard input moves selection") {
    MainMenuScreen s(false);
    MockInput inp;
    MockWindow win;

    inp.justPressed.insert(Key::ArrowDown);
    Screen next = s.update(inp, win);
    CHECK(next == Screen::MainMenu);
    CHECK(s.selectedIdx() == 1);
}

TEST_CASE("MainMenuScreen: Enter confirms selection") {
    MainMenuScreen s(false);
    MockInput inp;
    MockWindow win;
    inp.justPressed.insert(Key::Enter);
    Screen next = s.update(inp, win);
    CHECK(next == Screen::Loading); // first item = Sandbox
}

TEST_CASE("MainMenuScreen: buildElements not empty") {
    MainMenuScreen s(false);
    MockInput inp;
    MockWindow win;
    s.update(inp, win);
    auto elems = s.buildElements();
    CHECK(!elems.empty());
}
