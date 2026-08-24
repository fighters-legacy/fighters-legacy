// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for the multiplayer JoinServerScreen (#322) driven against the scripted NullGui (#156) and
// the HAL mocks — no window, GPU, or network. This is the pattern every IGui screen test follows: script
// the widget results, run update() once, assert the emitted vocabulary + the resulting Screen + the
// connection handed back through the onConnect callback.

#include "JoinServerScreen.h"

#include "mock_gui.h"
#include "mock_hal.h"

#include <catch2/catch_test_macros.hpp>

#include <optional>

using namespace fl;

// These suites drive the screen a fixed step at a time; nothing here is rate-dependent (#1241).
constexpr float kTestFrameDtS = 1.f / 60.f;

namespace {
struct Harness {
    NullGui gui;
    MockInput input;
    MockWindow window;
    std::optional<JoinServerScreen::Result> connected;

    JoinServerScreen make(const std::string& host = "", const std::string& callsign = "") {
        JoinServerScreen::Deps d;
        d.gui = &gui;
        d.initialHost = host;
        d.initialCallsign = callsign;
        d.onConnect = [this](const JoinServerScreen::Result& r) { connected = r; };
        return JoinServerScreen(std::move(d));
    }
};
} // namespace

TEST_CASE("JoinServerScreen prefills host + callsign and emits the form") {
    Harness h;
    auto screen = h.make("192.168.1.10:4780", "Maverick");

    CHECK(std::string(screen.address()) == "192.168.1.10:4780");
    CHECK(std::string(screen.callsign()) == "Maverick");

    const Screen next = screen.update(h.input, h.window, kTestFrameDtS);
    CHECK(next == Screen::JoinServer); // no action yet → stays on the form
    // The form window + its three fields' labels + both buttons were emitted.
    REQUIRE(h.gui.windows.size() == 1);
    CHECK(h.gui.windows[0] == "Join Server");
    REQUIRE(h.gui.buttons.size() == 2);
    CHECK(h.gui.buttons[0] == "Connect");
    CHECK(h.gui.buttons[1] == "Cancel");
    CHECK_FALSE(h.connected.has_value());
}

TEST_CASE("JoinServerScreen Connect button parses the address and hands back the connection") {
    Harness h;
    auto screen = h.make();
    h.gui.inputTextValues["##address"] = "example.com:5000";
    h.gui.inputTextValues["##password"] = "hunter2";
    h.gui.inputTextValues["##callsign"] = "Iceman";
    h.gui.buttonClicks["Connect"] = true;

    const Screen next = screen.update(h.input, h.window, kTestFrameDtS);

    CHECK(next == Screen::Loading);
    REQUIRE(h.connected.has_value());
    CHECK(h.connected->host == "example.com");
    CHECK(h.connected->port == 5000);
    CHECK(h.connected->joinPassword == "hunter2");
    CHECK(h.connected->callsign == "Iceman");
}

TEST_CASE("JoinServerScreen host without a port keeps the default port") {
    Harness h;
    auto screen = h.make();
    h.gui.inputTextValues["##address"] = "myserver.example";
    h.gui.buttonClicks["Connect"] = true;

    const Screen next = screen.update(h.input, h.window, kTestFrameDtS);
    CHECK(next == Screen::Loading);
    REQUIRE(h.connected.has_value());
    CHECK(h.connected->host == "myserver.example");
    CHECK(h.connected->port == 4778); // default game port
    CHECK(h.connected->joinPassword.empty());
}

TEST_CASE("JoinServerScreen Enter key confirms; empty address stays on the form") {
    Harness h;
    auto screen = h.make();
    // No address entered; Enter pressed.
    h.input.justPressed.insert(Key::Enter);
    const Screen next = screen.update(h.input, h.window, kTestFrameDtS);
    CHECK(next == Screen::JoinServer); // empty host → not a valid connection, stay
    CHECK_FALSE(h.connected.has_value());
}

TEST_CASE("JoinServerScreen Enter key confirms a valid prefilled address") {
    Harness h;
    auto screen = h.make("10.0.0.5", "");
    h.input.justPressed.insert(Key::Enter);
    const Screen next = screen.update(h.input, h.window, kTestFrameDtS);
    CHECK(next == Screen::Loading);
    REQUIRE(h.connected.has_value());
    CHECK(h.connected->host == "10.0.0.5");
}

TEST_CASE("JoinServerScreen Cancel button and Escape return to the main menu") {
    {
        Harness h;
        auto screen = h.make("1.2.3.4", "");
        h.gui.buttonClicks["Cancel"] = true;
        CHECK(screen.update(h.input, h.window, kTestFrameDtS) == Screen::MainMenu);
        CHECK_FALSE(h.connected.has_value());
    }
    {
        Harness h;
        auto screen = h.make("1.2.3.4", "");
        h.input.justPressed.insert(Key::Escape);
        CHECK(screen.update(h.input, h.window, kTestFrameDtS) == Screen::MainMenu);
        CHECK_FALSE(h.connected.has_value());
    }
}

TEST_CASE("JoinServerScreen with no GUI backend still cancels via Escape (never a dead end)") {
    Harness h;
    JoinServerScreen::Deps d;
    d.gui = nullptr; // ImGui backend unavailable
    JoinServerScreen screen(std::move(d));
    h.input.justPressed.insert(Key::Escape);
    CHECK(screen.update(h.input, h.window, kTestFrameDtS) == Screen::MainMenu);
}
