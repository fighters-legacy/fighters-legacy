// SPDX-License-Identifier: GPL-3.0-or-later
#include "RenderTypes.h"
#include "Utf8Decode.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using Catch::Approx;

TEST_CASE("HudElement defaults to left alignment", "[hud][align]") {
    fl::HudElement el{};
    CHECK(el.align == fl::HudAlign::Left);
}

TEST_CASE("hudAlignOffsetPx: Left is zero", "[hud][align]") {
    CHECK(fl::hudAlignOffsetPx(fl::HudAlign::Left, 0.f) == Approx(0.f));
    CHECK(fl::hudAlignOffsetPx(fl::HudAlign::Left, 128.f) == Approx(0.f));
}

TEST_CASE("hudAlignOffsetPx: Center shifts by half the width", "[hud][align]") {
    CHECK(fl::hudAlignOffsetPx(fl::HudAlign::Center, 0.f) == Approx(0.f));
    CHECK(fl::hudAlignOffsetPx(fl::HudAlign::Center, 128.f) == Approx(-64.f));
}

TEST_CASE("hudAlignOffsetPx: Right shifts by the full width", "[hud][align]") {
    CHECK(fl::hudAlignOffsetPx(fl::HudAlign::Right, 0.f) == Approx(0.f));
    CHECK(fl::hudAlignOffsetPx(fl::HudAlign::Right, 128.f) == Approx(-128.f));
}

TEST_CASE("Glyph-cell width composes with codepoint count and scale", "[hud][align]") {
    // The renderer's text width is codepoints * kHudGlyphWidthPx * scale.
    const auto count = fl::countUtf8Codepoints("HELLO");
    const float widthPx = static_cast<float>(count) * fl::kHudGlyphWidthPx * 2.0f;
    CHECK(widthPx == Approx(80.f));
    CHECK(fl::hudAlignOffsetPx(fl::HudAlign::Center, widthPx) == Approx(-40.f));
}
