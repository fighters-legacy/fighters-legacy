// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Explicit little-endian byte serialization for the engine's ON-DISK formats (FLIT input traces,
// `.flrep` replays).
//
// The wire structs in GameProtocol.h are naturally aligned and memcpy'd on purpose: producer and
// consumer are the same build talking over a socket. A FILE is different. It crosses machines and
// outlives the build that wrote it, so every field is written byte by byte at a fixed endianness and
// never as a native word. These helpers are that discipline in one place -- they were FLIT's private
// `fl::detail` block until `.flrep` needed the same rules and a second copy would have been a second
// chance to disagree about what a u32 looks like.
//
// Floats go through their IEEE-754 bit pattern via memcpy (never a reinterpret_cast, which is a
// strict-aliasing violation the optimiser is allowed to act on).

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace fl::detail {

inline void putU8(std::vector<uint8_t>& b, uint8_t v) {
    b.push_back(v);
}
inline void putU16LE(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v & 0xFFu));
    b.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
}
inline void putU32LE(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i)
        b.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFFu));
}
inline void putU64LE(std::vector<uint8_t>& b, uint64_t v) {
    for (int i = 0; i < 8; ++i)
        b.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFFu));
}
inline void putF32LE(std::vector<uint8_t>& b, float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    putU32LE(b, bits);
}
inline void putF64LE(std::vector<uint8_t>& b, double v) {
    uint64_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    putU64LE(b, bits);
}

// Length-prefixed UTF-8. A file wants to not truncate a callsign, so strings are never the wire's
// NUL-padded fixed arrays. The u32 length is the byte count, not a codepoint count.
inline void putStringLE(std::vector<uint8_t>& b, std::string_view s) {
    putU32LE(b, static_cast<uint32_t>(s.size()));
    b.insert(b.end(), s.begin(), s.end());
}

inline uint16_t getU16LE(const uint8_t* p) {
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8));
}
inline uint32_t getU32LE(const uint8_t* p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
        v |= static_cast<uint32_t>(p[i]) << (8 * i);
    return v;
}
inline uint64_t getU64LE(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return v;
}
inline float getF32LE(const uint8_t* p) {
    const uint32_t bits = getU32LE(p);
    float v;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}
inline double getF64LE(const uint8_t* p) {
    const uint64_t bits = getU64LE(p);
    double v;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

// Bounds-checked forward cursor over a byte buffer. Every read is checked against the REMAINING
// buffer before it happens and the cursor fails closed: once `ok` is false it stays false and every
// subsequent read yields zero, so a truncated or hostile file produces a clean refusal instead of a
// partially-populated struct that looks valid (the BitReader contract, applied to file parsing).
class ByteCursor {
  public:
    ByteCursor(const uint8_t* data, std::size_t size) noexcept : m_data(data), m_size(size) {}

    [[nodiscard]] bool ok() const noexcept {
        return m_ok;
    }
    [[nodiscard]] std::size_t offset() const noexcept {
        return m_pos;
    }
    [[nodiscard]] std::size_t remaining() const noexcept {
        return m_ok ? m_size - m_pos : 0;
    }
    void fail() noexcept {
        m_ok = false;
    }

    [[nodiscard]] bool need(std::size_t n) noexcept {
        if (!m_ok || m_size - m_pos < n) {
            m_ok = false;
            return false;
        }
        return true;
    }

    uint8_t u8() noexcept {
        if (!need(1))
            return 0;
        return m_data[m_pos++];
    }
    uint16_t u16() noexcept {
        if (!need(2))
            return 0;
        const uint16_t v = getU16LE(m_data + m_pos);
        m_pos += 2;
        return v;
    }
    uint32_t u32() noexcept {
        if (!need(4))
            return 0;
        const uint32_t v = getU32LE(m_data + m_pos);
        m_pos += 4;
        return v;
    }
    uint64_t u64() noexcept {
        if (!need(8))
            return 0;
        const uint64_t v = getU64LE(m_data + m_pos);
        m_pos += 8;
        return v;
    }
    float f32() noexcept {
        if (!need(4))
            return 0.f;
        const float v = getF32LE(m_data + m_pos);
        m_pos += 4;
        return v;
    }
    double f64() noexcept {
        if (!need(8))
            return 0.0;
        const double v = getF64LE(m_data + m_pos);
        m_pos += 8;
        return v;
    }

    // Reads a length-prefixed string. The declared length is checked against what is actually left
    // BEFORE any allocation -- a 4 GiB length in a 40-byte file must cost nothing.
    std::string str() {
        const uint32_t len = u32();
        if (!need(len))
            return {};
        std::string s(reinterpret_cast<const char*>(m_data + m_pos), len);
        m_pos += len;
        return s;
    }

    // Borrow `n` raw bytes without copying; nullptr (and failure) when they are not there.
    const uint8_t* bytes(std::size_t n) noexcept {
        if (!need(n))
            return nullptr;
        const uint8_t* p = m_data + m_pos;
        m_pos += n;
        return p;
    }

    void skip(std::size_t n) noexcept {
        if (need(n))
            m_pos += n;
    }

  private:
    const uint8_t* m_data{nullptr};
    std::size_t m_size{0};
    std::size_t m_pos{0};
    bool m_ok{true};
};

} // namespace fl::detail
