// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for engine/util/Json.h (#1080) — the engine's one JSON escaper, writer set and reader.
//
// Two things are worth testing here beyond the obvious. First, escaping: MatchEvent::text is
// attacker-controlled and reaches /events, the MCP audit mirror and the .flrep recorder, so the escaper
// gets a property test over every byte rather than a handful of examples. Second, the structural
// reader's advantage over the find-based readers it replaced — those are the cases that motivated
// promoting this scanner rather than the simplest one.
#include "util/Json.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

using namespace fl;

// ---------------------------------------------------------------------------
// Escaping — the hostile-input surface
// ---------------------------------------------------------------------------

TEST_CASE("json::escape emits the mandatory escapes and the short forms", "[json]") {
    CHECK(json::escape("plain") == "plain");
    CHECK(json::escape("say \"hi\"") == "say \\\"hi\\\"");
    CHECK(json::escape("C:\\path") == "C:\\\\path");
    CHECK(json::escape("a\nb") == "a\\nb");
    CHECK(json::escape("a\rb") == "a\\rb");
    CHECK(json::escape("a\tb") == "a\\tb");
    CHECK(json::escape("a\bb") == "a\\bb");
    CHECK(json::escape("a\fb") == "a\\fb");
    CHECK(json::escape(std::string("ctl\x01", 4)) == "ctl\\u0001");
    // Valid UTF-8 passes through: the chat path sanitizes to BMP UTF-8 already, and mangling it here
    // would corrupt a callsign rather than protect anything.
    CHECK(json::escape("caf\xc3\xa9") == "caf\xc3\xa9");
}

// The property that matters, over every byte a string can hold: whatever goes in, what comes out
// contains no raw quote, no raw backslash-that-is-not-an-escape, and no control character. Anything
// that survives one of those is a JSON-injection bug, not a cosmetic one.
TEST_CASE("json::escape: no byte can escape the string literal it is written into", "[json]") {
    for (int b = 0; b < 256; ++b) {
        const std::string in(1, static_cast<char>(b));
        const std::string out = json::escape(in);

        // Round-trip through the reader: the escaped form, quoted, must decode back to the input.
        // That is the real property -- it proves the escaping is both sufficient and lossless.
        const auto back = json::stringValue("\"" + out + "\"");
        REQUIRE(back.has_value());
        CHECK(*back == in);

        // No raw control byte survives into the document.
        for (const char oc : out)
            CHECK(static_cast<unsigned char>(oc) >= 0x20);

        // And the escaped form, quoted, is ONE complete string literal: scanning it consumes the whole
        // thing. This is the injection property stated exactly -- an embedded quote that terminated the
        // literal early would leave bytes over, and those bytes would be document structure.
        const std::string quoted = "\"" + out + "\"";
        CHECK(json::skipString(quoted, 0) == quoted.size());
    }
}

TEST_CASE("json::escape: a chat line built to break out of the document does not", "[json]") {
    // The shape an attacker sends: close the string, close the object, open a new key.
    const std::string hostile = R"(hi", "admin": true, "x": ")";
    const std::string doc = "{\"text\": \"" + json::escape(hostile) + "\"}";

    CHECK(json::stringField(doc, "text") == hostile); // read back verbatim
    CHECK(json::member(doc, "admin").empty());        // and injected nothing
}

// ---------------------------------------------------------------------------
// Writers
// ---------------------------------------------------------------------------

TEST_CASE("json::str and json::num write the deterministic report forms", "[json]") {
    CHECK(json::str("a\"b") == "\"a\\\"b\"");
    CHECK(json::num(1.5) == "1.5");
    CHECK(json::num(0.0) == "0");
    // %.6g, the form every report in the tree uses, so a golden test can compare bytes.
    CHECK(json::num(1.0 / 3.0) == "0.333333");
}

// ---------------------------------------------------------------------------
// The structural reader, and why it is the one that was promoted
// ---------------------------------------------------------------------------

TEST_CASE("json::member reads keys at one object level only", "[json]") {
    const std::string_view obj = R"({"a": 1, "b": "two", "c": {"d": 3}, "e": [1, 2]})";
    CHECK(json::intValue(json::member(obj, "a")) == 1);
    CHECK(json::stringValue(json::member(obj, "b")) == "two");
    CHECK(json::member(obj, "c") == R"({"d": 3})");
    CHECK(json::member(obj, "e") == "[1, 2]");
    CHECK(json::member(obj, "missing").empty());

    // THE POINT: `d` is a member of `c`, not of `obj`. A find("\"d\"") lookup answered 3 here, which
    // is how a nested field could satisfy a top-level check.
    CHECK(json::member(obj, "d").empty());
    CHECK(json::numberField(obj, "d") == std::nullopt);
}

