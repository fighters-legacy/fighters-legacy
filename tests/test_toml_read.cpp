// SPDX-License-Identifier: GPL-3.0-or-later
//
// fl::req_* / fl::opt_* — the shared TOML field reads (#1245).
//
// Seven def parsers (entity, weapon, sensor, flight model, airport, escalation policy, livery) each
// carried their own copy of these. The copies agreed on behaviour and disagreed only on spelling —
// which is how the message a content author sees came to depend on which parser rejected the file.
// What these tests pin is the part that IS user-facing: the exact message text, including the
// per-parser context prefix that three of the seven parsers prepend.

#include "config/TomlRead.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace {

toml::table parse(std::string_view src) {
    return toml::parse(src);
}

} // namespace

TEST_CASE("req_string returns the value and rejects a missing field", "[toml_read]") {
    auto tbl = parse("name = \"viper\"\n");
    REQUIRE(fl::req_string(tbl["name"], "name") == "viper");

    REQUIRE_THROWS_WITH(fl::req_string(tbl["callsign"], "callsign"), "missing required field: callsign");
}

TEST_CASE("req_double and req_float read the same node, one narrowing", "[toml_read]") {
    auto tbl = parse("lat = 47.5\nspan = 11.4\nwhole = 3\n");
    REQUIRE(fl::req_double(tbl["lat"], "lat") == 47.5);
    REQUIRE(fl::req_float(tbl["span"], "span") == 11.4f);

    // toml++ coerces an integer literal on the double path, so `whole = 3` is a legal float field.
    REQUIRE(fl::req_double(tbl["whole"], "whole") == 3.0);

    REQUIRE_THROWS_WITH(fl::req_double(tbl["lon"], "lon"), "missing required field: lon");
    REQUIRE_THROWS_WITH(fl::req_float(tbl["chord"], "chord"), "missing required field: chord");
}

TEST_CASE("a wrong-typed field reads as missing, not as a silent zero", "[toml_read]") {
    auto tbl = parse("mass = \"heavy\"\nname = 12\n");
    REQUIRE_THROWS_WITH(fl::req_double(tbl["mass"], "mass"), "missing required field: mass");
    REQUIRE_THROWS_WITH(fl::req_string(tbl["name"], "name"), "missing required field: name");
}

TEST_CASE("the context prefix is prepended verbatim", "[toml_read]") {
    auto tbl = parse("x = 1.0\n");

    // The three prefixes in the tree today. These strings are validator output a content author
    // reads, so they are pinned exactly, not as substrings.
    REQUIRE_THROWS_WITH(fl::req_string(tbl["id"], "airport.id", "airport: "),
                        "airport: missing required field: airport.id");
    REQUIRE_THROWS_WITH(fl::req_string(tbl["id"], "policy.id", "zone policy: "),
                        "zone policy: missing required field: policy.id");
    REQUIRE_THROWS_WITH(fl::req_string(tbl["name"], "livery.name", "livery def parse error: "),
                        "livery def parse error: missing required field: livery.name");
}

TEST_CASE("req_float_array reports empty and non-numeric distinctly", "[toml_read]") {
    auto tbl = parse("alpha = [-4.0, 0.0, 12.0]\nempty = []\nmixed = [1.0, \"two\"]\n");

    const std::vector<float> alpha = fl::req_float_array(tbl["alpha"], "alpha");
    REQUIRE(alpha == std::vector<float>{-4.f, 0.f, 12.f});

    // An absent array and an authored-but-empty one are the same authoring mistake, and say so.
    REQUIRE_THROWS_WITH(fl::req_float_array(tbl["mach"], "mach"), "missing or empty required array: mach");
    REQUIRE_THROWS_WITH(fl::req_float_array(tbl["empty"], "empty"), "missing or empty required array: empty");

    REQUIRE_THROWS_WITH(fl::req_float_array(tbl["mixed"], "mixed"), "non-numeric value in array: mixed");
    REQUIRE_THROWS_WITH(fl::req_float_array(tbl["mach"], "mach", "flight model: "),
                        "flight model: missing or empty required array: mach");
    REQUIRE_THROWS_WITH(fl::req_float_array(tbl["mixed"], "mixed", "flight model: "),
                        "flight model: non-numeric value in array: mixed");
}

TEST_CASE("the optional reads fall back instead of throwing", "[toml_read]") {
    auto tbl = parse("drag = 0.021\nradar = true\nrole = \"interceptor\"\nbad = \"x\"\n");

    REQUIRE(fl::opt_float(tbl["drag"], 1.f) == 0.021f);
    REQUIRE(fl::opt_float(tbl["absent"], 1.f) == 1.f);
    REQUIRE(fl::opt_float(tbl["bad"], 1.f) == 1.f); // wrong type takes the fallback too

    REQUIRE(fl::opt_bool(tbl["radar"], false) == true);
    REQUIRE(fl::opt_bool(tbl["absent"], true) == true);

    REQUIRE(fl::opt_string(tbl["role"]) == "interceptor");
    REQUIRE(fl::opt_string(tbl["absent"]).empty());
}
