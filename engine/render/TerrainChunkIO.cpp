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

std::vector<uint16_t> decodeTerrainChunkPng(const uint8_t* data, size_t size, int* outWidth, int* outHeight) noexcept {
    if (!data || size == 0)
        return {};

    // Reject non-16-bit sources — 8-bit PNG silently scaled to 16 would corrupt
    // the height encoding (offset=32768, scale=1 convention).
    if (!stbi_is_16_bit_from_memory(data, static_cast<int>(size)))
        return {};

    int w = 0, h = 0, ch = 0;
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
