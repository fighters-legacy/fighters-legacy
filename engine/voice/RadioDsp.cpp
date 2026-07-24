// SPDX-License-Identifier: GPL-3.0-or-later
#include "voice/RadioDsp.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace fl {
namespace {

// Wang hash — the deterministic noise source used everywhere else in the engine that needs
// byte-stable procedural data (BuiltinBiomes, SfxBuiltinSounds, the turbulence RNG). Never rand(),
// never a clock: two machines must synthesise the identical squelch.
uint32_t wangHash(uint32_t x) noexcept {
    x = (x ^ 61u) ^ (x >> 16);
    x *= 9u;
    x = x ^ (x >> 4);
    x *= 0x27D4EB2Du;
    x = x ^ (x >> 15);
    return x;
}

float noiseFrom(uint32_t& state) noexcept {
    state = wangHash(state + 0x9E3779B9u);
    return static_cast<float>(state >> 8) / 8388608.0f - 1.0f; // [-1, 1)
}

int16_t toPcm(float v) noexcept {
    return static_cast<int16_t>(std::clamp(v, -1.f, 1.f) * 32767.f);
}

// Soft clipper. tanh rather than a hard clamp: a hard clamp adds harsh odd harmonics that read as
// digital distortion, while tanh's knee is what a compressed radio channel actually sounds like.
float softClip(float v) noexcept {
    return std::tanh(v);
}

} // namespace

void RadioFilter::Biquad::reset() noexcept {
    x1 = x2 = y1 = y2 = 0.f;
}

float RadioFilter::Biquad::step(float x) noexcept {
    const float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1;
    x1 = x;
    y2 = y1;
    y1 = y;
    return y;
}

void RadioFilter::configure(const RadioProfile& profile, int sampleRate) noexcept {
    m_profile = profile;
    m_configured = false;
    if (sampleRate <= 0)
        return;

    const float sr = static_cast<float>(sampleRate);
    const float nyquist = sr * 0.5f;
    const float hpHz = std::clamp(profile.highpassHz, 20.f, nyquist * 0.9f);
    const float lpHz = std::clamp(profile.lowpassHz, hpHz + 50.f, nyquist * 0.95f);
    constexpr float kQ = 0.7071f; // Butterworth: maximally flat, no resonant peak at the corner

    // RBJ audio-EQ-cookbook biquads.
    auto design = [&](float hz, bool highpass, Biquad& out) {
        const float w0 = 2.f * std::numbers::pi_v<float> * hz / sr;
        const float cw = std::cos(w0);
        const float sw = std::sin(w0);
        const float alpha = sw / (2.f * kQ);
        const float a0 = 1.f + alpha;
        if (highpass) {
            out.b0 = ((1.f + cw) * 0.5f) / a0;
            out.b1 = (-(1.f + cw)) / a0;
            out.b2 = out.b0;
        } else {
            out.b0 = ((1.f - cw) * 0.5f) / a0;
            out.b1 = (1.f - cw) / a0;
            out.b2 = out.b0;
        }
        out.a1 = (-2.f * cw) / a0;
        out.a2 = (1.f - alpha) / a0;
        out.reset();
    };
    design(hpHz, /*highpass=*/true, m_hp);
    design(lpHz, /*highpass=*/false, m_lp);
    m_noiseState = 0x9E3779B9u;
    m_configured = true;
}

void RadioFilter::reset() noexcept {
    m_hp.reset();
    m_lp.reset();
    m_noiseState = 0x9E3779B9u;
}

void RadioFilter::process(std::span<int16_t> pcm) noexcept {
    if (!m_configured)
        return;
    const float drive = std::max(0.01f, m_profile.drive);
    const float noise = std::clamp(m_profile.noiseLevel, 0.f, 1.f);
    const float gain = std::max(0.f, m_profile.outputGain);
    for (int16_t& s : pcm) {
        float v = static_cast<float>(s) / 32768.f;
        v = m_hp.step(v);
        v = m_lp.step(v);
        v = softClip(v * drive);
        if (noise > 0.f)
            v += noiseFrom(m_noiseState) * noise;
        s = toPcm(v * gain);
    }
}

std::vector<int16_t> radioClickPcm(int sampleRate) {
    if (sampleRate <= 0)
        return {};
    // 15 ms: long enough to be heard as a key-down, short enough never to mask the first syllable.
    const int n = sampleRate * 15 / 1000;
    std::vector<int16_t> out(static_cast<std::size_t>(n));
    uint32_t rng = 0x51ED2701u;
    for (int i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(n);
        // A sharp attack with a fast exponential decay — a relay contact, not a beep.
        const float env = std::exp(-14.f * t);
        const float tone =
            std::sin(2.f * std::numbers::pi_v<float> * 1800.f * static_cast<float>(i) / static_cast<float>(sampleRate));
        const float body = 0.6f * tone + 0.4f * noiseFrom(rng);
        out[static_cast<std::size_t>(i)] = toPcm(body * env * 0.35f);
    }
    return out;
}

std::vector<int16_t> radioSquelchPcm(int sampleRate) {
    if (sampleRate <= 0)
        return {};
    // 120 ms: the click of the key releasing, then the carrier dropping out into a brief hiss.
    const int n = sampleRate * 120 / 1000;
    std::vector<int16_t> out(static_cast<std::size_t>(n));
    uint32_t rng = 0x1B873593u;
    const int clickN = sampleRate * 10 / 1000;
    for (int i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(sampleRate);
        float v = 0.f;
        if (i < clickN) {
            const float env = std::exp(-18.f * static_cast<float>(i) / static_cast<float>(clickN));
            v += 0.30f * env * noiseFrom(rng);
        }
        // The hiss swells as the carrier drops, then decays — the classic squelch "kshh".
        const float hissEnv = std::exp(-22.f * std::max(0.f, t - 0.008f));
        v += 0.18f * hissEnv * noiseFrom(rng);
        out[static_cast<std::size_t>(i)] = toPcm(v);
    }
    return out;
}

} // namespace fl
