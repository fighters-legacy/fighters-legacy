// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace fl {

// Decoded PCM output from an OGG Vorbis file (full decode, for short SFX clips).
struct DecodedPcm {
    std::vector<int16_t> samples; // interleaved int16_t; numSamples * channels elements
    int sampleRate{0};
    int channels{0};
    bool valid() const {
        return !samples.empty();
    }
};

// Decompression-bomb cap for decodeOgg(): maximum total interleaved int16_t
// elements (samples x channels) a full decode may produce — 64 MiB of PCM,
// ~6 minutes of 44.1 kHz stereo. Content-pack bytes are attacker-controlled;
// a tiny OGG can legally expand to gigabytes without this. Exceeding the cap
// FAILS the decode (invalid DecodedPcm) rather than truncating.
inline constexpr std::size_t kMaxDecodedSamples = 32u * 1024u * 1024u;

// Fully decodes an OGG Vorbis byte blob into interleaved int16_t PCM.
// Suitable for short SFX clips loaded via AssetManager::loadAudio().
// Returns an invalid (empty) DecodedPcm on any decode failure, including
// output exceeding maxTotalSamples (interleaved elements).
DecodedPcm decodeOgg(std::span<const uint8_t> bytes, std::size_t maxTotalSamples = kMaxDecodedSamples);

// ---------------------------------------------------------------------------
// Opaque streaming handle — for long music tracks decoded chunk-by-chunk.
// MusicManager uses this API; libvorbis (vorbisfile) internals stay in
// OggDecoder.cpp.
// ---------------------------------------------------------------------------
struct OggStream;

struct OggStreamInfo {
    int sampleRate{0};
    int channels{0};
};

// Opens a streaming decoder over a byte span. The caller must keep bytes alive
// for the lifetime of the returned handle. Returns nullptr on failure —
// including structurally invalid streams and streams outside the sanity
// envelope (channels 1..8, sample rate 8000..192000 Hz).
OggStream* openOggStream(std::span<const uint8_t> bytes);

// Metadata of the stream (valid after openOggStream succeeds).
OggStreamInfo getOggStreamInfo(const OggStream* stream);

// Decodes up to numSamples interleaved int16_t frames into buf.
// Returns the number of samples actually decoded (< numSamples at end-of-stream).
int readOggSamples(OggStream* stream, int16_t* buf, int numSamples);

// Seeks to the beginning of the stream for looping.
void seekOggStart(OggStream* stream);

// Closes and frees the stream handle.
void closeOggStream(OggStream* stream);

// RAII holder for OggStream (closeOggStream on destruction).
struct OggStreamCloser {
    void operator()(OggStream* stream) const {
        closeOggStream(stream);
    }
};
using OggStreamPtr = std::unique_ptr<OggStream, OggStreamCloser>;

} // namespace fl
