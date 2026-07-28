// SPDX-License-Identifier: GPL-3.0-or-later

// Fuzz target: the `.flrep` replay reader (#643).
//
// A replay is the one artifact this engine produces that a player DOWNLOADS FROM A STRANGER, so the
// reader parses attacker-controlled bytes by definition — docs/developer/replay-format.md §5 says so, and
// says the reader joins fuzz/ for that reason.
//
// Two entry points, because the file has two parsers with different exposure:
//
//   1. decodeTick over the raw buffer — the hot inner parser, and the one reached with a chunk's
//      DECOMPRESSED bytes, i.e. after an attacker has had a zstd frame's worth of amplification.
//   2. ReplayReader::open on a real file — the header, section list, trailer and chunk framing,
//      including the forward-scan index rebuild. It needs a path, so the input is written to a temp
//      file; that costs an iteration but it is the actual public entry point.
//
// The invariant is the usual one: no OOB read, no UB, no allocation sized off a number the file
// merely claims — under ASan + UBSan, for any input.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include <replay/ReplayFormat.h>
#include <replay/ReplayReader.h>

namespace {

std::filesystem::path scratchPath() {
    static const std::filesystem::path p = std::filesystem::temp_directory_path() / "fl_fuzz_flrep.tmp";
    return p;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // 1. The tick decoder, driven directly on the raw bytes.
    {
        std::size_t cursor = 0;
        fl::ReplayTick tick;
        // Loop so a well-formed prefix followed by garbage is exercised too: the cursor must never
        // advance past the buffer, and a failed decode must not leave a half-populated tick behind.
        for (int i = 0; i < 8; ++i) {
            if (!fl::ReplayReader::decodeTick(data, size, cursor, tick))
                break;
            if (cursor >= size)
                break;
        }
    }

    // 2. The whole-file path: header, sections, trailer or forward scan, chunk decompression.
    {
        const std::filesystem::path p = scratchPath();
        {
            std::ofstream out(p, std::ios::binary | std::ios::trunc);
            if (!out)
                return 0;
            out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
        }

        fl::ReplayReader r;
        if (r.open(p)) {
            fl::ReplayTick tick;
            // Bounded: a hostile index could otherwise describe an enormous stream, and the point is
            // memory safety, not endurance.
            for (int i = 0; i < 64 && r.readNextTick(tick); ++i) {
            }
            // Seeks resolve through the index, which the file itself supplied.
            (void)r.seekToKeyframeAtOrBefore(0);
            (void)r.seekToKeyframeAtOrBefore(UINT64_MAX);
            (void)r.rewind();
        }
    }

    return 0;
}
