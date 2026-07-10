// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "MissionBriefScreen.h"
#include "mock_hal.h"

using namespace fl;

static MockInput g_inp;
static MockWindow g_win;

TEST_CASE("MissionBriefScreen: default shows no mission name") {
    MissionBriefScreen s;
    s.update(g_inp, g_win);
    auto elems = s.buildElements();
    CHECK(!elems.empty());
}

TEST_CASE("MissionBriefScreen: setMission populates name in elements") {
    MissionBriefScreen s;
    s.setMission("m01", "Desert Storm");
    s.update(g_inp, g_win);
    auto elems = s.buildElements();

    bool foundName = false;
    for (auto& e : elems) {
        if (e.type == HudElement::Type::Text && e.text.find("Desert Storm") != std::string_view::npos)
            foundName = true;
    }
    CHECK(foundName);
}

TEST_CASE("MissionBriefScreen: Enter on Fly returns Loading") {
    MissionBriefScreen s;
    s.setMission("m01", "Alpha Mission");
    MockInput inp;
    inp.justPressed.insert(Key::Enter);
    Screen next = s.update(inp, g_win);
    CHECK(next == Screen::Loading);
}

TEST_CASE("MissionBriefScreen: ArrowRight then Enter returns MissionSelect") {
    MissionBriefScreen s;
    s.setMission("m01", "Bravo Mission");

    // Fly and Back are side-by-side; ArrowRight navigates to Back
    {
        MockInput inp;
        inp.justPressed.insert(Key::ArrowRight);
        s.update(inp, g_win);
    }
    {
        MockInput inp;
        inp.justPressed.insert(Key::Enter);
        Screen next = s.update(inp, g_win);
        CHECK(next == Screen::MissionSelect);
    }
}

TEST_CASE("MissionBriefScreen: Escape returns MissionSelect") {
    MissionBriefScreen s;
    s.setMission("m02", "Charlie Mission");
    MockInput inp;
    inp.justPressed.insert(Key::Escape);
    Screen next = s.update(inp, g_win);
    CHECK(next == Screen::MissionSelect);
}

TEST_CASE("MissionBriefScreen: buildElements shows terrain-id placeholder") {
    MissionBriefScreen s;
    s.setMission("world", "World Map Test");
    s.update(g_inp, g_win);
    auto elems = s.buildElements();

    bool foundMap = false;
    for (auto& e : elems) {
        if (e.type == HudElement::Type::Text && e.text.find("world") != std::string_view::npos)
            foundMap = true;
    }
    CHECK(foundMap);
}

TEST_CASE("MissionBriefScreen: text centered; buttons anchor at 0.35 and 0.65") {
    MissionBriefScreen s;
    s.setMission("m01", "Desert Storm");
    s.update(g_inp, g_win);
    auto elems = s.buildElements();
    bool foundFly = false;
    bool foundBack = false;
    for (const auto& el : elems) {
        if (el.type != HudElement::Type::Text)
            continue;
        CHECK(el.align == HudAlign::Center);
        if (el.text == "[ Fly ]") {
            foundFly = true;
            CHECK(el.x == Catch::Approx(0.35f)); // hover band [0.25,0.45) centers here
        } else if (el.text == "[ Back ]") {
            foundBack = true;
            CHECK(el.x == Catch::Approx(0.65f)); // hover band [0.55,0.75) centers here
        } else {
            CHECK(el.x == Catch::Approx(0.5f));
        }
    }
    CHECK(foundFly);
    CHECK(foundBack);
}
