// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace fl {

// Decodes the next UTF-8 codepoint from [p, end), advances p past the consumed
// bytes.  Returns U+FFFD for any invalid or incomplete sequence.
// BMP-only: 4-byte sequences (U+10000+) return U+FFFD.
inline uint32_t nextUtf8Codepoint(const char*& p, const char* end) noexcept {
    const auto b0 = static_cast<unsigned char>(*p++);
    if (b0 < 0x80)
        return b0;

    if ((b0 & 0xE0u) == 0xC0u && p < end && (static_cast<unsigned char>(*p) & 0xC0u) == 0x80u) {
        return ((b0 & 0x1Fu) << 6) | (static_cast<unsigned char>(*p++) & 0x3Fu);
    }

    if ((b0 & 0xF0u) == 0xE0u && p + 1 < end && (static_cast<unsigned char>(p[0]) & 0xC0u) == 0x80u &&
        (static_cast<unsigned char>(p[1]) & 0xC0u) == 0x80u) {
        const uint32_t cp = ((b0 & 0x0Fu) << 12) | ((static_cast<unsigned char>(p[0]) & 0x3Fu) << 6) |
                            (static_cast<unsigned char>(p[1]) & 0x3Fu);
        p += 2;
        return cp;
    }

    // Skip any trailing continuation bytes of an invalid or 4-byte sequence.
    while (p < end && (static_cast<unsigned char>(*p) & 0xC0u) == 0x80u)
        ++p;
    return 0xFFFDu;
}

} // namespace fl
