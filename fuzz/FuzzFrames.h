// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

// Splits one fuzz input into a sequence of length-prefixed frames so a single corpus entry can
// drive several onReceive() calls in order — required to reach the stateful message-handler paths
// (chunk reassembly, delta-after-full snapshot decode, selective-ack advance, heartbeat cadence).
//
// Frame format: [len: uint16_t little-endian][len bytes of payload]. A truncated trailing frame
// (fewer than 2 length bytes, or a length running past the buffer) ends iteration cleanly — the
// callback is never handed an out-of-bounds span, so the split itself introduces no UB for the
// sanitizers to flag; only the payload handed to the target under test matters.

namespace fl {

// Invokes fn(const uint8_t* frame, size_t frameLen) for each well-formed frame, in order.
template <typename Fn> inline void forEachFuzzFrame(const uint8_t* data, size_t size, Fn&& fn) {
    size_t off = 0;
    while (off + 2 <= size) {
        const uint16_t len = static_cast<uint16_t>(data[off] | (static_cast<uint16_t>(data[off + 1]) << 8));
        off += 2;
        if (len > size - off)
            break; // truncated final frame: stop rather than clamp, keeping frame boundaries stable
        std::forward<Fn>(fn)(data + off, static_cast<size_t>(len));
        off += len;
    }
}

} // namespace fl
