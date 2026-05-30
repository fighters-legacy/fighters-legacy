// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <span>
#include <vector>

// Decoded PCM output from an OGG Vorbis file (full decode, for short SFX clips).
struct DecodedPcm {
    std::vector<int16_t> samples; // interleaved int16_t; numSamples * channels elements
    int sampleRate{0};
    int channels{0};
    bool valid() const {
        return !samples.empty();
    }
};

// Fully decodes an OGG Vorbis byte blob into interleaved int16_t PCM.
// Suitable for short SFX clips loaded via AssetManager::loadAudio().
// Returns an invalid (empty) DecodedPcm on any decode failure.
DecodedPcm decodeOgg(std::span<const uint8_t> bytes);

// ---------------------------------------------------------------------------
// Opaque streaming handle — for long music tracks decoded chunk-by-chunk.
// MusicManager uses this API; stb_vorbis internals stay in OggDecoder.cpp.
// ---------------------------------------------------------------------------
struct OggStream;

struct OggStreamInfo {
    int sampleRate{0};
    int channels{0};
};

// Opens a streaming decoder over a byte span. The caller must keep bytes alive
// for the lifetime of the returned handle. Returns nullptr on failure.
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
