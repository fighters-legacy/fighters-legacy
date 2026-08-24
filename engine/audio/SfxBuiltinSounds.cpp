// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/SfxBuiltinSounds.h"

#include "audio/PcmSynth.h" // toI16 / noise / makeMonoPcm / uploadPcm (#1265)

#include <cmath>
#include <cstdint>

namespace fl {

namespace {

DecodedPcm make(int durationMs) {
    return makeMonoPcm(kSfxSampleRate,
                       static_cast<std::size_t>(kSfxSampleRate) * static_cast<std::size_t>(durationMs) / 1000u);
}

// A low decaying sine — the "body" of a clunk or a rumble.
float tone(uint32_t i, float freqHz, float decayPerSample) {
    const float t = static_cast<float>(i) / static_cast<float>(kSfxSampleRate);
    const float env = std::exp(-static_cast<float>(i) * decayPerSample);
    return std::sin(2.f * 3.14159265f * freqHz * t) * env;
}

} // namespace

DecodedPcm generateBuiltinSfx(SfxKind kind) {
    switch (kind) {
    case SfxKind::Gunfire: {
        DecodedPcm p = make(90);
        const std::size_t n = p.samples.size();
        for (std::size_t i = 0; i < n; ++i) {
            const float env = std::exp(-static_cast<float>(i) * 0.0008f); // fast decay
            p.samples[i] = toI16(0.7f * noise(static_cast<uint32_t>(i)) * env);
        }
        return p;
    }
    case SfxKind::Launch: {
        DecodedPcm p = make(600);
        const std::size_t n = p.samples.size();
        for (std::size_t i = 0; i < n; ++i) {
            // A whoosh: band-limited-ish noise (a running average smooths the hiss) that swells then fades.
            const float frac = static_cast<float>(i) / static_cast<float>(n);
            const float swell = std::sin(3.14159265f * frac); // 0 → 1 → 0
            const float smooth = 0.5f * noise(static_cast<uint32_t>(i)) + 0.5f * noise(static_cast<uint32_t>(i) / 2u);
            p.samples[i] = toI16(0.5f * smooth * swell);
        }
        return p;
    }
    case SfxKind::Release: {
        DecodedPcm p = make(180);
        const std::size_t n = p.samples.size();
        for (std::size_t i = 0; i < n; ++i) {
            const uint32_t u = static_cast<uint32_t>(i);
            const float click = (i < 300) ? 0.5f * noise(u) * std::exp(-static_cast<float>(i) * 0.02f) : 0.f;
            const float thud = 0.6f * tone(u, 110.f, 0.0006f); // a low mechanical thunk
            p.samples[i] = toI16(click + thud);
        }
        return p;
    }
    case SfxKind::Impact: {
        DecodedPcm p = make(120);
        const std::size_t n = p.samples.size();
        for (std::size_t i = 0; i < n; ++i) {
            const float env = std::exp(-static_cast<float>(i) * 0.0015f); // sharp metallic tick
            const float metal =
                0.6f * noise(static_cast<uint32_t>(i)) + 0.4f * tone(static_cast<uint32_t>(i), 1800.f, 0.001f);
            p.samples[i] = toI16(metal * env);
        }
        return p;
    }
    case SfxKind::Explosion: {
        DecodedPcm p = make(900);
        const std::size_t n = p.samples.size();
        for (std::size_t i = 0; i < n; ++i) {
            const uint32_t u = static_cast<uint32_t>(i);
            const float crack = 0.7f * noise(u) * std::exp(-static_cast<float>(i) * 0.0006f);
            const float rumble = 0.5f * tone(u, 55.f, 0.00025f); // a long low boom under the noise
            p.samples[i] = toI16(crack + rumble);
        }
        return p;
    }
    }
    return make(1); // unreachable; a valid one-sample buffer keeps callers safe
}

} // namespace fl
