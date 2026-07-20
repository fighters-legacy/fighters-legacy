// SPDX-License-Identifier: GPL-3.0-or-later
#include "RenderTypes.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

using namespace fl;

TEST_CASE("captureSwizzleToRgba passes RGBA through unchanged", "[capture]") {
    // Two pixels, source already RGBA. Alpha is forced opaque.
    std::array<uint8_t, 8> src{10, 20, 30, 7, 40, 50, 60, 3};
    std::array<uint8_t, 8> dst{};
    captureSwizzleToRgba(src.data(), 2, /*bgra=*/false, dst.data());
    CHECK(dst[0] == 10);
    CHECK(dst[1] == 20);
    CHECK(dst[2] == 30);
    CHECK(dst[3] == 255); // opaque
    CHECK(dst[4] == 40);
    CHECK(dst[5] == 50);
    CHECK(dst[6] == 60);
    CHECK(dst[7] == 255);
}

TEST_CASE("captureSwizzleToRgba swaps R and B for BGRA sources", "[capture]") {
    // One pixel B=10, G=20, R=30, A=7 -> RGBA R=30, G=20, B=10, A=255.
    std::array<uint8_t, 4> src{10, 20, 30, 7};
    std::array<uint8_t, 4> dst{};
    captureSwizzleToRgba(src.data(), 1, /*bgra=*/true, dst.data());
    CHECK(dst[0] == 30); // R came from src[2]
    CHECK(dst[1] == 20); // G passthrough
    CHECK(dst[2] == 10); // B came from src[0]
    CHECK(dst[3] == 255);
}

TEST_CASE("captureSwizzleToRgba handles zero pixels", "[capture]") {
    std::array<uint8_t, 4> dst{1, 2, 3, 4};
    captureSwizzleToRgba(nullptr, 0, false, dst.data());
    CHECK(dst[0] == 1); // untouched
}
