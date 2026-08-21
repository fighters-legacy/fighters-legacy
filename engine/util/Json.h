// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Json — the engine's one JSON escaper, one set of writer helpers, and one reader (#1080).
//
// Hand-rolled JSON is the right call here: the schemas are small, deterministic output matters, and a
// JSON library in `engine-*` would be a dependency bought for very little — `engine-protocol` links
// nothing but the stdlib and this header must not change that. What was NOT right is how many copies
// there were. Before this file: two escapers, six independent `toJson` bodies, and FIVE independent
// readers (`JsonScan`, `ServerTickReport::fromJson`, `HttpAdminAuth`'s field pair, `McpProtocol`'s
// member scanner, `LobbyListClient::parseJsonString`).
//
// Escaping is the part that matters. `MatchEvent::text` is attacker-controlled and reaches `/events`,
// the MCP audit mirror and the `.flrep` recorder; two escapers meant two chances to get it wrong and
// one place to fix it.
//
// ⚑ The READER promoted here is the STRUCTURAL one, from McpProtocol — not the simplest one. The four
// others located a key with `find("\"key\"")`, which matches a key nested inside a sub-object, or one
// that merely appears inside a string VALUE. `{"note": "\"admin\": true"}` satisfied a find-based
// lookup for `admin`. The scanner below walks the object: it reads keys only at the level asked for,
// compares raw key bytes (so an escaped spelling of a key is not a match), and fails closed on
// malformed input rather than guessing. Promoting the weakest reader would have spread that hole to
// the untrusted REST bodies instead of closing it.
//
// Header-only and stdlib-only, so a consumer gains no link edge across a layer boundary — the
// `Capability.h` pattern.
//
// Numbers go through `strtod`, NOT `std::from_chars`: Apple Clang has no floating-point `from_chars`.

#include "util/Str.h"
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fl::json {

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

// Escape for a JSON string literal: the two mandatory escapes plus the short forms, and \uXXXX for
// anything else below 0x20. Bytes >= 0x20 pass through, so valid UTF-8 stays intact (the chat path
// already sanitizes to BMP UTF-8 with control characters stripped; this is the second line).
[[nodiscard]] inline std::string escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (const char c : s) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                out += buf;
            } else {
                out += c;
            }
        }
    }
    return out;
}

// A quoted, escaped JSON string literal.
[[nodiscard]] inline std::string str(std::string_view s) {
    return "\"" + escape(s) + "\"";
}

// A number in the engine's deterministic report format. %.6g for the same reason every report uses
// it: identical input must produce identical bytes, so a golden test can compare them.
[[nodiscard]] inline std::string num(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.6g", v);
    return buf;
}

// ---------------------------------------------------------------------------
// Reading — a structural scanner, not a parser
// ---------------------------------------------------------------------------
//
// Enough to read a value back by name from a deterministic document, tolerant of missing, extra and
// reordered fields, and nothing more. That tolerance is deliberate: these formats are additive and
// name-keyed, so a reader must ignore what it does not recognise rather than fail on it. What it is
// NOT tolerant of is structure: a malformed object yields nothing rather than a guess.

// The one implementation lives in util/Str.h (#1244); these keep the `json::` spelling that reads
// correctly at the call sites below.
using fl::isWs;
using fl::trim;

// Index just past the string literal starting at `i` (which must be its opening quote), or npos.
[[nodiscard]] inline std::size_t skipString(std::string_view s, std::size_t i) noexcept {
    ++i; // opening quote
    while (i < s.size()) {
        if (s[i] == '\\') {
            i += 2; // the escape and whatever it escapes
            continue;
        }
        if (s[i] == '"')
            return i + 1;
        ++i;
    }
    return std::string_view::npos;
}

