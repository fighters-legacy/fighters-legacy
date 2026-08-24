// SPDX-License-Identifier: GPL-3.0-or-later
//
// fl::parse* / fl::isAllDigits / fl::trim (#1244).
//
// Nine hand-rolled number parsers existed across the console, the admin commands, the AI factory,
// the airport importer, the campaign engine and the mission runtime. They existed for one real
// reason -- std::from_chars' floating-point overloads are missing on Apple Clang before macOS 13.3
// -- and each copy answered the questions that fallback raises differently: is trailing junk an
// error, is 1e999 an error, is this field a float or a double.
//
// These tests pin the answers, because the answers are what the callers depend on. The strict form
// is the default; the tolerant form exists for exactly two callers and says so in its name.

#include "util/Parse.h"
#include "util/Str.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <string_view>

TEST_CASE("parseDouble takes the whole string or nothing", "[parse]") {
    CHECK(fl::parseDouble("1.5") == 1.5);
    CHECK(fl::parseDouble("-0.25") == -0.25);
    CHECK(fl::parseDouble("1e3") == 1000.0);
    CHECK(fl::parseDouble("0") == 0.0);

    // Trailing junk is an error, not a prefix read. This is the difference that mattered: the CSV
    // importer wanted "1500ft" to read as 1500 and the admin console did not, and both spellings
    // were called parseDouble.
    CHECK_FALSE(fl::parseDouble("1.5x").has_value());
    CHECK_FALSE(fl::parseDouble("1500ft").has_value());
    CHECK_FALSE(fl::parseDouble("").has_value());
    CHECK_FALSE(fl::parseDouble("abc").has_value());

    // strtod would skip a leading space and from_chars would not, so the float and integer forms
    // used to disagree about " 12". Both reject it now; a caller that wants the tolerance trims.
    CHECK_FALSE(fl::parseDouble(" 1.5").has_value());
    CHECK_FALSE(fl::parseDouble("1.5 ").has_value());
}

TEST_CASE("a value too large for the type is a failure, not an infinity", "[parse]") {
    // tp and detonate fed this straight into a world position. An entity teleported to infinity is
    // not a coordinate, and the old strtod copies accepted it.
    CHECK_FALSE(fl::parseDouble("1e999").has_value());
    CHECK_FALSE(fl::parseDouble("-1e999").has_value());
    CHECK_FALSE(fl::parseFloat("1e999").has_value());

    // A double that a float cannot hold is out of range for parseFloat but fine for parseDouble.
    CHECK_FALSE(fl::parseFloat("1e300").has_value());
    CHECK(fl::parseDouble("1e300").has_value());
}

TEST_CASE("parseFloat matches parseDouble on the values a float can hold", "[parse]") {
    CHECK(fl::parseFloat("24") == 24.f);
    CHECK(fl::parseFloat("0.5") == 0.5f);
    CHECK_FALSE(fl::parseFloat("24h").has_value());
    CHECK_FALSE(fl::parseFloat("").has_value());
}

TEST_CASE("the integer parsers are strict and reject a sign they cannot hold", "[parse]") {
    CHECK(fl::parseU32("0") == 0u);
    CHECK(fl::parseU32("4294967295") == 4294967295u);
    CHECK_FALSE(fl::parseU32("4294967296").has_value()); // one past the top
    CHECK_FALSE(fl::parseU32("-1").has_value());         // unsigned: not a wrap to 4294967295
    CHECK_FALSE(fl::parseU32("12abc").has_value());
    CHECK_FALSE(fl::parseU32("").has_value());
    CHECK_FALSE(fl::parseU32(" 12").has_value());

    CHECK(fl::parseI32("-7") == -7);
    CHECK(fl::parseU64("18446744073709551615") == std::numeric_limits<uint64_t>::max());
}

TEST_CASE("the tolerant parsers read a leading number and ignore the rest", "[parse]") {
    // The OurAirports importer: a field like "1500 ft" should import as 1500, not be dropped.
    CHECK(fl::parseLeadingDouble("1500ft") == 1500.0);
    CHECK(fl::parseLeadingDouble("1500 ft") == 1500.0);
    CHECK_FALSE(fl::parseLeadingDouble(" 1500").has_value()); // tolerant AFTER the number, not before
    CHECK(fl::parseLeadingDouble(fl::trim(" 1500 ")) == 1500.0);
    CHECK(fl::parseLeadingDouble("-12.5, 100") == -12.5);
    CHECK_FALSE(fl::parseLeadingDouble("ft1500").has_value()); // still needs a number FIRST
    CHECK_FALSE(fl::parseLeadingDouble("").has_value());

    // The campaign save fields: an unreadable value takes the caller's default.
    CHECK(fl::parseLeadingInt<int>("12abc") == 12);
    CHECK(fl::parseLeadingInt<int>("abc").value_or(-1) == -1);
    CHECK(fl::parseLeadingInt<uint64_t>("").value_or(99u) == 99u);
}

