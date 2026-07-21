// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/EngineAudio.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace fl {

namespace {

constexpr float kPi = 3.14159265358979323846f;

int16_t toI16(float v) {
    v = std::clamp(v, -1.f, 1.f);
    return static_cast<int16_t>(v * 32767.f);
}

// Deterministic white noise in [-1, 1] from a sample index — the SfxBuiltinSounds hash, so the
// waveform is byte-stable everywhere.
float noise(uint32_t i) {
    uint32_t h = i * 0x9E3779B1u + 0x27D4EB2Fu;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return (static_cast<float>(h & 0xFFFFu) / 32768.f) - 1.f;
}

DecodedPcm makeMono(std::size_t samples) {
    DecodedPcm p;
    p.sampleRate = kEngineAudioSampleRate;
    p.channels = 1;
    p.samples.resize(samples);
    return p;
}

// A turbine hum: a harmonic stack over an 80 Hz fundamental. The buffer holds a WHOLE number of
// cycles of every harmonic, so it loops with no seam. 0.2 s at 22050 Hz = 4410 samples ⇒ any
// frequency that is a multiple of (sampleRate / 4410 = 5 Hz) completes an integer number of cycles;
// 80/160/240/320 all qualify.
DecodedPcm makeEngineLoop() {
    constexpr std::size_t kN = 4410; // 0.2 s
    DecodedPcm p = makeMono(kN);
    constexpr float kFundamental = 80.f;
    // Harmonic weights (fundamental strongest) — a slightly buzzy, engine-like timbre.
    constexpr float kWeights[4] = {1.0f, 0.55f, 0.30f, 0.18f};
    float norm = 0.f;
    for (float w : kWeights)
        norm += w;
    for (std::size_t i = 0; i < kN; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kEngineAudioSampleRate);
        float v = 0.f;
        for (int h = 0; h < 4; ++h)
            v += kWeights[h] * std::sin(2.f * kPi * kFundamental * static_cast<float>(h + 1) * t);
        p.samples[i] = toI16(0.6f * v / norm);
    }
    return p;
}

// A broadband airframe rush: deterministic noise low-passed with a CIRCULAR moving average, so the
// filtered signal is exactly periodic over the buffer and the loop seam is continuous. 1 s buffer.
DecodedPcm makeWindLoop() {
    constexpr std::size_t kN = kEngineAudioSampleRate; // 1 s
    constexpr int kHalf = 6;                           // moving-average half-window (13-tap) — softens the hiss
    DecodedPcm p = makeMono(kN);
    for (std::size_t i = 0; i < kN; ++i) {
        float acc = 0.f;
        for (int k = -kHalf; k <= kHalf; ++k) {
            const std::size_t idx = (i + static_cast<std::size_t>(k + static_cast<int>(kN))) % kN; // wrap (circular)
            acc += noise(static_cast<uint32_t>(idx));
        }
        p.samples[i] = toI16(0.5f * acc / static_cast<float>(2 * kHalf + 1));
    }
    return p;
}

} // namespace

DecodedPcm generateEngineLoopPcm(EngineLoopKind kind) {
    switch (kind) {
    case EngineLoopKind::Engine:
        return makeEngineLoop();
    case EngineLoopKind::Wind:
        return makeWindLoop();
    }
    return makeMono(1); // unreachable; a valid buffer keeps callers safe
}

EngineToneParams engineTone(float throttle01, float airspeedMps, bool afterburner) noexcept {
    const float t = std::clamp(throttle01, 0.f, 1.f);
    const float ram = std::clamp(airspeedMps / kEngineRamPitchFullMps, 0.f, 1.f);
    float pitch = kEngineBasePitch + kEnginePitchPerThrottle * t + kEngineRamPitchBoost * ram +
                  (afterburner ? kEngineAbPitchBoost : 0.f);
    pitch = std::clamp(pitch, kEngineMinPitch, kEngineMaxPitch);
    const float gain =
        kEngineIdleGain + (kEngineMilGain - kEngineIdleGain) * t + (afterburner ? kEngineAbGainBoost : 0.f);
    return {pitch, gain};
}

float windRushGain(float airspeedMps) noexcept {
    const float x = std::clamp(airspeedMps / kWindFullMps, 0.f, 1.f);
    return x * x * kWindMaxGain; // ∝ dynamic pressure
}

float windRushPitch(float airspeedMps) noexcept {
    const float a = std::max(airspeedMps, 0.f);
    return std::clamp(kWindBasePitch + kWindPitchPerMps * a, kEngineMinPitch, kEngineMaxPitch);
}

} // namespace fl
