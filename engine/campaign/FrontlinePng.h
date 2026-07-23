// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The 8-bit-grayscale frontline raster PNG codec (#847): the ONE place a campaign frontline PNG is
// decoded, shared by fl-server (to feed Frontline::setPixels) and validate-campaign (to check bit
// depth + dimensions). Kept in its own target (engine-campaign-png) so engine-campaign holds no image
// library — Frontline stores decoded pixels only, by design. Untrusted-input hardened like
// TerrainChunkIO's decoder (PNG signature, chunk-length sanity, dimension cap before decode).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fl {

// Header-only inspection of a frontline PNG (no pixel decode): the dimensions, whether it is a valid
// 8-bit grayscale (1-channel) PNG, and a human error string on failure. Lets validate-campaign report
// the DISTINCT diagnostics the issue requires ("not 8-bit grayscale" vs "dimensions X != Y").
struct FrontlinePngInfo {
    bool ok{false};
    int width{0};
    int height{0};
    bool gray8{false};
    std::string error;
};

[[nodiscard]] FrontlinePngInfo probeFrontlinePng(const uint8_t* data, size_t size) noexcept;

// Decode an 8-bit grayscale frontline PNG to row-major single-channel bytes (width*height). Returns
// empty on any failure (not a PNG, not 8-bit, not 1-channel, or oversized). outW/outH filled on
// success.
[[nodiscard]] std::vector<uint8_t> decodeFrontlinePng(const uint8_t* data, size_t size, int* outW, int* outH) noexcept;

// Encode row-major single-channel bytes as an 8-bit grayscale PNG (tests / tools produce fixtures with
// no committed binaries). Empty on failure.
[[nodiscard]] std::vector<uint8_t> encodeFrontlinePng(const uint8_t* pixels, int w, int h);

} // namespace fl