// Index just past the JSON value starting at `i`, or npos. Handles the four shapes that can appear as
// a member value; a scalar simply runs to the next delimiter at depth 0.
[[nodiscard]] inline std::size_t skipValue(std::string_view s, std::size_t i) noexcept {
    if (i >= s.size())
        return std::string_view::npos;
    if (s[i] == '"')
        return skipString(s, i);
    if (s[i] == '{' || s[i] == '[') {
        int depth = 0;
        while (i < s.size()) {
            const char c = s[i];
            if (c == '"') {
                i = skipString(s, i);
                if (i == std::string_view::npos)
                    return std::string_view::npos;
                continue;
            }
            if (c == '{' || c == '[')
                ++depth;
            else if (c == '}' || c == ']') {
                --depth;
                if (depth == 0)
                    return i + 1;
                if (depth < 0)
                    return std::string_view::npos;
            }
            ++i;
        }
        return std::string_view::npos;
    }
    // Scalar: number, true, false, null.
    const std::size_t start = i;
    while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' && !isWs(s[i]))
        ++i;
    return i > start ? i : std::string_view::npos;
}

[[nodiscard]] inline bool isObject(std::string_view span) noexcept {
    const std::string_view t = trim(span);
    return t.size() >= 2 && t.front() == '{' && t.back() == '}';
}

// The raw span of `key`'s value in `obj`, or empty. Reads keys at THIS object's level only — a member
// of a nested object is not a member of this one, which is the difference between this and a
// `find("\"key\"")` lookup.
[[nodiscard]] inline std::string_view member(std::string_view obj, std::string_view key) noexcept {
    const std::string_view s = trim(obj);
    if (s.empty() || s.front() != '{')
        return {};

    std::size_t i = 1; // past '{'
    while (i < s.size()) {
        while (i < s.size() && (isWs(s[i]) || s[i] == ','))
            ++i;
        if (i >= s.size() || s[i] == '}')
            return {};
        if (s[i] != '"')
            return {}; // a key must be a string; anything else means malformed, so fail closed

        const std::size_t keyStart = i + 1;
        const std::size_t afterKey = skipString(s, i);
        if (afterKey == std::string_view::npos)
            return {};
        // Compare the RAW key bytes. Every key any of these readers looks for is plain ASCII with no
        // escapes, so an escaped spelling of one is not a match worth honouring.
        const std::string_view thisKey = s.substr(keyStart, afterKey - keyStart - 1);

        i = afterKey;
        while (i < s.size() && isWs(s[i]))
            ++i;
        if (i >= s.size() || s[i] != ':')
            return {};
        ++i;
        while (i < s.size() && isWs(s[i]))
            ++i;

        const std::size_t valStart = i;
        const std::size_t valEnd = skipValue(s, i);
        if (valEnd == std::string_view::npos)
            return {};
        if (thisKey == key)
            return trim(s.substr(valStart, valEnd - valStart));
        i = valEnd;
    }
    return {};
}

// Default string bound. Generous, because an MCP mission submission is a legitimate multi-kilobyte
// string; a caller reading an untrusted REST field passes something far smaller.
inline constexpr std::size_t kMaxStringValue = 64 * 1024;

// Decode the string literal in `span`, or nullopt when it is not one, is unterminated, or exceeds
// `maxLen`. The bound matters: a body is untrusted, and an unterminated quote must not make a reader
// scan to the end of a megabyte and return it.
[[nodiscard]] inline std::optional<std::string> stringValue(std::string_view span,
                                                            std::size_t maxLen = kMaxStringValue) {
    const std::string_view s = trim(span);
    if (s.size() < 2 || s.front() != '"')
        return std::nullopt;
    std::string out;
    for (std::size_t i = 1; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '"')
            return out;
        if (out.size() >= maxLen)
            return std::nullopt;
        if (c == '\\') {
            if (++i >= s.size())
                return std::nullopt;
            switch (s[i]) {
            case 'n':
                out += '\n';
                break;
            case 'r':
                out += '\r';
                break;
            case 't':
                out += '\t';
                break;
            case 'b':
                out += '\b';
                break;
            case 'f':
                out += '\f';
                break;
            case 'u': {
                // \uXXXX: decoded to UTF-8 for the BMP. A surrogate half is passed through as the
                // replacement character rather than rejected -- these documents are name-keyed and a
                // reader that fails the whole request over one bad code unit is worse than one that
                // reads the field it was asked for.
                if (i + 4 >= s.size())
                    return std::nullopt;
                unsigned cp = 0;
                for (int k = 1; k <= 4; ++k) {
                    const char h = s[i + static_cast<std::size_t>(k)];
                    const unsigned d = (h >= '0' && h <= '9')   ? static_cast<unsigned>(h - '0')
                                       : (h >= 'a' && h <= 'f') ? static_cast<unsigned>(h - 'a' + 10)
                                       : (h >= 'A' && h <= 'F') ? static_cast<unsigned>(h - 'A' + 10)
                                                                : 16u;
                    if (d == 16u)
                        return std::nullopt;
                    cp = (cp << 4) | d;
                }
                i += 4;
                if (cp >= 0xD800 && cp <= 0xDFFF)
                    cp = 0xFFFD;
                if (cp < 0x80) {
                    out += static_cast<char>(cp);
                } else if (cp < 0x800) {
                    out += static_cast<char>(0xC0 | (cp >> 6));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                } else {
                    out += static_cast<char>(0xE0 | (cp >> 12));
                    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                }
                break;
            }
            case '"':
            case '\\':
            case '/':
                out += s[i];
                break;
            default:
                // An escape JSON does not define fails CLOSED rather than passing the character
                // through. This was the promoted scanner's behaviour and it has a test: a reader that
                // silently accepts `\q` accepts a document no writer produced, which on an untrusted
                // path is a reader guessing at intent.
                return std::nullopt;
            }
            continue;
        }
        out += c;
    }
    return std::nullopt; // unterminated
}

