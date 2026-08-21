// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace fl {

// FNV-1a, in one place (#1247), with the TRUE standard offset basis.
//
// WHY THIS EXISTS. Five sites hand-rolled this hash, and two of them used a basis that was not
// FNV-1a's: `1469598103934665603` is the real `14695981039346656037` with its last digit dropped.
// Nothing recorded that as deliberate — the identifier and every comment said "FNV-1a" — so the
// replay state hash and the campaign/mission seeds were computing something that shares a name with
// FNV-1a and not a value. That is a trap with a date on it: the roadmap plans an offline `fl-review`
// anti-cheat service that will re-implement replay verification from the format docs, and an honest
// FNV-1a there would disagree with every hash the engine produced.
//
// All five sites now use the standard basis. This CHANGES VALUES for the two that were truncated —
// see the commit — which was cheap to do now and would not have been after the protocol freeze.
//
// This is a fingerprint, not a security primitive: it defends against drift, never against a
// deliberately-collided input.
//
// Header-only and stdlib-only, the Json.h pattern: no target, no link edge, no layering change.

inline constexpr uint64_t kFnv1a64Basis = 14695981039346656037ull;
inline constexpr uint64_t kFnv1a64Prime = 1099511628211ull;
inline constexpr uint32_t kFnv1a32Basis = 2166136261u;
inline constexpr uint32_t kFnv1a32Prime = 16777619u;

// ── incremental ──────────────────────────────────────────────────────────────
// For callers that mix in bytes from more than one source and cannot hand over a single buffer.

constexpr void fnv1a64Byte(uint64_t& h, uint8_t b) noexcept {
    h ^= b;
    h *= kFnv1a64Prime;
}

constexpr void fnv1a32Byte(uint32_t& h, uint8_t b) noexcept {
    h ^= b;
    h *= kFnv1a32Prime;
}

// Fold a 64-bit value in little-endian byte order. Spelled out rather than memcpy'd so the byte
// order is the hash's own, identical on every host — a state hash that depended on the machine's
// endianness would compare two honest recordings and call them different.
constexpr void fnv1a64Fold(uint64_t& h, uint64_t v) noexcept {
    for (int i = 0; i < 8; ++i)
        fnv1a64Byte(h, static_cast<uint8_t>((v >> (8 * i)) & 0xFFull));
}

// ── whole-input ──────────────────────────────────────────────────────────────
// `h` is the RUNNING value, defaulting to the basis: pass a previous result to continue one hash
// across several inputs, which is not the same as hashing them separately and combining.

[[nodiscard]] constexpr uint64_t fnv1a64(std::string_view s, uint64_t h = kFnv1a64Basis) noexcept {
    for (char c : s)
        fnv1a64Byte(h, static_cast<uint8_t>(c));
    return h;
}

[[nodiscard]] constexpr uint64_t fnv1a64(const uint8_t* data, std::size_t n, uint64_t h = kFnv1a64Basis) noexcept {
    for (std::size_t i = 0; i < n; ++i)
        fnv1a64Byte(h, data[i]);
    return h;
}

[[nodiscard]] constexpr uint32_t fnv1a32(const uint8_t* data, std::size_t n, uint32_t h = kFnv1a32Basis) noexcept {
    for (std::size_t i = 0; i < n; ++i)
        fnv1a32Byte(h, data[i]);
    return h;
}

} // namespace fl
