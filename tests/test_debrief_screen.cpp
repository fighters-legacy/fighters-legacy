// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "DebriefScreen.h"
#include "mock_hal.h"

#include <string>

static MockInput g_inp;
static MockWindow g_win;

TEST_CASE("DebriefScreen: Continue returns MainMenu") {
    DebriefScreen s;
    MockInput inp;
    inp.justPressed.insert(Key::Enter);
    CHECK(s.update(inp, g_win) == Screen::MainMenu);
}

TEST_CASE("DebriefScreen: Space also returns MainMenu") {
    DebriefScreen s;
    MockInput inp;
    inp.justPressed.insert(Key::Space);
    CHECK(s.update(inp, g_win) == Screen::MainMenu);
}

TEST_CASE("DebriefScreen: no input stays on Debrief") {
    DebriefScreen s;
    CHECK(s.update(g_inp, g_win) == Screen::Debrief);
}

TEST_CASE("DebriefScreen: default shows success") {
    DebriefScreen s;
    s.update(g_inp, g_win);
    auto elems = s.buildElements();
    bool foundSuccess = false;
    for (auto& e : elems) {
        if (e.type == HudElement::Type::Text &&
            (e.text.find("COMPLETE") != std::string_view::npos || e.text.find("Complete") != std::string_view::npos ||
             e.text.find("SUCCESS") != std::string_view::npos))
            foundSuccess = true;
    }
    CHECK(foundSuccess);
}

TEST_CASE("DebriefScreen: setStats with success=false shows failed text") {
    DebriefScreen s;
    s.setStats(0, 1, false);
    s.update(g_inp, g_win);
    auto elems = s.buildElements();
    bool foundFailed = false;
    for (auto& e : elems) {
        if (e.type == HudElement::Type::Text &&
            (e.text.find("FAIL") != std::string_view::npos || e.text.find("Fail") != std::string_view::npos ||
             e.text.find("FAILED") != std::string_view::npos))
            foundFailed = true;
    }
    CHECK(foundFailed);
}

TEST_CASE("DebriefScreen: setStats non-zero kills appear in elements") {
    DebriefScreen s;
    s.setStats(3, 1, true);
    s.update(g_inp, g_win);
    auto elems = s.buildElements();
    bool foundKills = false;
    for (auto& e : elems) {
        if (e.type == HudElement::Type::Text &&
            (e.text.find("3") != std::string_view::npos || e.text.find("Kills") != std::string_view::npos ||
             e.text.find("kills") != std::string_view::npos))
            foundKills = true;
    }
    CHECK(foundKills);
}

TEST_CASE("DebriefScreen: buildElements not empty") {
    DebriefScreen s;
    s.update(g_inp, g_win);
    CHECK(!s.buildElements().empty());
}
