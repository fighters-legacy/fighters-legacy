// SPDX-License-Identifier: GPL-3.0-or-later
//
// Contract test for the IGui HAL via the scripted NullGui (#156). NullGui is the seam every UI-screen
// unit test drives, so this pins its recording + scripted-response behavior: a screen emits the widget
// vocabulary, the mock records what was emitted and returns programmed click/edit results, and the
// screen's resulting state is asserted. The real Dear ImGui backend (platform-gui) is exercised by the
// game build + visual verification (no GPU in CI).

#include "mock_gui.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstring>

using namespace fl;

TEST_CASE("NullGui records the widget vocabulary a screen emits") {
    NullGui gui;
    gui.newFrame();
    REQUIRE(gui.beginWindow("Join Server", 0.3f, 0.3f, 0.4f, 0.4f));
    gui.label("Address");
    (void)gui.button("Connect");
    (void)gui.button("Cancel");
    gui.endWindow();
    gui.render();

    CHECK(gui.newFrameCount == 1);
    CHECK(gui.renderCount == 1);
    REQUIRE(gui.windows.size() == 1);
    CHECK(gui.windows[0] == "Join Server");
    REQUIRE(gui.labels.size() == 1);
    CHECK(gui.labels[0] == "Address");
    REQUIRE(gui.buttons.size() == 2);
    CHECK(gui.buttons[0] == "Connect");
    CHECK(gui.buttons[1] == "Cancel");
}

TEST_CASE("NullGui returns scripted button and selectable clicks") {
    NullGui gui;
    gui.buttonClicks["Connect"] = true;
    gui.selectableClicks["server-2"] = true;

    CHECK(gui.button("Connect"));      // scripted true
    CHECK_FALSE(gui.button("Cancel")); // not scripted → false
    CHECK(gui.selectable("server-2", false));
    CHECK_FALSE(gui.selectable("server-1", false));
}

TEST_CASE("NullGui checkbox toggles the bound value once (#838)") {
    NullGui gui;
    gui.checkboxToggles["Wireframe"] = true;

    bool wire = false;
    CHECK(gui.checkbox("Wireframe", &wire)); // scripted toggle -> returns true, sets the value
    CHECK(wire);
    CHECK_FALSE(gui.checkbox("Wireframe", &wire)); // one-shot: consumed, no further toggle
    CHECK_FALSE(gui.checkbox("Normals", &wire));   // not scripted
    CHECK(std::find(gui.checkboxes.begin(), gui.checkboxes.end(), "Wireframe") != gui.checkboxes.end());
}

TEST_CASE("NullGui treeNode open/leaf/selection and balanced depth (#838)") {
    NullGui gui;
    gui.treeOpen["root"] = true;    // an open non-leaf
    gui.treeClicks["child"] = true; // a clicked leaf

    // Open non-leaf pushes the tree stack (depth +1); the caller must treePop().
    CHECK(gui.treeNode("root", "Root", nullptr, /*leaf=*/false));
    CHECK(gui.treeDepth == 1);

    // A leaf never opens and never pushes; its click sets *selected.
    bool sel = false;
    CHECK_FALSE(gui.treeNode("child", "Child", &sel, /*leaf=*/true));
    CHECK(sel);
    CHECK(gui.treeDepth == 1); // unchanged by the leaf

    gui.treePop();
    CHECK(gui.treeDepth == 0); // balanced

    // A closed non-leaf returns false and does not push.
    CHECK_FALSE(gui.treeNode("other", "Other", nullptr, /*leaf=*/false));
    CHECK(gui.treeDepth == 0);
}

TEST_CASE("NullGui inputText copies a queued value once, then reports no change") {
    NullGui gui;
    gui.inputTextValues["Address"] = "10.0.0.5:4780";

    std::array<char, 64> buf{};
    // First query: the queued value is written and the field reports "changed".
    CHECK(gui.inputText("Address", buf.data(), buf.size()));
    CHECK(std::string(buf.data()) == "10.0.0.5:4780");
    // Second query: consumed — buffer untouched, no change reported.
    CHECK_FALSE(gui.inputText("Address", buf.data(), buf.size()));
    CHECK(std::string(buf.data()) == "10.0.0.5:4780");
}

TEST_CASE("NullGui inputText respects the buffer capacity") {
    NullGui gui;
    gui.inputTextValues["cs"] = "abcdefghij"; // 10 chars
    std::array<char, 5> buf{};                // room for 4 + NUL
    CHECK(gui.inputText("cs", buf.data(), buf.size()));
    CHECK(std::string(buf.data()) == "abcd");
}

TEST_CASE("NullGui records table structure and honors clear() between frames") {
    NullGui gui;
    const std::array<std::string_view, 3> headers{"Name", "Kills", "Ping"};

    gui.newFrame();
    REQUIRE(gui.beginTable("scoreboard", 3));
    gui.tableHeadersRow(headers);
    gui.tableNextRow();
    gui.tableCell("Viper");
    gui.tableCell("3");
    gui.tableCell("42");
    gui.endTable();
    gui.render();

    REQUIRE(gui.headers.size() == 3);
    CHECK(gui.headers[1] == "Kills");
    CHECK(gui.rowCount == 1);
    REQUIRE(gui.cells.size() == 3);
    CHECK(gui.cells[0] == "Viper");
    CHECK(gui.cells[2] == "42");

    // A new simulated frame: clear() resets recorded emissions but keeps scripted state.
    gui.clear();
    CHECK(gui.headers.empty());
    CHECK(gui.cells.empty());
    CHECK(gui.rowCount == 0);
    CHECK(gui.newFrameCount == 1); // lifecycle counters are NOT reset by clear()
}

TEST_CASE("NullGui reports the scripted input-capture flags") {
    NullGui gui;
    CHECK_FALSE(gui.wantCaptureKeyboard());
    CHECK_FALSE(gui.wantCaptureMouse());
    gui.captureKeyboard = true;
    gui.captureMouse = true;
    CHECK(gui.wantCaptureKeyboard());
    CHECK(gui.wantCaptureMouse());
}
