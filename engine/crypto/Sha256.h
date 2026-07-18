// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Clean-room SHA-256 (FIPS 180-4). Stdlib-only, no link deps — deliberately NOT OpenSSL, which is
// gated behind FL_ENABLE_GNS here, so pack/content hashing (#490) must not depend on it. Streaming
// API (reset/update/finalize) so a multi-gigabyte download is hashed incrementally without buffering.
//
// `GameProtocol.h` already reserves a 32-byte contentHash for a future engine-side pack hash; this is
// the implementation behind it and behind ContentDownloader's manifest verification.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace fl {

class Sha256 {
  public:
    static constexpr std::size_t kDigestBytes = 32;
    using Digest = std::array<uint8_t, kDigestBytes>;

    Sha256() noexcept {
        reset();
    }

    void reset() noexcept;
    void update(const void* data, std::size_t len) noexcept;
    // Finalizes and returns the digest. The object is left usable again only after reset().
    [[nodiscard]] Digest finalize() noexcept;

    // One-shot convenience.
    [[nodiscard]] static Digest hash(const void* data, std::size_t len) noexcept;

  private:
    void processBlock(const uint8_t block[64]) noexcept;

    std::array<uint32_t, 8> m_state{};
    uint64_t m_bitLen{0};
    std::array<uint8_t, 64> m_buffer{};
    std::size_t m_bufferLen{0};
};

// Lower-case hex encoding of a digest (64 chars).
[[nodiscard]] std::string sha256Hex(const Sha256::Digest& d);

// One-shot data -> lower-case hex.
[[nodiscard]] std::string sha256Hex(const void* data, std::size_t len);

} // namespace fl
