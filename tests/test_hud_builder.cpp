// SPDX-License-Identifier: GPL-3.0-or-later
//
// fl::HudBuilder (#1261).
//
// FlightHud and GameConsole each carried a near-identical pushText/pushLine/pushRect trio -- the
// vsnprintf into a string arena, the capacity check, the field-by-field fill -- and eight screens
// carried the same thirteen-line fullscreen-background block verbatim.
//
// The two properties worth pinning are the ones the copies got subtly differently: every element
// starts DEFAULT-CONSTRUCTED (GameConsole filled a reused slot in place), and the string arena
// keeps formatted text alive for the frame, because HudElement::text is a string_view.

#include "render/HudBuilder.h"

#include <catch2/catch_test_macros.hpp>

using namespace fl;

TEST_CASE("hudFullscreenBg covers the screen and defaults to opaque black", "[hud_builder]") {
    const HudElement bg = hudFullscreenBg();
    CHECK(bg.type == HudElement::Type::Rect);
    CHECK(bg.x == 0.f);
    CHECK(bg.y == 0.f);
    CHECK(bg.x2 == 1.f);
    CHECK(bg.y2 == 1.f);
    CHECK(bg.r == 0.f);
    CHECK(bg.g == 0.f);
    CHECK(bg.b == 0.f);
    CHECK(bg.a == 1.f);

    // The colour is a parameter because one screen genuinely renders a white background.
    const HudElement white = hudFullscreenBg(1.f, 1.f, 1.f, 1.f);
    CHECK(white.r == 1.f);
    CHECK(white.b == 1.f);
}

TEST_CASE("formatted text is copied into the arena and stays valid", "[hud_builder]") {
    HudBuilder<8, 4> b;
    {
        // The formatted value goes out of scope here. HudElement::text is a string_view, so if the
        // builder did not own a copy this would dangle -- which is the rule five call sites used to
        // re-document individually.
        const int mach = 92;
        REQUIRE(b.text(HudAlign::Center, 0.5f, 0.1f, 1.f, 1.f, 1.f, "M %d", mach));
    }
    REQUIRE(b.elements().size() == 1u);
    CHECK(b.elements()[0].text == "M 92");
    CHECK(b.elements()[0].align == HudAlign::Center);
    CHECK(b.elements()[0].type == HudElement::Type::Text);
}

TEST_CASE("textView does not consume an arena slot", "[hud_builder]") {
    HudBuilder<8, 1> b; // room for exactly ONE formatted string
    static const std::string stable = "PAUSED";

    REQUIRE(b.text(HudAlign::Left, 0.f, 0.f, 1.f, 1.f, 1.f, "%s", "one"));
    // A second formatted string would overflow the arena...
    CHECK_FALSE(b.text(HudAlign::Left, 0.f, 0.1f, 1.f, 1.f, 1.f, "%s", "two"));
    CHECK(b.overflowed());

    // ...but a caller-owned view needs no slot, which is the whole point of the second entry point.
    HudBuilder<8, 1> c;
    REQUIRE(c.text(HudAlign::Left, 0.f, 0.f, 1.f, 1.f, 1.f, "%s", "one"));
    REQUIRE(c.textView(HudAlign::Left, 0.f, 0.1f, 1.f, 1.f, 1.f, stable));
    CHECK_FALSE(c.overflowed());
    CHECK(c.elements()[1].text == "PAUSED");
}

TEST_CASE("every element starts default-constructed", "[hud_builder]") {
    // GameConsole filled a REUSED slot in place, so stale fields survived across frames -- harmless
    // only because console elements never set align and Text ignores x2/y2. A rect followed by text
    // in the same slot index is exactly that case.
    HudBuilder<4, 2> b;
    REQUIRE(b.rect(0.1f, 0.2f, 0.9f, 0.8f, 0.f, 0.f, 0.f, 0.5f));
    b.clear();
    REQUIRE(b.text(HudAlign::Left, 0.f, 0.f, 1.f, 1.f, 1.f, "hi"));

    const HudElement& el = b.elements()[0];
    CHECK(el.type == HudElement::Type::Text);
    CHECK(el.x2 == 0.f); // not 0.9 left over from the rect
    CHECK(el.y2 == 0.f);
    CHECK(el.a == 1.f); // not 0.5
    CHECK(el.scale == 1.f);
    CHECK(el.align == HudAlign::Left);
}

TEST_CASE("overflow is reported rather than silently truncating", "[hud_builder]") {
    HudBuilder<2, 4> b;
    REQUIRE(b.rect(0.f, 0.f, 1.f, 1.f, 0.f, 0.f, 0.f, 1.f));
    REQUIRE(b.line(0.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f));
    CHECK_FALSE(b.overflowed());

    CHECK_FALSE(b.rect(0.f, 0.f, 1.f, 1.f, 0.f, 0.f, 0.f, 1.f));
    CHECK(b.overflowed());
    CHECK(b.elements().size() == 2u); // the cap held

    b.clear();
    CHECK_FALSE(b.overflowed()); // clear resets the flag with the counts
    CHECK(b.elements().empty());
}

TEST_CASE("append copies a sub-widget's elements in order", "[hud_builder]") {
    // The flight screen appends sub-menu spans, and hudBox returns four lines at once.
    const std::array<HudElement, 2> sub{hudFullscreenBg(0.1f, 0.f, 0.f, 1.f), hudFullscreenBg(0.2f, 0.f, 0.f, 1.f)};
    HudBuilder<4, 1> b;
    REQUIRE(b.append(sub));
    REQUIRE(b.elements().size() == 2u);
    CHECK(b.elements()[0].r == 0.1f);
    CHECK(b.elements()[1].r == 0.2f);
}