TEST_CASE("json::member does not match a key that only appears inside a string value", "[json]") {
    // The find-based readers this replaced answered `true` for `admin` on exactly this document.
    const std::string_view obj = R"({"note": "\"admin\": true"})";
    CHECK(json::member(obj, "admin").empty());
    CHECK(json::boolField(obj, "admin") == std::nullopt);
}

TEST_CASE("json::member compares raw key bytes, so an escaped spelling is not a match", "[json]") {
    // Every key any of these readers looks for is plain ASCII, so honouring `\u0061dmin` as `admin`
    // would only ever help someone trying to smuggle one past a check.
    CHECK(json::member(R"({"\u0061dmin": true})", "admin").empty());
}

TEST_CASE("json::member fails closed on malformed input", "[json]") {
    CHECK(json::member(R"({"a": )", "a").empty());     // truncated value
    CHECK(json::member(R"({"a" 1})", "a").empty());    // missing colon
    CHECK(json::member(R"({a: 1})", "a").empty());     // unquoted key
    CHECK(json::member(R"([{"a": 1}])", "a").empty()); // an array, not an object
    CHECK(json::member(R"({"a": "unterminated)", "a").empty());
    CHECK(json::member("", "a").empty());
    CHECK(json::member("null", "a").empty());
}

TEST_CASE("json value decoders reject what they are not", "[json]") {
    CHECK(json::stringValue("42") == std::nullopt);
    CHECK(json::intValue(R"("42")") == std::nullopt); // a quoted number is a string, not an int
    CHECK(json::boolValue("1") == std::nullopt);      // JSON booleans are true/false, not 0/1
    CHECK(json::boolValue("true") == true);
    CHECK(json::boolValue("false") == false);
    CHECK(json::numberValue("-1.5e2") == -150.0);
    CHECK(json::numberValue("nonsense") == std::nullopt);
}

TEST_CASE("json::stringValue bounds the decoded length and decodes \\u", "[json]") {
    CHECK(json::stringValue(R"("abcdef")", 3) == std::nullopt); // over the bound: refused, not truncated
    CHECK(json::stringValue(R"("abc")", 3) == "abc");           // exactly at it
    CHECK(json::stringValue(R"("a\u0062c")") == "abc");
    CHECK(json::stringValue(R"("\u00e9")") == "\xc3\xa9");     // BMP -> UTF-8
    CHECK(json::stringValue(R"("\ud800")") == "\xef\xbf\xbd"); // a lone surrogate -> replacement char
    CHECK(json::stringValue(R"("\uZZZZ")") == std::nullopt);
    // An escape JSON does not define fails closed rather than passing the character through.
    CHECK(json::stringValue(R"("bad \q escape")") == std::nullopt);
    CHECK(json::stringValue(R"("unterminated)") == std::nullopt);
}

TEST_CASE("json::arrayElements yields element spans and honours its cap", "[json]") {
    const std::string_view arr = R"([{"a": 1}, "two", 3, [4], null])";
    const auto e = json::arrayElements(arr, 16);
    REQUIRE(e.size() == 5);
    CHECK(e[0] == R"({"a": 1})");
    CHECK(e[1] == R"("two")");
    CHECK(e[2] == "3");
    CHECK(e[3] == "[4]");
    CHECK(e[4] == "null");

    CHECK(json::arrayElements(arr, 2).size() == 2);        // the cap is what stops an untrusted list
    CHECK(json::arrayElements(R"({"a": 1})", 16).empty()); // an object is not an array
    CHECK(json::arrayElements("[", 16).empty());
    // A malformed tail yields what was readable rather than nothing: these documents are additive, and
    // the caller asked for the rows it can use.
    CHECK(json::arrayElements(R"([1, 2, {"unterminated)", 16).size() == 2);
}

TEST_CASE("json field helpers are member() plus a decode", "[json]") {
    const std::string_view obj = R"({"s": "x", "n": 2.5, "i": -7, "b": true})";
    CHECK(json::stringField(obj, "s") == "x");
    CHECK(json::numberField(obj, "n") == 2.5);
    CHECK(json::intField(obj, "i") == -7);
    CHECK(json::boolField(obj, "b") == true);
    CHECK(json::stringField(obj, "nope") == std::nullopt);
}
