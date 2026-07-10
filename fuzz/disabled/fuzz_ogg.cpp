// SPDX-License-Identifier: GPL-3.0-or-later

// DISABLED HARNESS — parked, not built or run. See fuzz/disabled/README.md.
//
// Fuzz target: the OGG Vorbis decode paths (stb_vorbis) — decodeOgg (full decode) plus the
// streaming API (openOggStream / getOggStreamInfo / readOggSamples / seekOggStart / closeOggStream)
// that MusicManager drives. Content-pack audio is attacker-controlled, so this IS a real attack
// surface — but stb_vorbis is a trusted-input decoder with memory-safety defects on malformed input
// (a SEGV in its own vorbis_deinit/setup_free cleanup, plus integer-overflow-driven wild allocations)
// that no sanitizer configuration papers over. The harness is preserved for the day the underlying
// OGG decode path is hardened or sandboxed (tracked as #723); re-enabling instructions are in
// fuzz/disabled/README.md.

#include <cstddef>
#include <cstdint>
#include <span>

#include "audio/OggDecoder.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::span<const uint8_t> bytes(data, size);

    (void)fl::decodeOgg(bytes);

    // `data` stays alive for the whole call, satisfying openOggStream's keep-bytes-alive contract.
    fl::OggStream* stream = fl::openOggStream(bytes);
    if (stream) {
        (void)fl::getOggStreamInfo(stream);
        int16_t buf[512];
        int decoded = 0;
        // Bound the drain so a huge/looping stream can't run the smoke past its time budget.
        while (decoded < 200000) {
            const int n = fl::readOggSamples(stream, buf, 512);
            if (n <= 0)
                break;
            decoded += n;
        }
        fl::seekOggStart(stream);
        (void)fl::readOggSamples(stream, buf, 512);
        fl::closeOggStream(stream);
    }
    return 0;
}
