// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// MSB-first bit packing for the quantized snapshot stream (see SnapshotCodec.h).
//
// Portability contract (the codec must be byte-identical on Linux/macOS/Windows, x86-64 + ARM64):
//   * Bits are assembled with shifts/masks into a uint8_t buffer, never memcpy of a native
//     multi-byte int, so byte order never reaches the wire.
//   * The accumulator is uint64_t and every single writeBits/readBits moves at most 32 bits with
//     at most 7 leftover bits held, so the running width stays <= 39 bits — no shift-by->=width UB.
//   * Only unsigned shifts/masks are used; signedness is handled by the caller (offset-binary in
//     Quantization.h), so there is no implementation-defined signed-shift behaviour.
//   * The reader bounds-checks every refill and returns false on a truncated/malformed buffer
//     rather than reading out of bounds.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace fl {

// Mask of the low n bits (n in [0, 32]; 0 -> 0).
inline constexpr uint64_t bitMask(int n) noexcept {
    return n <= 0 ? 0ull : (n >= 64 ? ~0ull : ((uint64_t{1} << n) - 1ull));
}

// Appends bits MSB-first into an owned byte buffer. Reusable across ticks via clear().
class BitWriter {
  public:
    BitWriter() = default;
    explicit BitWriter(std::size_t reserveBytes) {
        m_bytes.reserve(reserveBytes);
    }

    // Write the low `n` bits of `value` (n in [0, 32]), high bit first.
    void writeBits(uint32_t value, int n) noexcept {
        if (n <= 0)
            return;
        // m_count < 8 on entry and n <= 32, so m_count + n <= 39 — no uint64 overflow.
        m_acc = (m_acc << n) | (static_cast<uint64_t>(value) & bitMask(n));
        m_count += n;
        while (m_count >= 8) {
            m_count -= 8;
            m_bytes.push_back(static_cast<uint8_t>((m_acc >> m_count) & 0xFFu));
        }
        m_acc &= bitMask(m_count); // drop already-emitted high bits
    }

    // LEB128-style unsigned varint (7 payload bits + continuation bit per byte, low group first).
    void writeVarint(uint32_t value) noexcept {
        while (value >= 0x80u) {
            writeBits((value & 0x7Fu) | 0x80u, 8);
            value >>= 7;
        }
        writeBits(value, 8);
    }

    // Pad with zero bits up to the next byte boundary (call once after the last record).
    void alignToByte() noexcept {
        if (m_count > 0)
            writeBits(0u, 8 - m_count);
    }

    const std::vector<uint8_t>& bytes() const noexcept {
        return m_bytes;
    }
    std::size_t byteCount() const noexcept {
        return m_bytes.size();
    }

    void clear() noexcept {
        m_bytes.clear();
        m_acc = 0;
        m_count = 0;
    }

  private:
    std::vector<uint8_t> m_bytes;
    uint64_t m_acc{0};
    int m_count{0}; // pending bits held in m_acc (0..7 between writes)
};

// Reads bits MSB-first from a fixed buffer; every read bounds-checks and fails closed.
class BitReader {
  public:
    BitReader(const void* data, std::size_t size) noexcept : m_data(static_cast<const uint8_t*>(data)), m_size(size) {}

    // Read `n` bits (n in [0, 32]) into out, MSB-first. Returns false if the buffer is exhausted.
    [[nodiscard]] bool readBits(int n, uint32_t& out) noexcept {
        if (n <= 0) {
            out = 0;
            return true;
        }
        while (m_count < n) {
            if (m_bytePos >= m_size)
                return false; // truncated
            m_acc = (m_acc << 8) | m_data[m_bytePos++];
            m_count += 8;
        }
        m_count -= n;
        out = static_cast<uint32_t>((m_acc >> m_count) & bitMask(n));
        m_acc &= bitMask(m_count); // drop consumed high bits
        return true;
    }

    [[nodiscard]] bool readVarint(uint32_t& out) noexcept {
        uint32_t result = 0;
        int shift = 0;
        uint32_t byte = 0;
        do {
            if (!readBits(8, byte))
                return false;
            if (shift < 32)
                result |= (byte & 0x7Fu) << shift;
            shift += 7;
        } while ((byte & 0x80u) != 0u);
        out = result;
        return true;
    }

  private:
    const uint8_t* m_data;
    std::size_t m_size;
    std::size_t m_bytePos{0};
    uint64_t m_acc{0};
    int m_count{0};
};

} // namespace fl
