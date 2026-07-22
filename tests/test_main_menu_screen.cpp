// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "MainMenuScreen.h"
#include "mock_hal.h"

using namespace fl;

TEST_CASE("MainMenuScreen: no packs - no Select Mission item") {
    MainMenuScreen s(/*hasPacks=*/false);
    CHECK(s.itemCount() == 4); // Instant Action, Free Flight, Settings, Exit to Desktop
}

TEST_CASE("MainMenuScreen: with packs - includes Select Mission") {
    MainMenuScreen s(/*hasPacks=*/true);
    CHECK(s.itemCount() == 5); // Instant Action, Free Flight, Select Mission, Settings, Exit to Desktop
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

TEST_CASE("MainMenuScreen: Instant Action launches the builtin mission; Free Flight is empty (#40)") {
    MainMenuScreen s(false);
    // Item 0 = Instant Action -> Loading with the builtin skirmish; item 1 = Free Flight -> empty.
    CHECK(s.selectedIdx() == 0);
    CHECK(s.confirm() == Screen::Loading);
    CHECK(s.confirmedMission() == "builtin:sandbox");

    s.selectNext(); // Free Flight
    CHECK(s.confirm() == Screen::Loading);
    CHECK(s.confirmedMission().empty());
}

TEST_CASE("MainMenuScreen: setConfirmedMission injects the auto-start mission") {
    // The --mission/--auto menu bypass sets the confirmed mission directly, then Game drives the
    // same enters-session transition a confirm produces; the getter must echo the injection.
    MainMenuScreen s(false);
    CHECK(s.confirmedMission().empty());
    s.setConfirmedMission("builtin:shape-gallery");
    CHECK(s.confirmedMission() == "builtin:shape-gallery");
    // A later human confirm overwrites it (Instant Action -> builtin:sandbox).
    CHECK(s.confirm() == Screen::Loading);
    CHECK(s.confirmedMission() == "builtin:sandbox");
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
    CHECK(next == Screen::Loading); // first item = Instant Action
}

TEST_CASE("MainMenuScreen: buildElements not empty") {
    MainMenuScreen s(false);
    MockInput inp;
    MockWindow win;
    s.update(inp, win);
    auto elems = s.buildElements();
    CHECK(!elems.empty());
}

TEST_CASE("MainMenuScreen: multiplayer mode labels first item Join Server") {
    MainMenuScreen s(/*hasPacks=*/false, /*isMultiplayer=*/true);
    MockInput inp;
    MockWindow win;
    s.update(inp, win);
    // First item still navigates to Screen::Loading.
    CHECK(s.selectedIdx() == 0);
    // "Join Server" now opens the direct-connect form (#322), not straight to Loading.
    CHECK(s.confirm() == Screen::JoinServer);
    // The label visible in elements must contain "Join Server".
    auto elems = s.buildElements();
    bool found = false;
    for (const auto& el : elems)
        if (el.type == HudElement::Type::Text && el.text.find("Join Server") != std::string_view::npos)
            found = true;
    CHECK(found);
}

TEST_CASE("MainMenuScreen: multiplayer replaces Instant Action + Free Flight with a single Join Server") {
    MainMenuScreen sp(/*hasPacks=*/false, /*isMultiplayer=*/false);
    MainMenuScreen mp(/*hasPacks=*/false, /*isMultiplayer=*/true);
    // Single-player offers two launch entries (Instant Action, Free Flight); multiplayer offers one
    // (Join Server), so it has exactly one fewer item.
    CHECK(mp.itemCount() == sp.itemCount() - 1);
}

TEST_CASE("MainMenuScreen: title and items are center-aligned at x=0.5") {
    MainMenuScreen s(/*hasPacks=*/true);
    MockInput inp;
    MockWindow win;
    s.update(inp, win);
    auto elems = s.buildElements();
    int textCount = 0;
    for (const auto& el : elems) {
        if (el.type != HudElement::Type::Text)
            continue;
        ++textCount;
        CHECK(el.align == HudAlign::Center);
        CHECK(el.x == Catch::Approx(0.5f));
    }
    CHECK(textCount == s.itemCount() + 1); // title + menu items
}
