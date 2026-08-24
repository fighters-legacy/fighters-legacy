// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "MissionSelectScreen.h"
#include "mock_hal.h"

using namespace fl;

// These suites drive the screen a fixed step at a time; nothing here is rate-dependent (#1241).
constexpr float kTestFrameDtS = 1.f / 60.f;

static MockInput g_inp;
static MockWindow g_win;

TEST_CASE("MissionSelectScreen: empty list stays MissionSelect without input") {
    MissionSelectScreen s({});
    Screen stay = s.update(g_inp, g_win, kTestFrameDtS);
    CHECK(stay == Screen::MissionSelect);
}

TEST_CASE("MissionSelectScreen: empty list returns MainMenu on Escape") {
    MissionSelectScreen s({});
    MockInput inp;
    inp.justPressed.insert(Key::Escape);
    Screen next = s.update(inp, g_win, kTestFrameDtS);
    CHECK(next == Screen::MainMenu);
}

TEST_CASE("MissionSelectScreen: Escape returns MainMenu") {
    MissionSelectScreen s({"m01", "m02"});
    MockInput inp;
    inp.justPressed.insert(Key::Escape);
    Screen next = s.update(inp, g_win, kTestFrameDtS);
    CHECK(next == Screen::MainMenu);
}

TEST_CASE("MissionSelectScreen: ArrowDown moves selection") {
    MissionSelectScreen s({"alpha", "bravo", "charlie"});
    MockInput inp;
    inp.justPressed.insert(Key::ArrowDown);
    s.update(inp, g_win, kTestFrameDtS);
    CHECK(s.selectedMission().empty()); // not confirmed yet
}

TEST_CASE("MissionSelectScreen: Enter confirms and returns MissionBrief") {
    MissionSelectScreen s({"m01", "m02"});
    MockInput inp;
    inp.justPressed.insert(Key::Enter);
    Screen next = s.update(inp, g_win, kTestFrameDtS);
    CHECK(next == Screen::MissionBrief);
    CHECK(s.selectedMission() == "m01"); // first item confirmed
}

TEST_CASE("MissionSelectScreen: Down then Enter selects second item") {
    MissionSelectScreen s({"alpha", "bravo"});

    {
        MockInput inp;
        inp.justPressed.insert(Key::ArrowDown);
        s.update(inp, g_win, kTestFrameDtS);
    }
    {
        MockInput inp;
        inp.justPressed.insert(Key::Enter);
        Screen next = s.update(inp, g_win, kTestFrameDtS);
        CHECK(next == Screen::MissionBrief);
        CHECK(s.selectedMission() == "bravo");
    }
}

TEST_CASE("MissionSelectScreen: buildElements not empty with missions") {
    MissionSelectScreen s({"m01"});
    s.update(g_inp, g_win, kTestFrameDtS);
    CHECK(!s.buildElements().empty());
}

TEST_CASE("MissionSelectScreen: buildElements not empty even with no missions") {
    MissionSelectScreen s({});
    s.update(g_inp, g_win, kTestFrameDtS);
    CHECK(!s.buildElements().empty());
}

TEST_CASE("MissionSelectScreen: title and rows are center-aligned at x=0.5") {
    MissionSelectScreen s({"m01", "m02"});
    s.update(g_inp, g_win, kTestFrameDtS);
    auto elems = s.buildElements();
    int textCount = 0;
    for (const auto& el : elems) {
        if (el.type != HudElement::Type::Text)
            continue;
        ++textCount;
        CHECK(el.align == HudAlign::Center);
        CHECK(el.x == Catch::Approx(0.5f));
    }
    CHECK(textCount == 3); // title + two mission rows
}
