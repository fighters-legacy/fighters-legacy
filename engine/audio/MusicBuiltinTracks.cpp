// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/MusicBuiltinTracks.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace fl {

namespace {

constexpr float kPi = 3.14159265358979f;

int16_t toI16(float v) {
    if (v > 1.f)
        v = 1.f;
    if (v < -1.f)
        v = -1.f;
    return static_cast<int16_t>(v * 32767.f);
}

// Add a plucked/pad note: a fundamental + a soft second harmonic under a smooth attack/release
// envelope, summed into the mix buffer. Deterministic float math only.
void addNote(std::vector<float>& mix, double startS, double durS, float freqHz, float amp) {
    const int sr = kMusicSampleRate;
    const std::size_t s0 = static_cast<std::size_t>(startS * sr);
    const std::size_t n = static_cast<std::size_t>(durS * sr);
    const float atk = 0.08f * static_cast<float>(n); // samples
    const float rel = 0.35f * static_cast<float>(n);
    for (std::size_t i = 0; i < n && (s0 + i) < mix.size(); ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(sr);
        float env = 1.f;
        if (static_cast<float>(i) < atk)
            env = static_cast<float>(i) / atk;
        else if (static_cast<float>(n - i) < rel)
            env = static_cast<float>(n - i) / rel;
        const float w = 2.f * kPi * freqHz * t;
        const float s = std::sin(w) + 0.28f * std::sin(2.f * w) + 0.12f * std::sin(3.f * w);
        mix[s0 + i] += amp * env * s;
    }
}

// A soft kick-drum-ish pulse (a fast-decaying low sine) for the combat groove.
void addPulse(std::vector<float>& mix, double startS, float amp) {
    const int sr = kMusicSampleRate;
    const std::size_t s0 = static_cast<std::size_t>(startS * sr);
    const std::size_t n = static_cast<std::size_t>(0.14 * sr);
    for (std::size_t i = 0; i < n && (s0 + i) < mix.size(); ++i) {
        const float decay = std::exp(-18.f * static_cast<float>(i) / static_cast<float>(sr));
        const float freq = 80.f - 30.f * (static_cast<float>(i) / static_cast<float>(n)); // pitch drop
        mix[s0 + i] += amp * decay * std::sin(2.f * kPi * freq * static_cast<float>(i) / static_cast<float>(sr));
    }
}

DecodedPcm finalize(std::vector<float>& mix) {
    // Normalise to a comfortable headroom, then quantise. Deterministic.
    float peak = 1e-6f;
    for (float v : mix)
        peak = std::max(peak, std::fabs(v));
    const float g = 0.85f / peak;
    DecodedPcm pcm;
    pcm.sampleRate = kMusicSampleRate;
    pcm.channels = 1;
    pcm.samples.resize(mix.size());
    for (std::size_t i = 0; i < mix.size(); ++i)
        pcm.samples[i] = toI16(mix[i] * g);
    return pcm;
}

} // namespace

DecodedPcm generateBuiltinMusic(MusicMood mood) {
    // Note frequencies (Hz). A minor pentatonic-ish palette keeps the loops consonant.
    constexpr float A2 = 110.0f, C3 = 130.81f, D3 = 146.83f, E3 = 164.81f, G3 = 196.0f;
    constexpr float C4 = 261.63f, D4 = 293.66f, E4 = 329.63f, G4 = 392.0f;

    switch (mood) {
    case MusicMood::Menu: {
        // 8 s slow major-lean pad, two chords drifting.
        std::vector<float> mix(static_cast<std::size_t>(8.0 * kMusicSampleRate), 0.f);
        const std::array<std::array<float, 3>, 2> chords{{{A2, E3, C4}, {G3, D4, G4}}};
        for (int bar = 0; bar < 2; ++bar) {
            const double t0 = bar * 4.0;
            for (float f : chords[bar])
                addNote(mix, t0, 4.0, f, 0.5f);
        }
        return finalize(mix);
    }
    case MusicMood::Patrol: {
        // 8 s mid-tempo arpeggio over a held bass — a steady cruise feel.
        std::vector<float> mix(static_cast<std::size_t>(8.0 * kMusicSampleRate), 0.f);
        const std::array<float, 8> arp{C4, E4, G4, E4, D4, G4, E4, C4};
        for (int bar = 0; bar < 2; ++bar) {
            const double t0 = bar * 4.0;
            addNote(mix, t0, 4.0, bar == 0 ? A2 : G3, 0.5f); // bass
            for (int i = 0; i < 8; ++i)
                addNote(mix, t0 + i * 0.5, 0.5, arp[static_cast<std::size_t>(i)], 0.4f);
        }
        return finalize(mix);
    }
    case MusicMood::Combat: {
        // 6 s driving pulse: a fast bass ostinato + a rhythmic kick + tense stabs.
        std::vector<float> mix(static_cast<std::size_t>(6.0 * kMusicSampleRate), 0.f);
        const std::array<float, 4> bass{A2, A2, C3, D3};
        for (int beat = 0; beat < 24; ++beat) { // 6 s at 4 beats/s
            const double t = beat * 0.25;
            addPulse(mix, t, 0.9f);
            addNote(mix, t, 0.25, bass[static_cast<std::size_t>(beat) % 4], 0.5f);
            if (beat % 4 == 2)
                addNote(mix, t, 0.5, E4, 0.35f); // off-beat stab
        }
        return finalize(mix);
    }
    }
    return generateBuiltinMusic(MusicMood::Menu);
}

DecodedPcm builtinMusicTrack(std::string_view assetName) {
    if (assetName == "builtin:music-menu")
        return generateBuiltinMusic(MusicMood::Menu);
    if (assetName == "builtin:music-patrol")
        return generateBuiltinMusic(MusicMood::Patrol);
    if (assetName == "builtin:music-combat")
        return generateBuiltinMusic(MusicMood::Combat);
    return {};
}

std::span<const std::string_view> builtinMusicTrackNames() noexcept {
    static constexpr std::string_view kNames[] = {"builtin:music-menu", "builtin:music-patrol", "builtin:music-combat"};
    return kNames;
}

PlaylistData builtinDefaultPlaylist() {
    PlaylistData p;
    p.crossfadeDuration = 3.0f;
    auto state = [](const char* id, const char* track, bool loop) {
        PlaylistState s;
        s.id = id;
        s.tracks = {track};
        s.loop = loop;
        return s;
    };
    // State ids match MusicManager's gameStateName().
    p.states.push_back(state("Menu", "builtin:music-menu", true));
    p.states.push_back(state("FlightPatrol", "builtin:music-patrol", true));
    p.states.push_back(state("FlightCombat", "builtin:music-combat", true));
    p.states.push_back(state("MissionSuccess", "builtin:music-patrol", false));
    p.states.push_back(state("Debrief", "builtin:music-menu", true));
    return p;
}

} // namespace fl
