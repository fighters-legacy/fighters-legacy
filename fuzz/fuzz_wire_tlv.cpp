// SPDX-License-Identifier: GPL-3.0-or-later

// Fuzz target: the TLV extension-block scanner (fl::findExt / fl::readExtValue). The whole input is
// treated as an extension region, so the fuzzer explores malformed [tag:u16 LE][len:u16 LE][data]
// chains — truncated headers, lengths that overflow the window, unknown tags, size mismatches.
// findExt bounds-checks each entry and breaks on overflow; the harness asserts no OOB read / no UB.

#include <cstddef>
#include <cstdint>

#include <net/GameProtocol.h>
#include <net/WireCodec.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const uint16_t tags[] = {
        static_cast<uint16_t>(fl::ExtTag::SnapshotPeerCount),
        static_cast<uint16_t>(fl::ExtTag::SnapshotPeerLatency),
        static_cast<uint16_t>(fl::ExtTag::SnapshotPeerDelayTicks),
        static_cast<uint16_t>(fl::ExtTag::SnapshotDespawn),
        0x0000u, // absent/unknown tag: exercises the skip-to-end path
    };

    for (uint16_t tag : tags) {
        uint16_t valueLen = 0;
        const uint8_t* p = fl::findExt(data, size, tag, valueLen);
        if (p) {
            // Touch every byte of the returned value window to trip ASan on any over-read.
            volatile uint8_t sink = 0;
            for (uint16_t i = 0; i < valueLen; ++i)
                sink = static_cast<uint8_t>(sink ^ p[i]);
            (void)sink;
        }

        // Typed readers: the fixed-width snapshot extensions are uint16_t; despawn is a uint32_t[].
        uint16_t v16 = 0;
        (void)fl::readExtValue<uint16_t>(data, size, tag, v16);
        uint32_t v32 = 0;
        (void)fl::readExtValue<uint32_t>(data, size, tag, v32);
    }
    return 0;
}
