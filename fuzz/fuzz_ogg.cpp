// SPDX-License-Identifier: GPL-3.0-or-later

// Fuzz target: the OGG Vorbis decode paths — decodeOgg (full decode, with its decompression-bomb
// cap) plus the streaming API (openOggStream / getOggStreamInfo / readOggSamples / seekOggStart /
// closeOggStream) that MusicManager drives. Content-pack audio is attacker-controlled, so this is
// a real attack surface: everything past the 4-byte OggS magic that AssetValidator checks reaches
// the decoder unvalidated. Backed by libvorbis (vorbisfile) since #723 — the earlier stb_vorbis
// backend was a trusted-input decoder with real memory-safety defects on malformed input, which is
// why this harness spent time parked in fuzz/disabled/. Invariant: no crash, leak, OOM, or hang on
// any input; rejected input returns cleanly.

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
