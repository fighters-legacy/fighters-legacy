// SPDX-License-Identifier: GPL-3.0-or-later

// Terrain chunk PNG decode and binary cache I/O.
// STB_IMAGE_STATIC makes all stbi symbols TU-local, preventing ODR conflicts
// with VkResources.cpp which also defines STB_IMAGE_IMPLEMENTATION.
// clang-format off
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
// clang-format on

#include "render/TerrainChunkIO.h"

#include <cstring>
#include <filesystem>
#include <fstream>

namespace fl {

// Reject a PNG whose chunk lengths overrun the buffer before handing it to stb_image. stb allocates
// a chunk's DECLARED length up-front (its IDAT reader honors any length below its internal 1 GB cap),
// so a tiny file claiming a huge IDAT triggers a huge allocation from an untrusted content-pack chunk
// (a memory-exhaustion DoS). A well-formed PNG never declares a chunk longer than the data that
// follows it. Walk the chunk list: 8-byte signature, then repeated [len:u32 BE][type:4][data:len][crc:4].
static bool pngChunkLengthsSane(const uint8_t* d, size_t n) noexcept {
    if (n < 8)
        return false;
    size_t off = 8; // skip the PNG signature
    while (off + 8 <= n) {
        const uint32_t len = (static_cast<uint32_t>(d[off]) << 24) | (static_cast<uint32_t>(d[off + 1]) << 16) |
                             (static_cast<uint32_t>(d[off + 2]) << 8) | static_cast<uint32_t>(d[off + 3]);
        // After the 4-byte length + 4-byte type at off, `len` data bytes must fit in what remains.
        if (len > n - off - 8)
            return false;
        off += static_cast<size_t>(12) + len; // len(4) + type(4) + data(len) + crc(4); no overflow (len ≤ n)
    }
    return true;
}

std::vector<uint16_t> decodeTerrainChunkPng(const uint8_t* data, size_t size, int* outWidth, int* outHeight) noexcept {
    if (!data || size == 0)
        return {};

    // Terrain chunks are always PNG. Require the 8-byte PNG signature up front so stb_image never
    // dispatches to its other, more fragile format decoders (PSD/PNM/HDR/...) on a mis-typed or
    // malicious chunk — that keeps the untrusted-decode surface to stb's PNG path alone.
    static constexpr uint8_t kPngSig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    if (size < sizeof(kPngSig) || std::memcmp(data, kPngSig, sizeof(kPngSig)) != 0)
        return {};

    if (!pngChunkLengthsSane(data, size))
        return {};

    // Reject non-16-bit sources — 8-bit PNG silently scaled to 16 would corrupt
    // the height encoding (offset=32768, scale=1 convention).
    if (!stbi_is_16_bit_from_memory(data, static_cast<int>(size)))
        return {};

    // Reject absurd declared dimensions BEFORE decoding. stbi_info reads only the header, so a
    // malicious chunk claiming huge dimensions is rejected without ever attempting the multi-hundred-MB
    // decode allocation (a memory-exhaustion DoS on any host loading an untrusted content pack). Real
    // chunks are 513x513; 4096 is generous headroom while capping the worst-case decode buffer
    // (w*h*channels*2 + intermediates) well under a sane budget. Also keeps w*h below int-overflow range.
    int w = 0, h = 0, ch = 0;
    constexpr int kMaxChunkDim = 4096;
    if (stbi_info_from_memory(data, static_cast<int>(size), &w, &h, &ch) && (w > kMaxChunkDim || h > kMaxChunkDim))
        return {};

    w = h = ch = 0;
    stbi_us* pixels = stbi_load_16_from_memory(data, static_cast<int>(size), &w, &h, &ch, 1);
    if (!pixels)
        return {};

    std::vector<uint16_t> result(pixels, pixels + static_cast<ptrdiff_t>(w * h));
    stbi_image_free(pixels);

    if (outWidth)
        *outWidth = w;
    if (outHeight)
        *outHeight = h;
    return result;
}

// ---------------------------------------------------------------------------
// Binary cache helpers
// ---------------------------------------------------------------------------

bool writeTerrainChunkCache(const std::string& path, const uint16_t* data, int width, int height) noexcept {
    if (!data || width <= 0 || height <= 0 || path.empty())
        return false;

    try {
        std::filesystem::create_directories(std::filesystem::path(path).parent_path());

        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f)
            return false;

        const uint32_t magic = kTerrainCacheMagic;
        const auto w = static_cast<uint16_t>(width);
        const auto h = static_cast<uint16_t>(height);

        f.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
        f.write(reinterpret_cast<const char*>(&w), sizeof(w));
        f.write(reinterpret_cast<const char*>(&h), sizeof(h));
        f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(width) * height * sizeof(uint16_t));

        return f.good();
    } catch (...) {
        return false;
    }
}

std::vector<uint16_t> readTerrainChunkCache(const std::string& path, int* outWidth, int* outHeight) noexcept {
    if (path.empty())
        return {};

    try {
        std::ifstream f(path, std::ios::binary);
        if (!f)
            return {};

        uint32_t magic = 0;
        uint16_t w = 0, h = 0;

        f.read(reinterpret_cast<char*>(&magic), sizeof(magic));
        f.read(reinterpret_cast<char*>(&w), sizeof(w));
        f.read(reinterpret_cast<char*>(&h), sizeof(h));

        if (!f || magic != kTerrainCacheMagic || w == 0 || h == 0)
            return {};

        std::vector<uint16_t> result(static_cast<size_t>(w) * h);
        f.read(reinterpret_cast<char*>(result.data()), static_cast<std::streamsize>(result.size()) * sizeof(uint16_t));

        if (!f)
            return {};

        if (outWidth)
            *outWidth = static_cast<int>(w);
        if (outHeight)
            *outHeight = static_cast<int>(h);
        return result;
    } catch (...) {
        return {};
    }
}

} // namespace fl
