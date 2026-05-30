// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

// Self-contained raw PCM output from a synthesiser.
// Intentionally mirrors engine/audio/OggDecoder.h::DecodedPcm but lives in platform/
// so that IAudioSynthesizer stays free of engine/ header dependencies.
struct SynthesisedAudio {
    std::vector<int16_t> samples; // interleaved int16_t; numSamples * channels elements
    int sampleRate{0};
    int channels{0};
    bool valid() const {
        return !samples.empty();
    }
};

// Pure-virtual TTS/speech-synthesis hook.
// No implementation ships in this PR. Future backends (e.g. Piper TTS, a cloud API)
// implement this interface and are injected into VoiceCalloutManager::init().
//
// Threading: synthesise() is called on the main thread, same as AssetManager::loadAudio().
// Blocking is permitted; callers treat it like a synchronous asset load.
//
// Design rules (consistent with all other HAL interfaces):
//   - No platform-specific headers in this file.
//   - Single method; callers fall back to the OGG asset on false.
class IAudioSynthesizer {
  public:
    virtual ~IAudioSynthesizer() = default;

    // Synthesise speech from text into decoded int16_t PCM.
    // Returns false if synthesis is unavailable; caller falls back to the pre-recorded OGG asset.
    virtual bool synthesise(std::string_view text, SynthesisedAudio& out) = 0;
};
