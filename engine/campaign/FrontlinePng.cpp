// SPDX-License-Identifier: GPL-3.0-or-later

// STB_IMAGE_STATIC keeps all stbi symbols TU-local so this codec never collides with the other
// stb_image TUs in the build (TerrainChunkIO, tinygltf-impl). clang-format off around the vendored
// header block.
// clang-format off
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
// clang-format on

#include "campaign/FrontlinePng.h"

#include <cstring>

namespace fl {

namespace {
constexpr uint8_t kPngSig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
constexpr int kMaxDim = 8192;

// Reject a PNG whose chunk lengths overrun the buffer before handing it to stb (memory-exhaustion DoS
// on an untrusted content-pack file). Mirrors TerrainChunkIO::pngChunkLengthsSane.
bool pngChunkLengthsSane(const uint8_t* d, size_t n) noexcept {
    if (n < 8)
        return false;
    size_t off = 8;
    while (off + 8 <= n) {
        const uint32_t len = (static_cast<uint32_t>(d[off]) << 24) | (static_cast<uint32_t>(d[off + 1]) << 16) |
                             (static_cast<uint32_t>(d[off + 2]) << 8) | static_cast<uint32_t>(d[off + 3]);
        if (len > n - off - 8)
            return false;
        off += static_cast<size_t>(12) + len;
    }
    return true;
}

// Write-callback that appends to a std::vector (stb_image_write's memory path).
void appendToVector(void* ctx, void* data, int size) {
    auto* out = static_cast<std::vector<uint8_t>*>(ctx);
    const auto* bytes = static_cast<const uint8_t*>(data);
    out->insert(out->end(), bytes, bytes + size);
}
} // namespace

FrontlinePngInfo probeFrontlinePng(const uint8_t* data, size_t size) noexcept {
    FrontlinePngInfo info;
    if (!data || size < sizeof(kPngSig) || std::memcmp(data, kPngSig, sizeof(kPngSig)) != 0) {
        info.error = "not a PNG (bad signature)";
        return info;
    }
    if (!pngChunkLengthsSane(data, size)) {
        info.error = "malformed PNG (chunk lengths overrun the file)";
        return info;
    }
    int w = 0, h = 0, ch = 0;
    if (!stbi_info_from_memory(data, static_cast<int>(size), &w, &h, &ch)) {
        info.error = "unreadable PNG header";
        return info;
    }
    info.width = w;
    info.height = h;
    if (w > kMaxDim || h > kMaxDim) {
        info.error = "PNG dimensions exceed the cap";
        return info;
    }
    // A frontline raster must be authored as 8-bit grayscale (1 channel). Report the authored form,
    // not what stb could convert — a 24-bit RGB PNG would silently "work" but is not what the encoding
    // expects.
    const bool sixteen = stbi_is_16_bit_from_memory(data, static_cast<int>(size)) != 0;
    info.gray8 = (ch == 1) && !sixteen;
    if (!info.gray8) {
        info.error = sixteen ? "PNG is 16-bit, not 8-bit grayscale"
                             : ("PNG has " + std::to_string(ch) + " channels, not 8-bit grayscale");
        return info;
    }
    info.ok = true;
    return info;
}

std::vector<uint8_t> decodeFrontlinePng(const uint8_t* data, size_t size, int* outW, int* outH) noexcept {
    FrontlinePngInfo info = probeFrontlinePng(data, size);
    if (!info.ok)
        return {};
    int w = 0, h = 0, ch = 0;
    stbi_uc* pixels = stbi_load_from_memory(data, static_cast<int>(size), &w, &h, &ch, 1); // force 1 channel
    if (!pixels)
        return {};
    std::vector<uint8_t> out(pixels, pixels + static_cast<size_t>(w) * h);
    stbi_image_free(pixels);
    if (outW)
        *outW = w;
    if (outH)
        *outH = h;
    return out;
}

std::vector<uint8_t> encodeFrontlinePng(const uint8_t* pixels, int w, int h) {
    std::vector<uint8_t> out;
    if (!pixels || w <= 0 || h <= 0)
        return out;
    if (!stbi_write_png_to_func(appendToVector, &out, w, h, /*comp=*/1, pixels, /*stride=*/w))
        out.clear();
    return out;
}

} // namespace fl
