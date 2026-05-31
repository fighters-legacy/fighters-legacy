// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fl {

// ---------------------------------------------------------------------------
// PNG decode (for content-pack chunks — 16-bit grayscale PNG)
// ---------------------------------------------------------------------------

// Decode a 16-bit grayscale PNG from memory into a row-major uint16_t array.
// Sets *outWidth and *outHeight on success. Returns an empty vector on failure
// (corrupt data, wrong bit-depth, etc.).
std::vector<uint16_t> decodeTerrainChunkPng(const uint8_t* data, size_t size, int* outWidth, int* outHeight) noexcept;

// ---------------------------------------------------------------------------
// Binary cache (for engine-generated procedural chunks)
//
// Format: 4-byte magic (kTerrainCacheMagic) + uint16_t width + uint16_t height
//         + width*height uint16_t values, row-major, little-endian.
// Files use the ".u16" extension to distinguish from content-pack ".png" files.
// ---------------------------------------------------------------------------

static constexpr uint32_t kTerrainCacheMagic = 0x464C4348u; // 'FLCH'

// Write a heightmap to a binary cache file. Creates parent directories.
// Returns false on failure.
bool writeTerrainChunkCache(const std::string& path, const uint16_t* data, int width, int height) noexcept;

// Read a binary cache file written by writeTerrainChunkCache.
// Sets *outWidth and *outHeight on success. Returns empty vector on failure.
std::vector<uint16_t> readTerrainChunkCache(const std::string& path, int* outWidth, int* outHeight) noexcept;

} // namespace fl
