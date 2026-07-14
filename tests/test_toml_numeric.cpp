// SPDX-License-Identifier: GPL-3.0-or-later
//
// fl::tomlInt / fl::tomlIntNarrow (#824).
//
// These sit between every TOML parser in the tree and a genuine undefined-behaviour bug in toml++:
// its float→int conversion performs static_cast<int64_t>(double) inside the very guard meant to
// validate it, so an out-of-range float literal in an integer field is UB. The scheduled deep fuzz
// run found it with `port = 10888888888888888888888888888888.0`.
//
// Five parsers now depend on this being right — server_config, ModLoader, UserConfig,
// FlightModelParser and EntityDefParser — and content-pack TOML is untrusted input.

#include "config/TomlNumeric.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <string_view>

using namespace fl;

namespace {

// Parses `v = <literal>` and reads it back through the hardened accessor.
std::optional<int64_t> readInt(std::string_view literal) {
    const std::string src = std::string("v = ") + std::string(literal) + "\n";
    const toml::table tbl = toml::parse(src);
    return tomlInt(tbl["v"]);
}

} // namespace

TEST_CASE("tomlInt: plain integers round-trip", "[toml_numeric]") {
    CHECK(readInt("0") == 0);
    CHECK(readInt("4778") == 4778);
    CHECK(readInt("-1") == -1);
    CHECK(readInt("9223372036854775807") == std::numeric_limits<int64_t>::max());
    CHECK(readInt("-9223372036854775808") == std::numeric_limits<int64_t>::min());
}

TEST_CASE("tomlInt: a whole-number float is accepted", "[toml_numeric]") {
    // toml++ means to allow this spelling, and so do we. The hardening is about representability,
    // not about punishing anyone who typed a decimal point.
    CHECK(readInt("4778.0") == 4778);
    CHECK(readInt("-3.0") == -3);
}

TEST_CASE("tomlInt: a fractional float is REFUSED, not truncated", "[toml_numeric]") {
    // Silently turning 4778.7 into 4778 would be a lie about what the author asked for. An
    // unusable value is no value: the caller sees an absent field and applies its default.
    CHECK(readInt("4778.7") == std::nullopt);
    CHECK(readInt("0.5") == std::nullopt);
}

TEST_CASE("tomlInt: an out-of-range float is refused rather than cast", "[toml_numeric]") {
    // THE BUG. Every one of these would have been static_cast<int64_t>(double) with the result
    // outside int64's range -- undefined behaviour, inside toml++, on a value an author controls.
    CHECK(readInt("10888888888888888888888888888888.0") == std::nullopt); // the fuzz reproducer
    CHECK(readInt("1e308") == std::nullopt);
    CHECK(readInt("-1e308") == std::nullopt);

    // 2^63 exactly: representable as a double, one past INT64_MAX, and the classic off-by-one that
    // an inclusive bound would wave straight through into the UB.
    CHECK(readInt("9223372036854775808.0") == std::nullopt);
}

TEST_CASE("tomlInt: non-finite floats are refused", "[toml_numeric]") {
    CHECK(readInt("nan") == std::nullopt);
    CHECK(readInt("inf") == std::nullopt);
    CHECK(readInt("-inf") == std::nullopt);
}

TEST_CASE("tomlInt: non-numeric and absent nodes are refused", "[toml_numeric]") {
    const toml::table tbl = toml::parse("s = \"text\"\nb = true\n");
    CHECK(tomlInt(tbl["s"]) == std::nullopt);
    CHECK(tomlInt(tbl["b"]) == std::nullopt);
    CHECK(tomlInt(tbl["missing"]) == std::nullopt); // an empty node_view must not dereference
}

TEST_CASE("tomlIntNarrow: values outside int's range are refused", "[toml_numeric]") {
    const toml::table tbl = toml::parse("small = 42\nbig = 5000000000\nhuge = 1e30\n");
    CHECK(tomlIntNarrow(tbl["small"]) == 42);
    CHECK(tomlIntNarrow(tbl["big"]) == std::nullopt); // > INT32_MAX
    CHECK(tomlIntNarrow(tbl["huge"]) == std::nullopt);
}
