// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IAudio.h"
#include "audio/OggDecoder.h" // DecodedPcm
#include "math/Angles.h"      // kPi — the one pi (#1246)

#include <cstddef>
#include <cstdint>

namespace fl {

// The primitives every builtin-PCM generator needs, in one place (#1265).
//
// engine/audio has four procedural generators — SFX, engine hum, warning tones and music — and each
// opened with its own float->int16 conversion, its own noise hash, and its own DecodedPcm factory.
// The hash in particular was copied on purpose: EngineAudio.cpp's comment says it is "the
// SfxBuiltinSounds hash, so the waveform is byte-stable everywhere", which is a correctness claim
// resting on two literals staying equal. It is now one literal.
//
// ⚠ NOT for engine/voice's RadioDsp. That TU compiles into engine-radio, which is deliberately
// stdlib-only so engine-net can consume it WITHOUT linking libopus (#925). Including this header
// there would invert audio -> radio and defeat that isolation, so RadioDsp keeps its own two-line
// conversion. Two lines is cheaper than the dependency.

// Float sample to int16. Clamp to [-1, 1], then a TRUNCATING cast — 0.99999f becomes 32766, not
// 32767. That is what all four copies did, and the builtin waveforms are what they are because of
// it; rounding here would move every generated sample.
[[nodiscard]] inline int16_t toI16(float v) noexcept {
    if (v > 1.f)
        v = 1.f;
    if (v < -1.f)
        v = -1.f;
    return static_cast<int16_t>(v * 32767.f);
}

// Deterministic white noise in [-1, 1] from a sample index — a fixed hash, never rand()/time, so
// the waveform is byte-identical on every machine and every run. Stateless on purpose: a generator
// can produce sample N without having produced N-1.
[[nodiscard]] inline float noise(uint32_t i) noexcept {
    uint32_t h = i * 0x9E3779B1u + 0x27D4EB2Fu;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return (static_cast<float>(h & 0xFFFFu) / 32768.f) - 1.f;
}

// A zeroed mono DecodedPcm the caller then fills. The sample rate is a parameter because each
// generator has its own — that is the one thing the four factories genuinely disagreed about.
[[nodiscard]] inline DecodedPcm makeMonoPcm(int sampleRate, std::size_t samples) {
    DecodedPcm pcm;
    pcm.sampleRate = sampleRate;
    pcm.channels = 1;
    pcm.samples.resize(samples);
    return pcm;
}

// Hand a clip to the audio backend. Eight sites spelled out the same
// `samples.data(), samples.size() * sizeof(int16_t), sampleRate, channels` — a byte count derived
// from an element count, which is the kind of expression that is wrong exactly once and then
// silently plays half a clip.
//
// Templated because DecodedPcm (a decoded OGG) and SynthesisedAudio (a TTS backend's output) are
// field-for-field the same clip as far as an upload is concerned, and both were spelling this out.
template <class Clip> [[nodiscard]] AudioBufferId uploadPcm(IAudio& audio, const Clip& pcm) {
    return audio.uploadBuffer(pcm.samples.data(), pcm.samples.size() * sizeof(int16_t), pcm.sampleRate, pcm.channels);
}

} // namespace fl
