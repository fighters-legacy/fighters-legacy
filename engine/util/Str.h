// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace fl {

// Whitespace trimming, in one place (#1244). Three byte-identical copies existed — engine/util/Json.h,
// engine/ai/ChatIntentBridge.cpp and the lambda inside RequiredPackPolicy.h — and `fl::json::trim`
// stays as an alias of this one so JSON callers keep the name that reads correctly there.
//
// Header-only and stdlib-only, the Json.h pattern: no target, no link edge, no layering change.

// ASCII whitespace only, and deliberately not `std::isspace`: that one is locale-sensitive and takes
// an int, so passing it a plain `char` is UB for bytes above 127 — which arrive in mod ids and chat.
[[nodiscard]] inline constexpr bool isWs(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// The view with leading and trailing whitespace removed. An all-whitespace input trims to empty.
[[nodiscard]] inline constexpr std::string_view trim(std::string_view s) noexcept {
    std::size_t b = 0, e = s.size();
    while (b < e && isWs(s[b]))
        ++b;
    while (e > b && isWs(s[e - 1]))
        --e;
    return s.substr(b, e - b);
}

// ASCII case folding, in one place (#1265). Seven sites hand-rolled it in four shapes: a
// std::transform, a per-char helper, an inline loop inside a key builder, and a character-by-
// character case-insensitive compare.
//
// They agreed only because nothing in this program calls setlocale — which is a property of the
// WHOLE PROGRAM, not of any of those functions, and therefore not something any of them could rely
// on locally. std::tolower is kept rather than rewritten to arithmetic folding so adopting these is
// provably behaviour-preserving for bytes 0x80-0xFF as well.
//
// ⚠ std::tolower takes an int and is UB for a negative one, so the cast through unsigned char is
// load-bearing, not decoration — plain `char` is signed on x86 and mod ids and chat carry bytes
// above 127.
[[nodiscard]] inline char asciiToLower(char c) noexcept {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

[[nodiscard]] inline std::string asciiLower(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        c = asciiToLower(c);
    return out;
}

// Case-insensitive equality and prefix test. `istartsWith` exists because the sites that needed it
// were comparing against a lowercase literal one character at a time, which reads as a loop rather
// than as the question it is asking.
[[nodiscard]] inline bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (asciiToLower(a[i]) != asciiToLower(b[i]))
            return false;
    return true;
}

[[nodiscard]] inline bool istartsWith(std::string_view s, std::string_view prefix) noexcept {
    return s.size() >= prefix.size() && iequals(s.substr(0, prefix.size()), prefix);
}

[[nodiscard]] inline bool iendsWith(std::string_view s, std::string_view suffix) noexcept {
    return s.size() >= suffix.size() && iequals(s.substr(s.size() - suffix.size()), suffix);
}

} // namespace fl
