// SPDX-License-Identifier: GPL-3.0-or-later
#include "CommsMenu.h"

#include "mock_hal.h"

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string>

using namespace fl;

TEST_CASE("CommsMenu: toggles open and closed", "[comms_menu]") {
    CommsMenu menu;
    CHECK_FALSE(menu.isOpen());
    menu.toggle();
    CHECK(menu.isOpen());
    CHECK(menu.page() == static_cast<int>(0)); // root
    menu.toggle();
    CHECK_FALSE(menu.isOpen());
}

TEST_CASE("CommsMenu: closed menu consumes no input and emits nothing", "[comms_menu]") {
    CommsMenu menu;
    MockInput in;
    in.justPressed.insert(Key::Num1);
    CHECK_FALSE(menu.update(in).has_value());
    CHECK(menu.buildElements().empty());
}

TEST_CASE("CommsMenu: root ATC item descends to the ATC page without emitting a command", "[comms_menu]") {
    CommsMenu menu;
    menu.toggle();
    MockInput in;
    in.justPressed.insert(Key::Num1); // "ATC" on the root page
    auto cmd = menu.update(in);
    CHECK_FALSE(cmd.has_value());
    CHECK(menu.isOpen());
    CHECK(menu.page() != 0); // now on the ATC page
}

TEST_CASE("CommsMenu: selecting an ATC command emits the verb string and closes", "[comms_menu]") {
    CommsMenu menu;
    menu.toggle();
    // Descend to ATC.
    {
        MockInput in;
        in.justPressed.insert(Key::Num1);
        menu.update(in);
    }
    // Pick "Request landing" (item 2).
    MockInput in;
    in.justPressed.insert(Key::Num2);
    auto cmd = menu.update(in);
    REQUIRE(cmd.has_value());
    CHECK(std::string(cmd->command) == "atc request_landing");
    CHECK_FALSE(menu.isOpen()); // a command closes the menu
}

TEST_CASE("CommsMenu: Escape backs out a page, then closes at the root", "[comms_menu]") {
    CommsMenu menu;
    menu.toggle();
    {
        MockInput in;
        in.justPressed.insert(Key::Num1); // -> ATC page
        menu.update(in);
    }
    CHECK(menu.page() != 0);
    {
        MockInput in;
        in.justPressed.insert(Key::Escape); // back to root
        menu.update(in);
    }
    CHECK(menu.isOpen());
    CHECK(menu.page() == 0);
    {
        MockInput in;
        in.justPressed.insert(Key::Escape); // close
        menu.update(in);
    }
    CHECK_FALSE(menu.isOpen());
}

TEST_CASE("CommsMenu: the Flight placeholder emits nothing and keeps the menu open", "[comms_menu]") {
    CommsMenu menu;
    menu.toggle();
    MockInput in;
    in.justPressed.insert(Key::Num3); // "Flight (coming soon)" on the root page (slot 3 since #55)
    auto cmd = menu.update(in);
    CHECK_FALSE(cmd.has_value());
    CHECK(menu.isOpen());
    CHECK(menu.page() == 0);
}

TEST_CASE("CommsMenu: the Ground crew page emits the base-ops radio verbs (#55)", "[comms_menu]") {
    CommsMenu menu;
    menu.toggle();
    {
        MockInput in;
        in.justPressed.insert(Key::Num2); // root -> Ground crew
        CHECK_FALSE(menu.update(in).has_value());
        CHECK(menu.page() == 2);
    }
    {
        MockInput in;
        in.justPressed.insert(Key::Num1); // Refuel
        auto cmd = menu.update(in);
        REQUIRE(cmd.has_value());
        CHECK(std::string(cmd->command) == "base refuel");
        CHECK_FALSE(menu.isOpen()); // a sent command closes the menu
    }
}

TEST_CASE("CommsMenu: arrow navigation + Enter selects the highlighted item", "[comms_menu]") {
    CommsMenu menu;
    menu.toggle();
    {
        MockInput in;
        in.justPressed.insert(Key::Num1); // -> ATC page, selection 0
        menu.update(in);
    }
    {
        MockInput in;
        in.justPressed.insert(Key::ArrowDown); // select item 1 (Request landing)
        menu.update(in);
    }
    CHECK(menu.selected() == 1);
    MockInput in;
    in.justPressed.insert(Key::Enter);
    auto cmd = menu.update(in);
    REQUIRE(cmd.has_value());
    CHECK(std::string(cmd->command) == "atc request_landing");
}

TEST_CASE("CommsMenu: renders a title + item rows when open", "[comms_menu]") {
    CommsMenu menu;
    menu.toggle();
    auto els = menu.buildElements();
    CHECK(els.size() >= 3); // title + 2 root items
    CHECK(els[0].type == HudElement::Type::Text);
}
