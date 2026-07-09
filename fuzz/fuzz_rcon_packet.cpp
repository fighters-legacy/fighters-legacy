// SPDX-License-Identifier: GPL-3.0-or-later

// Fuzz target: the Source-Engine RCON packet decoder (fl::rcon::decodePacket). This is the parse
// path for bytes arriving on the RCON admin TCP socket. decodePacket returns bytes-consumed (>0),
// 0 (need more), or -1 (malformed); the real server loops it to drain a stream, so the harness does
// the same — driving multi-packet buffers and the length-field boundary logic under ASan+UBSan.

#include <cstddef>
#include <cstdint>

#include <RconServer.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    int offset = 0;
    const int len = static_cast<int>(size);
    // Bound the loop by consumed bytes; decodePacket returns <= 0 to stop (need-more / malformed).
    for (int guard = 0; guard < 4096 && offset < len; ++guard) {
        fl::rcon::RconPacket out;
        const int consumed = fl::rcon::decodePacket(data + offset, len - offset, out);
        if (consumed <= 0)
            break;
        offset += consumed;
    }
    return 0;
}
