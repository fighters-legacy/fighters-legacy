// SPDX-License-Identifier: GPL-3.0-or-later
#include "Utf8Decode.h"

#include <catch2/catch_test_macros.hpp>
#include <string_view>

using fl::nextUtf8Codepoint;

static uint32_t decode1(std::string_view s) {
    const char* p = s.data();
    return nextUtf8Codepoint(p, p + s.size());
}

// ---------------------------------------------------------------------------
// ASCII (1-byte sequences, U+0000-U+007F)
// ---------------------------------------------------------------------------

TEST_CASE("Utf8Decode: ASCII NUL decodes to U+0000", "[utf8]") {
    const char buf[] = {'\x00'};
    const char* p = buf;
    CHECK(nextUtf8Codepoint(p, buf + 1) == 0x0000u);
    CHECK(p == buf + 1);
}

TEST_CASE("Utf8Decode: ASCII printable range", "[utf8]") {
    CHECK(decode1(" ") == 0x0020u);
    CHECK(decode1("A") == 0x0041u);
    CHECK(decode1("z") == 0x007Au);
    CHECK(decode1("\x7F") == 0x007Fu);
}

TEST_CASE("Utf8Decode: ASCII advances pointer by one byte", "[utf8]") {
    const char buf[] = "AB";
    const char* p = buf;
    CHECK(nextUtf8Codepoint(p, buf + 2) == 0x0041u); // A
    CHECK(nextUtf8Codepoint(p, buf + 2) == 0x0042u); // B
    CHECK(p == buf + 2);
}

// ---------------------------------------------------------------------------
// 2-byte sequences (U+0080-U+07FF)
// ---------------------------------------------------------------------------

TEST_CASE("Utf8Decode: U+00B0 DEGREE SIGN (2-byte)", "[utf8]") {
    // UTF-8: 0xC2 0xB0
    CHECK(decode1("\xC2\xB0") == 0x00B0u);
}

TEST_CASE("Utf8Decode: U+00B1 PLUS-MINUS SIGN (2-byte)", "[utf8]") {
    CHECK(decode1("\xC2\xB1") == 0x00B1u);
}

TEST_CASE("Utf8Decode: U+07FF last 2-byte codepoint", "[utf8]") {
    // U+07FF = 0xDF 0xBF
    CHECK(decode1("\xDF\xBF") == 0x07FFu);
}

TEST_CASE("Utf8Decode: 2-byte sequence advances pointer by two bytes", "[utf8]") {
    const char buf[] = "\xC2\xB0\x41"; // ° A
    const char* p = buf;
    CHECK(nextUtf8Codepoint(p, buf + 3) == 0x00B0u);
    CHECK(nextUtf8Codepoint(p, buf + 3) == 0x0041u);
    CHECK(p == buf + 3);
}

// ---------------------------------------------------------------------------
// 3-byte sequences (U+0800-U+FFFF)
// ---------------------------------------------------------------------------

TEST_CASE("Utf8Decode: U+2591 LIGHT SHADE (3-byte)", "[utf8]") {
    // UTF-8: 0xE2 0x96 0x91
    CHECK(decode1("\xE2\x96\x91") == 0x2591u);
}

TEST_CASE("Utf8Decode: U+2592 MEDIUM SHADE (3-byte)", "[utf8]") {
    CHECK(decode1("\xE2\x96\x92") == 0x2592u);
}

TEST_CASE("Utf8Decode: U+2593 DARK SHADE (3-byte)", "[utf8]") {
    CHECK(decode1("\xE2\x96\x93") == 0x2593u);
}

TEST_CASE("Utf8Decode: U+2588 FULL BLOCK (3-byte)", "[utf8]") {
    CHECK(decode1("\xE2\x96\x88") == 0x2588u);
}

TEST_CASE("Utf8Decode: U+FFFF last BMP codepoint (3-byte)", "[utf8]") {
    // U+FFFF = 0xEF 0xBF 0xBF
    CHECK(decode1("\xEF\xBF\xBF") == 0xFFFFu);
}

TEST_CASE("Utf8Decode: 3-byte sequence advances pointer by three bytes", "[utf8]") {
    const char buf[] = "\xE2\x96\x91\x41"; // ░ A
    const char* p = buf;
    CHECK(nextUtf8Codepoint(p, buf + 4) == 0x2591u);
    CHECK(nextUtf8Codepoint(p, buf + 4) == 0x0041u);
    CHECK(p == buf + 4);
}

// ---------------------------------------------------------------------------
// 4-byte sequences (U+10000+) — BMP-only decoder returns U+FFFD
// ---------------------------------------------------------------------------

TEST_CASE("Utf8Decode: 4-byte sequence returns U+FFFD (out of BMP)", "[utf8]") {
    // U+1F600 GRINNING FACE = 0xF0 0x9F 0x98 0x80
    const char buf[] = "\xF0\x9F\x98\x80";
    const char* p = buf;
    CHECK(nextUtf8Codepoint(p, buf + 4) == 0xFFFDu);
    CHECK(p == buf + 4); // pointer fully consumed
}

// ---------------------------------------------------------------------------
// Invalid sequences
// ---------------------------------------------------------------------------

TEST_CASE("Utf8Decode: bare continuation byte returns U+FFFD", "[utf8]") {
    const char buf[] = {'\x80'};
    const char* p = buf;
    CHECK(nextUtf8Codepoint(p, buf + 1) == 0xFFFDu);
    CHECK(p == buf + 1);
}

TEST_CASE("Utf8Decode: lead byte with missing continuation returns U+FFFD", "[utf8]") {
    // 0xC2 starts a 2-byte sequence but buffer ends — truncated
    const char buf[] = {'\xC2'};
    const char* p = buf;
    CHECK(nextUtf8Codepoint(p, buf + 1) == 0xFFFDu);
}

TEST_CASE("Utf8Decode: 3-byte sequence truncated after first continuation returns U+FFFD", "[utf8]") {
    const char buf[] = {'\xE2', '\x96'}; // missing third byte
    const char* p = buf;
    CHECK(nextUtf8Codepoint(p, buf + 2) == 0xFFFDu);
}

TEST_CASE("Utf8Decode: invalid lead byte 0xFF returns U+FFFD", "[utf8]") {
    const char buf[] = {'\xFF', '\x41'}; // 0xFF then A
    const char* p = buf;
    CHECK(nextUtf8Codepoint(p, buf + 2) == 0xFFFDu);
    // Pointer should have advanced past the invalid byte; A still decodeable.
    CHECK(nextUtf8Codepoint(p, buf + 2) == 0x0041u);
}

TEST_CASE("Utf8Decode: continuation bytes after invalid lead are skipped", "[utf8]") {
    // 0xFF followed by two continuation bytes, then ASCII 'X'
    const char buf[] = {'\xFF', '\x80', '\x80', '\x58'};
    const char* p = buf;
    CHECK(nextUtf8Codepoint(p, buf + 4) == 0xFFFDu);
    // The two continuation bytes were consumed as part of the invalid sequence.
    CHECK(nextUtf8Codepoint(p, buf + 4) == 0x0058u); // X
    CHECK(p == buf + 4);
}
