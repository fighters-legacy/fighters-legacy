// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

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

} // namespace fl
