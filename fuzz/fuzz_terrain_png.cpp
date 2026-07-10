// SPDX-License-Identifier: GPL-3.0-or-later

// Fuzz target: decodeTerrainChunkPng — the 16-bit grayscale PNG decoder (stb_image) that ingests
// content-pack terrain chunks. Attacker-controlled PNG bytes reach this from any loaded mod. The
// function is noexcept and returns an empty vector on failure; the invariant is no OOB read / no UB
// inside stb_image for any malformed chunk.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "render/TerrainChunkIO.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    int w = 0, h = 0;
    std::vector<uint16_t> pixels = fl::decodeTerrainChunkPng(data, size, &w, &h);
    (void)pixels;
    (void)w;
    (void)h;
    return 0;
}
