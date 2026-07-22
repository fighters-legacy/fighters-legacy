// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for the in-match ChatOverlay (#646): the display ring (deterministic clock) and the IGui
// input box (scripted NullGui).

#include "ChatOverlay.h"

#include "IClock.h"
#include "mock_gui.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>

using namespace fl;

namespace {
bool anyLineContains(std::span<const HudElement> els, const std::string& needle) {
    return std::any_of(els.begin(), els.end(), [&](const HudElement& e) {
        return std::string_view(e.text).find(needle) != std::string_view::npos;
    });
}
} // namespace

TEST_CASE("ChatOverlay: pushLine renders bottom-left, left-aligned", "[chat_overlay]") {
    ManualClock clock;
    ChatOverlay chat;
    chat.setClock(clock);
    chat.pushLine("Maverick", "hello all", ChatChannel::All);
    auto els = chat.buildElements();
    REQUIRE(els.size() == 1u);
    CHECK(els[0].align == HudAlign::Left);
    CHECK(els[0].x < 0.1f);
    CHECK(anyLineContains(els, "Maverick"));
    CHECK(anyLineContains(els, "hello all"));
}

TEST_CASE("ChatOverlay: team lines carry a [Team] tag; system lines have no name", "[chat_overlay]") {
    ManualClock clock;
    ChatOverlay chat;
    chat.setClock(clock);
    chat.pushLine("Iceman", "on your six", ChatChannel::Team);
    chat.pushLine("", "Player joined", ChatChannel::All, /*system=*/true);
    auto els = chat.buildElements();
    REQUIRE(els.size() == 2u);
    CHECK(anyLineContains(els, "[Team]"));
    CHECK(anyLineContains(els, "Player joined"));
    // The system line does not render a "name:" prefix.
    CHECK_FALSE(anyLineContains(els, ": Player joined"));
}

TEST_CASE("ChatOverlay: a muted callsign is dropped at push", "[chat_overlay]") {
    ManualClock clock;
    ChatOverlay chat;
    chat.setClock(clock);
    chat.muteCallsign("Spammer");
    CHECK(chat.isMuted("Spammer"));
    chat.pushLine("Spammer", "buy gold", ChatChannel::All);
    chat.pushLine("Maverick", "hi", ChatChannel::All);
    auto els = chat.buildElements();
    CHECK(els.size() == 1u);
    CHECK(anyLineContains(els, "Maverick"));
    CHECK_FALSE(anyLineContains(els, "Spammer"));
    // Unmute restores.
    chat.unmuteCallsign("Spammer");
    CHECK_FALSE(chat.isMuted("Spammer"));
}

TEST_CASE("ChatOverlay: lines expire after their lifetime", "[chat_overlay]") {
    ManualClock clock;
    ChatOverlay chat;
    chat.setClock(clock);
    chat.pushLine("A", "temp", ChatChannel::All);
    CHECK(chat.buildElements().size() == 1u);
    clock.advance(std::chrono::seconds(20));
    CHECK(chat.buildElements().empty());
}

TEST_CASE("ChatOverlay: ring caps at kMaxLines", "[chat_overlay]") {
    ManualClock clock;
    ChatOverlay chat;
    chat.setClock(clock);
    for (std::size_t i = 0; i < ChatOverlay::kMaxLines + 4; ++i)
        chat.pushLine("P", "line " + std::to_string(i), ChatChannel::All);
    CHECK(chat.buildElements().size() == ChatOverlay::kMaxLines);
    // The oldest line was dropped.
    CHECK_FALSE(anyLineContains(chat.buildElements(), "line 0"));
}

TEST_CASE("ChatOverlay: input box opens, edits, and sends via NullGui", "[chat_overlay]") {
    ChatOverlay chat;
    NullGui gui;
    CHECK_FALSE(chat.isInputOpen());
    CHECK_FALSE(chat.renderInput(&gui)); // closed: no-op, no window

    chat.open(ChatChannel::Team);
    CHECK(chat.isInputOpen());
    CHECK(chat.channel() == ChatChannel::Team);

    // Script the text edit + a Send click.
    gui.inputTextValues["##chat"] = "ready to engage";
    gui.buttonClicks["Send"] = true;
    const bool send = chat.renderInput(&gui);
    CHECK(send);
    CHECK(chat.text() == "ready to engage");
    CHECK(std::find(gui.windows.begin(), gui.windows.end(), "Chat") != gui.windows.end());

    chat.submit();
    CHECK_FALSE(chat.isInputOpen());
    CHECK(chat.text().empty());
}

TEST_CASE("ChatOverlay: cancel closes and clears without sending", "[chat_overlay]") {
    ChatOverlay chat;
    NullGui gui;
    chat.open(ChatChannel::All);
    gui.inputTextValues["##chat"] = "never mind";
    chat.renderInput(&gui);
    CHECK(chat.text() == "never mind");
    chat.cancel();
    CHECK_FALSE(chat.isInputOpen());
    CHECK(chat.text().empty());
}
