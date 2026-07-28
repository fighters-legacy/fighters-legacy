// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "voice/RadioNet.h"
#include "voice/VoiceActivity.h"

#include <array>
#include <string>

namespace fl {

// Client-side voice configuration (Epic J). Persisted as [voice] in user.toml.
//
// The per-net volumes are indexed by NET INDEX, not by net id, because the wire already addresses
// nets by index and a player's "turn the proximity net down" should survive an operator renaming
// the net's display label. A server whose table is shorter than the array simply leaves the tail
// unused; a longer one is impossible (kMaxRadioNets bounds both).
struct VoiceSettings {
    // Path to a whisper.cpp model for the deterministic voice-command tier (#935). Empty = no voice
    // commands; the radio menu is the path. The project ships no model — it is an operator/player
    // choice of size vs accuracy, and bundling one would add hundreds of megabytes to every install
    // for a feature most players will not use.
    std::string sttModelPath;
    bool enabled{true};         // master switch; false = neither transmit nor receive
    bool transmitEnabled{true}; // false = listen-only (no capture device opened at all)

    std::string inputDevice; // capture device NAME; empty = system default (see IAudioCapture)

    VoiceKeyMode keyMode{VoiceKeyMode::PushToTalk};
    float voxThreshold{VoiceActivityGate::kDefaultThreshold}; // linear RMS, [0, 1]
    float micGain{1.0f};                                      // pre-encode input trim, [0, 4]

    int bitrate{24000};        // encoder target bits/s, clamped [6000, 128000]
    int jitterTargetFrames{3}; // playback de-jitter depth in 20 ms frames, [1, 12]

    bool radioEffect{true};     // apply the radio DSP the server's net profile asks for (#925)
    bool subtitles{true};       // show a "who is transmitting" line on the radio subtitle path (#925)
    float duckingAmount{0.55f}; // how far music/SFX duck while a net is live, [0, 1]; 0 = no ducking

    // Per-net receive volume in [0, 2]; 1 = the net's own authored gain, 0 = muted.
    std::array<float, kMaxRadioNets> netVolume{{1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f}};
};

} // namespace fl
