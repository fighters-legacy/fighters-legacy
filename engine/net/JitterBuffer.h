// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>

namespace fl {

// Control inputs extracted from MsgClientInput, validated and clamped by WorldBroadcaster::onReceive.
struct BufferedInput {
    float throttle{0.f};
    float elevator{0.f};
    float aileron{0.f};
    float rudder{0.f};
    uint8_t buttons{0};
};

// Fixed-depth ring buffer for per-peer MsgClientInput delivery.
// When full, push() drops the oldest entry (drop-oldest overflow policy).
// Sim thread only — no internal synchronisation.
class JitterBuffer {
  public:
    static constexpr uint32_t kHardMaxDepth = 32u;

    explicit JitterBuffer(uint32_t maxDepth = 4) noexcept
        : m_maxDepth(maxDepth == 0u ? 1u : (maxDepth > kHardMaxDepth ? kHardMaxDepth : maxDepth)) {}

    // Clamps d to [1, kHardMaxDepth]. If d < size(), drops oldest entries to fit.
    void setMaxDepth(uint32_t d) noexcept {
        if (d == 0u)
            d = 1u;
        if (d > kHardMaxDepth)
            d = kHardMaxDepth;
        m_maxDepth = d;
        while (m_size > m_maxDepth) {
            m_readHead = (m_readHead + 1u) % kHardMaxDepth;
            --m_size;
        }
    }

    uint32_t maxDepth() const noexcept {
        return m_maxDepth;
    }
    uint32_t size() const noexcept {
        return m_size;
    }
    bool empty() const noexcept {
        return m_size == 0u;
    }

    // Enqueue one input. If full, the oldest entry is silently dropped.
    void push(const BufferedInput& in) noexcept {
        if (m_size == m_maxDepth) {
            // Drop oldest: advance read head without changing size.
            m_readHead = (m_readHead + 1u) % kHardMaxDepth;
        } else {
            ++m_size;
        }
        m_slots[m_writeHead] = in;
        m_writeHead = (m_writeHead + 1u) % kHardMaxDepth;
    }

    // Dequeue one input into out. Returns false (and leaves out unchanged) when empty.
    bool pop(BufferedInput& out) noexcept {
        if (m_size == 0u)
            return false;
        out = m_slots[m_readHead];
        m_readHead = (m_readHead + 1u) % kHardMaxDepth;
        --m_size;
        return true;
    }

  private:
    uint32_t m_maxDepth;
    uint32_t m_size{0u};
    uint32_t m_readHead{0u};
    uint32_t m_writeHead{0u};
    // Plain C array avoids std::array<T, uint32_t> implicit-conversion warning on MSVC
    // (CMAKE_COMPILE_WARNING_AS_ERROR=ON).
    BufferedInput m_slots[kHardMaxDepth]{};
};

} // namespace fl