[[nodiscard]] inline std::optional<double> numberValue(std::string_view span) {
    const std::string_view s = trim(span);
    if (s.empty())
        return std::nullopt;
    // strtod needs NUL termination, and a string_view carries no guarantee of it. Copying a bounded
    // prefix is enough: no legitimate number in these documents is longer.
    const std::string buf(s.substr(0, 64));
    char* end = nullptr;
    const double v = std::strtod(buf.c_str(), &end);
    if (end == buf.c_str())
        return std::nullopt;
    return v;
}

[[nodiscard]] inline std::optional<long long> intValue(std::string_view span) noexcept {
    const std::string_view s = trim(span);
    if (s.empty())
        return std::nullopt;
    const std::string buf(s.substr(0, 32));
    char* end = nullptr;
    const long long v = std::strtoll(buf.c_str(), &end, 10);
    if (end == buf.c_str())
        return std::nullopt;
    return v;
}

[[nodiscard]] inline std::optional<bool> boolValue(std::string_view span) noexcept {
    const std::string_view s = trim(span);
    if (s == "true")
        return true;
    if (s == "false")
        return false;
    return std::nullopt;
}

// Raw spans of an array's elements, in order, at most `maxElements` of them. Empty when `span` is not
// an array or is malformed. The cap is a parameter rather than a policy: a lobby list from a stranger
// wants a small one, and an internal report can afford a large one.
[[nodiscard]] inline std::vector<std::string_view> arrayElements(std::string_view span, std::size_t maxElements) {
    std::vector<std::string_view> out;
    const std::string_view s = trim(span);
    if (s.size() < 2 || s.front() != '[')
        return out;

    std::size_t i = 1; // past '['
    while (i < s.size() && out.size() < maxElements) {
        while (i < s.size() && (isWs(s[i]) || s[i] == ','))
            ++i;
        if (i >= s.size() || s[i] == ']')
            break;
        const std::size_t end = skipValue(s, i);
        if (end == std::string_view::npos)
            break; // malformed: return what was read rather than guessing at the rest
        out.push_back(trim(s.substr(i, end - i)));
        i = end;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Field convenience — member() plus a value decode, which is what every caller wanted
// ---------------------------------------------------------------------------

[[nodiscard]] inline std::optional<std::string> stringField(std::string_view obj, std::string_view key,
                                                            std::size_t maxLen = kMaxStringValue) {
    return stringValue(member(obj, key), maxLen);
}

[[nodiscard]] inline std::optional<double> numberField(std::string_view obj, std::string_view key) {
    return numberValue(member(obj, key));
}

[[nodiscard]] inline std::optional<long long> intField(std::string_view obj, std::string_view key) noexcept {
    return intValue(member(obj, key));
}

[[nodiscard]] inline std::optional<bool> boolField(std::string_view obj, std::string_view key) noexcept {
    return boolValue(member(obj, key));
}

} // namespace fl::json
