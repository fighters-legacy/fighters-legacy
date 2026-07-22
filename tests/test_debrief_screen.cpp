// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "DebriefScreen.h"
#include "mock_hal.h"

#include <string>

using namespace fl;

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

TEST_CASE("DebriefScreen: setMatchResult shows winner banner and team scores (#647)") {
    DebriefScreen s;
    s.setStats(4, 2, true);
    s.setMatchResult("RED WINS", {{"Red", 50}, {"Blue", 41}});
    s.update(g_inp, g_win);
    auto elems = s.buildElements();
    bool foundWinner = false, foundRed = false, foundBlue = false;
    for (const auto& e : elems) {
        if (e.type != HudElement::Type::Text)
            continue;
        if (e.text.find("RED WINS") != std::string_view::npos)
            foundWinner = true;
        if (e.text.find("Red") != std::string_view::npos && e.text.find("50") != std::string_view::npos)
            foundRed = true;
        if (e.text.find("Blue") != std::string_view::npos && e.text.find("41") != std::string_view::npos)
            foundBlue = true;
    }
    CHECK(foundWinner);
    CHECK(foundRed);
    CHECK(foundBlue);
}

TEST_CASE("DebriefScreen: empty setMatchResult clears the match section (#647)") {
    DebriefScreen s;
    s.setMatchResult("X WINS", {{"X", 10}});
    s.setMatchResult("", {}); // clear
    s.update(g_inp, g_win);
    auto elems = s.buildElements();
    int textCount = 0;
    for (const auto& e : elems)
        if (e.type == HudElement::Type::Text)
            ++textCount;
    CHECK(textCount == 4); // back to outcome/kills/losses/continue
}

TEST_CASE("DebriefScreen: all text elements are center-aligned at x=0.5") {
    DebriefScreen s;
    s.setStats(3, 1, true);
    s.update(g_inp, g_win);
    auto elems = s.buildElements();
    int textCount = 0;
    for (const auto& el : elems) {
        if (el.type != HudElement::Type::Text)
            continue;
        ++textCount;
        CHECK(el.align == HudAlign::Center);
        CHECK(el.x == Catch::Approx(0.5f));
    }
    CHECK(textCount == 4); // outcome, kills, losses, continue
}
