// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace fl {

// The 8-byte PNG signature.
inline constexpr uint8_t kPngSig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

[[nodiscard]] inline bool hasPngSignature(const uint8_t* d, std::size_t n) noexcept {
    return d != nullptr && n >= sizeof(kPngSig) && std::memcmp(d, kPngSig, sizeof(kPngSig)) == 0;
}

// Reject a PNG whose chunk lengths overrun the buffer before handing it to a decoder (#1237). stb
// allocates a chunk's DECLARED length up-front (its IDAT reader honors any length below its internal
// 1 GB cap), so a tiny file claiming a huge IDAT triggers a huge allocation from an untrusted
// content-pack chunk (a memory-exhaustion DoS). A well-formed PNG never declares a chunk longer
// than the data that follows it. Walk the chunk list: 8-byte signature, then repeated
// [len:u32 BE][type:4][data:len][crc:4].
//
// Lives at the platform root (beside Utf8Decode.h) because its consumers span engine-render,
// engine-campaign AND platform-vulkan — and platform/ must not include engine/.
[[nodiscard]] inline bool pngChunkLengthsSane(const uint8_t* d, std::size_t n) noexcept {
    if (n < 8)
        return false;
    std::size_t off = 8; // skip the PNG signature
    while (off + 8 <= n) {
        const uint32_t len = (static_cast<uint32_t>(d[off]) << 24) | (static_cast<uint32_t>(d[off + 1]) << 16) |
                             (static_cast<uint32_t>(d[off + 2]) << 8) | static_cast<uint32_t>(d[off + 3]);
        // After the 4-byte length + 4-byte type at off, `len` data bytes must fit in what remains.
        if (len > n - off - 8)
            return false;
        off += static_cast<std::size_t>(12) + len; // len(4) + type(4) + data(len) + crc(4); no overflow (len <= n)
    }
    return true;
}

} // namespace fl
