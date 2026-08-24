// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for ServerBrowserScreen (#143), driven against the scripted NullGui + the HAL mocks.

#include "ServerBrowserScreen.h"

#include "mock_gui.h"
#include "mock_hal.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

using namespace fl;

// These suites drive the screen a fixed step at a time; nothing here is rate-dependent (#1241).
constexpr float kTestFrameDtS = 1.f / 60.f;

namespace {
BrowserRow row(const char* name, const char* host, uint16_t port, int players) {
    BrowserRow r;
    r.name = name;
    r.host = host;
    r.gamePort = port;
    r.players = players;
    r.maxPlayers = 16;
    return r;
}
} // namespace

TEST_CASE("ServerBrowserScreen: renders rows + Refresh/Direct/Cancel controls (#143)", "[server_browser_screen]") {
    NullGui gui;
    MockInput input;
    MockWindow window;
    std::vector<BrowserRow> rows{row("Alpha", "1.2.3.4", 4778, 3), row("Bravo", "host.example", 5000, 0)};

    ServerBrowserScreen::Deps d;
    d.gui = &gui;
    d.rows = &rows;
    ServerBrowserScreen scr(std::move(d));

    const Screen next = scr.update(input, window, kTestFrameDtS);
    CHECK(next == Screen::ServerBrowser); // nothing clicked -> stays
    CHECK(gui.newFrameCount == 0);        // the screen does not open the frame (Game does)
    // The three action buttons were queried, and both rows were offered as selectables.
    auto has = [](const std::vector<std::string>& v, const std::string& s) {
        return std::find(v.begin(), v.end(), s) != v.end();
    };
    CHECK(has(gui.buttons, "Refresh"));
    CHECK(has(gui.buttons, "Direct Connect"));
    CHECK(has(gui.buttons, "Cancel"));
    CHECK(gui.selectables.size() == 2u);
}

TEST_CASE("ServerBrowserScreen: clicking a row prefills + navigates to JoinServer (#143)", "[server_browser_screen]") {
    NullGui gui;
    MockInput input;
    MockWindow window;
    std::vector<BrowserRow> rows{row("Alpha", "1.2.3.4", 4778, 3)};

    std::optional<std::pair<std::string, uint16_t>> joined;
    ServerBrowserScreen::Deps d;
    d.gui = &gui;
    d.rows = &rows;
    d.onJoin = [&](const std::string& host, uint16_t port) { joined = {host, port}; };
    ServerBrowserScreen scr(std::move(d));

    // Script the first selectable (its label starts with "Alpha") to click.
    // The label is composed, so match by prefix via the scripted map's exact label after a dry run.
    gui.clear();
    scr.update(input, window, kTestFrameDtS); // records the selectable labels
    REQUIRE(gui.selectables.size() == 1u);
    gui.selectableClicks[gui.selectables[0]] = true;

    const Screen next = scr.update(input, window, kTestFrameDtS);
    CHECK(next == Screen::JoinServer);
    REQUIRE(joined.has_value());
    CHECK(joined->first == "1.2.3.4");
    CHECK(joined->second == 4778);
}

TEST_CASE("ServerBrowserScreen: Refresh fires the callback; Cancel + Esc leave (#143)", "[server_browser_screen]") {
    NullGui gui;
    MockInput input;
    MockWindow window;
    std::vector<BrowserRow> rows;

    int refreshes = 0;
    ServerBrowserScreen::Deps d;
    d.gui = &gui;
    d.rows = &rows;
    d.onRefresh = [&]() { ++refreshes; };
    ServerBrowserScreen scr(std::move(d));

    gui.buttonClicks["Refresh"] = true;
    scr.update(input, window, kTestFrameDtS);
    CHECK(refreshes == 1);

    gui.buttonClicks["Refresh"] = false;
    gui.buttonClicks["Cancel"] = true;
    CHECK(scr.update(input, window, kTestFrameDtS) == Screen::MainMenu);

    // Keyboard fallback: Escape leaves too.
    gui.buttonClicks["Cancel"] = false;
    MockInput esc;
    esc.justPressed.insert(Key::Escape);
    CHECK(scr.update(esc, window, kTestFrameDtS) == Screen::MainMenu);
}
