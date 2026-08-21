// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "util/Str.h"

#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace fl {

// Strict number parsing from a string_view, in one place (#1244).
//
// WHY THE COPIES EXISTED. `std::from_chars`' floating-point overloads are missing on Apple Clang
// before macOS 13.3 (Xcode 14.3), so every float parser in the tree fell back to strtod/strtof on a
// NUL-terminated copy — and each one re-derived that fallback, re-wrote the Apple-Clang comment,
// and picked its own answer to the questions the fallback raises: is a trailing character an error,
// is an out-of-range value an error, is this field a float or a double. There were nine such
// parsers and they gave four different answers.
//
// THE CONTRACT HERE. The number must begin at the FIRST character — no leading whitespace, which
// strtod would otherwise skip and from_chars would not, so the float and integer forms answered
// differently for " 12". `parse*` is then strict: nothing may follow it either, and a value that
// overflows the type is a failure rather than an infinity. Failure is `std::nullopt` — never a zero
// the caller cannot distinguish from an authored one, which is what `strtod(s, nullptr)` hands back
// and how `timer(soon)` used to fire the instant a mission started.
//
// `parseLeading*` is the documented tolerant form, for the two callers that genuinely want it: a
// CSV importer reading fields with unit suffixes, and campaign state fields that fall back to a
// default. It ignores what FOLLOWS the number, nothing more. A caller that wants surrounding
// whitespace tolerated says so by trimming — see `util/Str.h` — rather than inheriting it by
// accident from whichever library function happened to be underneath.
//
// Header-only and stdlib-only, the Json.h pattern: no target, no link edge, no layering change.

namespace detail {

// strtod/strtof need a NUL-terminated string. Short inputs — every real number literal — use a
// stack buffer; anything longer falls back to an allocation rather than being rejected for length.
template <typename T, typename Conv>
[[nodiscard]] inline std::optional<T> strToFloat(std::string_view s, Conv conv, bool strict) {
    if (s.empty() || isWs(s.front()))
        return std::nullopt; // strtod would skip the space; from_chars would not. Neither, here.

    char stack[64];
    std::string heap;
    const char* c = nullptr;
    if (s.size() < sizeof(stack)) {
        std::memcpy(stack, s.data(), s.size());
        stack[s.size()] = '\0';
        c = stack;
    } else {
        heap.assign(s);
        c = heap.c_str();
    }

    char* end = nullptr;
    errno = 0;
    const T v = conv(c, &end);
    if (end == c)
        return std::nullopt; // no number at all
    if (errno == ERANGE)
        return std::nullopt; // 1e999 is not a coordinate
    if (strict && static_cast<std::size_t>(end - c) != s.size())
        return std::nullopt; // trailing junk
    return v;
}

template <typename T> [[nodiscard]] inline std::optional<T> strToInt(std::string_view s, bool strict) {
    T v{};
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{})
        return std::nullopt;
    if (strict && ptr != s.data() + s.size())
        return std::nullopt;
    return v;
}

} // namespace detail

// ── strict: the whole input must be the number ───────────────────────────────

[[nodiscard]] inline std::optional<double> parseDouble(std::string_view s) {
    return detail::strToFloat<double>(s, std::strtod, /*strict=*/true);
}

[[nodiscard]] inline std::optional<float> parseFloat(std::string_view s) {
    return detail::strToFloat<float>(s, std::strtof, /*strict=*/true);
}

[[nodiscard]] inline std::optional<uint32_t> parseU32(std::string_view s) {
    return detail::strToInt<uint32_t>(s, /*strict=*/true);
}

[[nodiscard]] inline std::optional<int32_t> parseI32(std::string_view s) {
    return detail::strToInt<int32_t>(s, /*strict=*/true);
}

[[nodiscard]] inline std::optional<uint64_t> parseU64(std::string_view s) {
    return detail::strToInt<uint64_t>(s, /*strict=*/true);
}

// ── tolerant: read a leading number and ignore whatever follows ──────────────

[[nodiscard]] inline std::optional<double> parseLeadingDouble(std::string_view s) {
    return detail::strToFloat<double>(s, std::strtod, /*strict=*/false);
}

template <typename T> [[nodiscard]] inline std::optional<T> parseLeadingInt(std::string_view s) {
    return detail::strToInt<T>(s, /*strict=*/false);
}

// ── adapter for the older call shape ────────────────────────────────────────

// Assigns on success and reports whether it did, for the `bool f(sv, out)` shape the argument
// checks in the console and AI-factory command tables are written in. New code should take the
// optional directly; this exists so adopting the shared rule did not require rewriting sixty-odd
// call sites by hand, each one a chance to get an argument index wrong.
template <typename T> [[nodiscard]] inline bool readInto(std::optional<T> parsed, T& out) noexcept {
    if (!parsed)
        return false;
    out = *parsed;
    return true;
}

// ── digit classification ─────────────────────────────────────────────────────

// True when `s` is non-empty and every byte is an ASCII digit. Used to tell "this argument is a
// peer id" from "this argument is a name", so it must not accept a sign, a space or a decimal point.
[[nodiscard]] inline constexpr bool isAllDigits(std::string_view s) noexcept {
    if (s.empty())
        return false;
    for (char c : s)
        if (c < '0' || c > '9')
            return false;
    return true;
}

} // namespace fl
