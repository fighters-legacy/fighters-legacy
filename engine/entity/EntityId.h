// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace fl {

// Stable opaque handle to a live entity. The 32-bit generation field increments each time a
// pool slot is recycled, making stale handles detectable at O(1) without scanning the pool.
//
// Safety contract: raw pointers returned by EntityPool::get() are invalidated by any alloc()
// that triggers vector reallocation. Always store EntityId, not raw pointers, across ticks.
struct EntityId {
    uint32_t index{0};
    uint32_t generation{0}; // 0 == null/invalid

    [[nodiscard]] bool valid() const noexcept {
        return generation != 0;
    }
    static constexpr EntityId null() noexcept {
        return {};
    }
    bool operator==(const EntityId&) const noexcept = default;
};

} // namespace fl
