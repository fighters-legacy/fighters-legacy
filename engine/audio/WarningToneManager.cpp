// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/WarningToneManager.h"

#include "ILogger.h"

#include <cmath>
#include <cstdint>

namespace fl {

namespace {

int16_t toI16(float v) {
    if (v > 1.f)
        v = 1.f;
    if (v < -1.f)
        v = -1.f;
    return static_cast<int16_t>(v * 32767.f);
}

DecodedPcm makeMono(std::size_t sampleCount) {
    DecodedPcm pcm;
    pcm.sampleRate = kWarningToneSampleRate;
    pcm.channels = 1;
    pcm.samples.resize(sampleCount);
    return pcm;
}

} // namespace

DecodedPcm generateWarningTonePcm(WarningTone tone) {
    constexpr float kPi = 3.14159265f;
    const float sr = static_cast<float>(kWarningToneSampleRate);

    switch (tone) {
    case WarningTone::Stall: {
        // Intermittent horn: a 700 Hz tone gated on for 0.15 s then silent for 0.10 s. The buffer is
        // exactly four 0.25 s gate cycles and ENDS in the silent phase, so the loop seam is silence.
        constexpr std::size_t kCycle = 5512; // 0.25 s at 22050 Hz
        constexpr std::size_t kCycles = 4;
        constexpr std::size_t kOnSamples = 3307; // ~0.15 s
        DecodedPcm p = makeMono(kCycle * kCycles);
        const std::size_t n = p.samples.size();
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t phase = i % kCycle;
            if (phase >= kOnSamples) {
                p.samples[i] = 0; // gated-off — silence
                continue;
            }
            const float t = static_cast<float>(i) / sr;
            // A short attack/decay envelope inside each beep keeps clicks off the gate edges.
            const float local = static_cast<float>(phase) / static_cast<float>(kOnSamples);
            const float env = std::sin(kPi * local); // 0 -> 1 -> 0 across the beep
            p.samples[i] = toI16(0.6f * std::sin(2.f * kPi * 700.f * t) * env);
        }
        return p;
    }
    case WarningTone::Overspeed: {
        // Steady clacker: a harsh 1050 Hz tone (1050 divides 22050 exactly -> 21 samples/cycle, so an
        // integer number of cycles loops with no seam). Mild hard-clip gives it an edge over a pure sine.
        constexpr std::size_t kCycleSamples = 21; // 1050 Hz
        constexpr std::size_t kCycles = 525;      // 0.5 s
        DecodedPcm p = makeMono(kCycleSamples * kCycles);
        const std::size_t n = p.samples.size();
        for (std::size_t i = 0; i < n; ++i) {
            const float t = static_cast<float>(i) / sr;
            const float s = std::sin(2.f * kPi * 1050.f * t);
            p.samples[i] = toI16(0.55f * (s * 1.6f)); // overdrive then clip in toI16 -> square-ish edge
        }
        return p;
    }
    }
    return makeMono(1); // unreachable; a valid one-sample buffer keeps callers safe
}

bool WarningToneManager::init(IAudio* audio, ILogger* logger) {
    m_audio = audio;
    m_logger = logger;
    // Sources are created lazily on first activation so an aircraft that never stalls holds no voices.
    return true;
}

void WarningToneManager::shutdown() {
    if (m_audio) {
        for (Channel* ch : {&m_stall, &m_overspeed}) {
            if (ch->src) {
                m_audio->stop(ch->src);
                m_audio->destroySource(ch->src);
            }
            if (ch->buf)
                m_audio->freeBuffer(ch->buf);
        }
    }
    m_stall = Channel{};
    m_overspeed = Channel{};
    m_audio = nullptr;
    m_logger = nullptr;
}

void WarningToneManager::driveChannel(Channel& ch, WarningTone tone, bool want, float gain, float dt) {
    // Hysteresis: a live predicate refreshes the hold; a cleared one bleeds it down over kHoldSeconds.
    bool effective;
    if (want) {
        ch.holdTimer = kHoldSeconds;
        effective = true;
    } else {
        ch.holdTimer -= dt;
        if (ch.holdTimer < 0.f)
            ch.holdTimer = 0.f;
        effective = ch.holdTimer > 0.f;
    }

    if (effective) {
        if (!ch.active) {
            // Lazily create the looping head-locked source + upload the tone buffer on first use.
            if (ch.src == 0) {
                ch.src = m_audio->createSource();
                const DecodedPcm pcm = generateWarningTonePcm(tone);
                ch.buf = m_audio->uploadBuffer(pcm.samples.data(), pcm.samples.size() * sizeof(int16_t), pcm.sampleRate,
                                               pcm.channels);
                m_audio->setSourceRelative(ch.src, true); // head-locked: not spatialised
                m_audio->setPosition(ch.src, 0.f, 0.f, 0.f);
                m_audio->setRolloffFactor(ch.src, 0.f);
                m_audio->setLooping(ch.src, true);
            }
            m_audio->setGain(ch.src, gain);
            m_audio->play(ch.src, ch.buf);
            ch.active = true;
        } else {
            m_audio->setGain(ch.src, gain); // live-track the volume slider
        }
    } else if (ch.active) {
        m_audio->stop(ch.src);
        ch.active = false;
    }
}

void WarningToneManager::update(const WarningToneInputs& in, const AudioSettings& settings, float dt) {
    if (!m_audio)
        return;

    // Leaving flight silences everything immediately — bypass the hold so a tone never lingers into a
    // menu or a spectator view.
    if (!in.inFlight) {
        for (Channel* ch : {&m_stall, &m_overspeed}) {
            ch->holdTimer = 0.f;
            if (ch->active) {
                m_audio->stop(ch->src);
                ch->active = false;
            }
        }
        return;
    }

    const float gain = settings.masterVolume * settings.sfxVolume;
    driveChannel(m_stall, WarningTone::Stall, in.stall, gain, dt);
    driveChannel(m_overspeed, WarningTone::Overspeed, in.overspeed, gain, dt);
}

} // namespace fl
