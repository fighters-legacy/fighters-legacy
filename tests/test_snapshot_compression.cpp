// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for the engine-layer snapshot payload codec (#775): round-trip fidelity, the
// raw-fallback contract (tiny / incompressible payloads), the receiver's fail-closed bounds
// (oversized or mismatched claims, garbage frames), and determinism (the property the #512
// serial-equivalence guarantee leans on).
#include "net/SnapshotCompression.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <numeric>
#include <vector>

namespace {

// Highly compressible payload: repeating structured bytes, like a snapshot full of similar records.
std::vector<uint8_t> compressiblePayload(std::size_t n) {
    std::vector<uint8_t> v(n);
    for (std::size_t i = 0; i < n; ++i)
        v[i] = static_cast<uint8_t>((i % 24u) + (i / 96u));
    return v;
}

// Incompressible payload: a deterministic xorshift byte stream (no PRNG from <random> — this must
// be byte-stable across platforms so the "not strictly smaller" branch is hit reliably).
std::vector<uint8_t> incompressiblePayload(std::size_t n) {
    std::vector<uint8_t> v(n);
    uint32_t x = 0x9E3779B9u;
    for (std::size_t i = 0; i < n; ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        v[i] = static_cast<uint8_t>(x);
    }
    return v;
}

} // namespace

TEST_CASE("SnapshotCompression: round-trips a compressible payload", "[compress]") {
    const auto src = compressiblePayload(1200);
    std::vector<uint8_t> compressed;
    const std::size_t csz = fl::compressSnapshotPayload(src.data(), src.size(), compressed);
    REQUIRE(csz > 0u);
    CHECK(csz < src.size()); // strictly smaller, else the caller would have sent raw
    CHECK(compressed.size() == csz);

    std::vector<uint8_t> back;
    REQUIRE(fl::decompressSnapshotPayload(compressed.data(), csz, static_cast<uint32_t>(src.size()), back));
    CHECK(back == src);
}

TEST_CASE("SnapshotCompression: payloads under the minimum are not compressed", "[compress]") {
    const auto src = compressiblePayload(fl::kMinSnapshotCompressBytes - 1u);
    std::vector<uint8_t> compressed;
    CHECK(fl::compressSnapshotPayload(src.data(), src.size(), compressed) == 0u);
}

TEST_CASE("SnapshotCompression: incompressible payloads fall back to raw", "[compress]") {
    const auto src = incompressiblePayload(1200);
    std::vector<uint8_t> compressed;
    // zstd cannot strictly shrink high-entropy input; the contract is 0 => caller sends raw.
    CHECK(fl::compressSnapshotPayload(src.data(), src.size(), compressed) == 0u);
}

TEST_CASE("SnapshotCompression: decompress rejects an oversized claim before allocating", "[compress]") {
    const auto src = compressiblePayload(1200);
    std::vector<uint8_t> compressed;
    REQUIRE(fl::compressSnapshotPayload(src.data(), src.size(), compressed) > 0u);
    std::vector<uint8_t> back;
    CHECK_FALSE(fl::decompressSnapshotPayload(compressed.data(), compressed.size(),
                                              static_cast<uint32_t>(fl::kMaxSnapshotPayloadBytes + 1u), back));
}

TEST_CASE("SnapshotCompression: decompress rejects a claim that does not match the frame", "[compress]") {
    const auto src = compressiblePayload(1200);
    std::vector<uint8_t> compressed;
    REQUIRE(fl::compressSnapshotPayload(src.data(), src.size(), compressed) > 0u);
    std::vector<uint8_t> back;
    // Both directions: a short claim truncates, a long claim under-fills — each is malformed framing.
    CHECK_FALSE(fl::decompressSnapshotPayload(compressed.data(), compressed.size(),
                                              static_cast<uint32_t>(src.size() - 1u), back));
    CHECK_FALSE(fl::decompressSnapshotPayload(compressed.data(), compressed.size(),
                                              static_cast<uint32_t>(src.size() + 1u), back));
}

TEST_CASE("SnapshotCompression: decompress rejects garbage and empty frames", "[compress]") {
    std::vector<uint8_t> garbage(64, 0xA5u);
    std::vector<uint8_t> back;
    CHECK_FALSE(fl::decompressSnapshotPayload(garbage.data(), garbage.size(), 1200u, back));
    CHECK_FALSE(fl::decompressSnapshotPayload(garbage.data(), 0u, 1200u, back));
    CHECK_FALSE(fl::decompressSnapshotPayload(nullptr, 32u, 1200u, back));
    CHECK_FALSE(fl::decompressSnapshotPayload(garbage.data(), garbage.size(), 0u, back));
}

TEST_CASE("SnapshotCompression: identical input compresses to identical bytes", "[compress]") {
    // Context reuse across calls must not leak state into the output — the property that keeps the
    // parallel per-peer build byte-identical across worker counts (#512).
    const auto a = compressiblePayload(900);
    const auto b = incompressiblePayload(300); // interleave something else through the same thread ctx
    std::vector<uint8_t> first, junk, second;
    REQUIRE(fl::compressSnapshotPayload(a.data(), a.size(), first) > 0u);
    (void)fl::compressSnapshotPayload(b.data(), b.size(), junk);
    REQUIRE(fl::compressSnapshotPayload(a.data(), a.size(), second) > 0u);
    CHECK(first == second);
}
