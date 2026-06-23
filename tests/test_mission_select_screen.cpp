// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "MissionSelectScreen.h"
#include "mock_hal.h"

using namespace fl;

static MockInput g_inp;
static MockWindow g_win;

TEST_CASE("MissionSelectScreen: empty list stays MissionSelect without input") {
    MissionSelectScreen s({});
    Screen stay = s.update(g_inp, g_win);
    CHECK(stay == Screen::MissionSelect);
}

TEST_CASE("MissionSelectScreen: empty list returns MainMenu on Escape") {
    MissionSelectScreen s({});
    MockInput inp;
    inp.justPressed.insert(Key::Escape);
    Screen next = s.update(inp, g_win);
    CHECK(next == Screen::MainMenu);
}

TEST_CASE("MissionSelectScreen: Escape returns MainMenu") {
    MissionSelectScreen s({"m01", "m02"});
    MockInput inp;
    inp.justPressed.insert(Key::Escape);
    Screen next = s.update(inp, g_win);
    CHECK(next == Screen::MainMenu);
}

TEST_CASE("MissionSelectScreen: ArrowDown moves selection") {
    MissionSelectScreen s({"alpha", "bravo", "charlie"});
    MockInput inp;
    inp.justPressed.insert(Key::ArrowDown);
    s.update(inp, g_win);
    CHECK(s.selectedMission().empty()); // not confirmed yet
}

TEST_CASE("MissionSelectScreen: Enter confirms and returns MissionBrief") {
    MissionSelectScreen s({"m01", "m02"});
    MockInput inp;
    inp.justPressed.insert(Key::Enter);
    Screen next = s.update(inp, g_win);
    CHECK(next == Screen::MissionBrief);
    CHECK(s.selectedMission() == "m01"); // first item confirmed
}

TEST_CASE("MissionSelectScreen: Down then Enter selects second item") {
    MissionSelectScreen s({"alpha", "bravo"});

    {
        MockInput inp;
        inp.justPressed.insert(Key::ArrowDown);
        s.update(inp, g_win);
    }
    {
        MockInput inp;
        inp.justPressed.insert(Key::Enter);
        Screen next = s.update(inp, g_win);
        CHECK(next == Screen::MissionBrief);
        CHECK(s.selectedMission() == "bravo");
    }
}

TEST_CASE("MissionSelectScreen: buildElements not empty with missions") {
    MissionSelectScreen s({"m01"});
    s.update(g_inp, g_win);
    CHECK(!s.buildElements().empty());
}

TEST_CASE("MissionSelectScreen: buildElements not empty even with no missions") {
    MissionSelectScreen s({});
    s.update(g_inp, g_win);
    CHECK(!s.buildElements().empty());
}