TEST_CASE("readInto assigns only on success", "[parse]") {
    double out = -1.0;
    CHECK(fl::readInto(fl::parseDouble("3.5"), out));
    CHECK(out == 3.5);

    // The failure path must leave the caller's variable alone: the command tables check the bool
    // and then reuse the same out-parameter for the next argument.
    CHECK_FALSE(fl::readInto(fl::parseDouble("nope"), out));
    CHECK(out == 3.5);
}

TEST_CASE("isAllDigits accepts only unsigned decimal digits", "[parse]") {
    CHECK(fl::isAllDigits("0"));
    CHECK(fl::isAllDigits("65535"));
    CHECK_FALSE(fl::isAllDigits(""));
    CHECK_FALSE(fl::isAllDigits("-1"));
    CHECK_FALSE(fl::isAllDigits("1.5"));
    CHECK_FALSE(fl::isAllDigits("12 "));
    CHECK_FALSE(fl::isAllDigits("1a"));
}

TEST_CASE("trim removes ASCII whitespace from both ends", "[parse]") {
    CHECK(fl::trim("  a b  ") == "a b");
    CHECK(fl::trim("\t\r\nx\n") == "x");
    CHECK(fl::trim("").empty());
    CHECK(fl::trim("   ").empty());
    CHECK(fl::trim("nospace") == "nospace");

    // Not std::isspace: a byte above 127 is not whitespace here, and must not be UB either.
    CHECK(fl::trim("\xC3\xA9") == "\xC3\xA9");
}

// ---------------------------------------------------------------------------
// ASCII case folding (#1265)
// ---------------------------------------------------------------------------

TEST_CASE("asciiLower folds letters and leaves everything else alone", "[parse]") {
    CHECK(fl::asciiLower("MiXeD") == "mixed");
    CHECK(fl::asciiLower("a/B_c-9.Ext") == "a/b_c-9.ext");
    CHECK(fl::asciiLower("") == "");
    CHECK(fl::asciiLower("already lower") == "already lower");
    // Digits and punctuation are not letters and must survive byte-for-byte: these strings are
    // asset ids and config values, not prose.
    CHECK(fl::asciiLower("0123!@#") == "0123!@#");
}

TEST_CASE("asciiToLower is safe for bytes above 127", "[parse]") {
    // std::tolower takes an int and is UB for a negative one, and plain char is signed here. A UTF-8
    // continuation byte reaching this must come back unchanged rather than invoking that UB -- mod
    // ids and chat lines carry them.
    const char high = static_cast<char>(0xC3); // UTF-8 lead byte of 'Ã'
    CHECK(fl::asciiToLower(high) == high);
    CHECK(fl::asciiLower("caf\xC3\xA9") == "caf\xC3\xA9");
}

TEST_CASE("iequals compares without case and without surprises", "[parse]") {
    CHECK(fl::iequals("Bearer", "bearer"));
    CHECK(fl::iequals("", ""));
    CHECK_FALSE(fl::iequals("bearer", "bearers")); // length first: a prefix is not a match
    CHECK_FALSE(fl::iequals("bearer", "beare"));
    CHECK_FALSE(fl::iequals("token", "bearer"));
}

TEST_CASE("istartsWith and iendsWith handle the boundary cases", "[parse]") {
    CHECK(fl::istartsWith("Bearer abc123", "bearer"));
    CHECK(fl::istartsWith("BEARER", "bearer"));     // the whole string IS the prefix
    CHECK(fl::istartsWith("anything", ""));         // an empty prefix always matches
    CHECK_FALSE(fl::istartsWith("Bear", "bearer")); // shorter than the prefix

    CHECK(fl::iendsWith("model.GLB", ".glb"));
    CHECK(fl::iendsWith(".glb", ".glb"));
    CHECK(fl::iendsWith("anything", ""));
    CHECK_FALSE(fl::iendsWith("glb", ".glb"));
}
