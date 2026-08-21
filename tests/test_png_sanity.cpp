// SPDX-License-Identifier: GPL-3.0-or-later
//
// platform/PngSanity.h (#1237): the ONE untrusted-PNG chunk-length guard behind the terrain
// decoder, the campaign frontline decoder and the Vulkan texture loader. Its contract: a chunk
// declaring more data than the buffer holds is rejected BEFORE any decoder allocates the declared
// length — the memory-exhaustion DoS a hostile or corrupt content-pack PNG would otherwise trigger.

#include <PngSanity.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

using namespace fl;

namespace {

std::vector<uint8_t> sigOnly() {
    return {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
}

// Append one chunk header declaring `len` data bytes, then `actual` bytes of data + a 4-byte CRC.
void appendChunk(std::vector<uint8_t>& png, uint32_t len, uint32_t actual) {
    png.push_back(static_cast<uint8_t>(len >> 24));
    png.push_back(static_cast<uint8_t>(len >> 16));
    png.push_back(static_cast<uint8_t>(len >> 8));
    png.push_back(static_cast<uint8_t>(len));
    for (const char c : {'I', 'D', 'A', 'T'})
        png.push_back(static_cast<uint8_t>(c));
    for (uint32_t i = 0; i < actual + 4; ++i) // data + crc
        png.push_back(0);
}

} // namespace

TEST_CASE("hasPngSignature: signature, non-signature, short and null input") {
    const auto sig = sigOnly();
    CHECK(hasPngSignature(sig.data(), sig.size()));
    const uint8_t jpeg[] = {0xFF, 0xD8, 0xFF, 0xE0, 0, 0, 0, 0};
    CHECK_FALSE(hasPngSignature(jpeg, sizeof(jpeg)));
    CHECK_FALSE(hasPngSignature(sig.data(), 4));
    CHECK_FALSE(hasPngSignature(nullptr, 8));
}

TEST_CASE("pngChunkLengthsSane: well-formed chunks pass") {
    auto png = sigOnly();
    appendChunk(png, 16, 16);
    appendChunk(png, 0, 0);
    CHECK(pngChunkLengthsSane(png.data(), png.size()));
}

TEST_CASE("pngChunkLengthsSane: a truncated IDAT declaring more data than the file holds is rejected") {
    // The DoS shape: a tiny file whose IDAT claims a huge length. stb allocates the DECLARED length
    // up-front, so this must be refused before the decoder ever sees it.
    auto png = sigOnly();
    appendChunk(png, 0x0FFFFFFF, 8); // declares 256 MB, carries 8 bytes
    CHECK_FALSE(pngChunkLengthsSane(png.data(), png.size()));
}

TEST_CASE("pngChunkLengthsSane: shorter than a signature is rejected") {
    const auto sig = sigOnly();
    CHECK_FALSE(pngChunkLengthsSane(sig.data(), 7));
}
